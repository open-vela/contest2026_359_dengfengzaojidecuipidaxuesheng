#!/usr/bin/env python3
"""Build and run with real LVGL and an existing prepared host NetSurf core."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess
import struct
import zlib
import json
import hashlib
import os
from openvela_common import prepare_png

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--core', type=Path, required=True)
p.add_argument('--lvgl', type=Path, required=True)
p.add_argument('--quickjs', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--webjs', type=Path, help='namespaced webpage-engine build directory')
p.add_argument('--html5test-replay', type=Path, help='optional unmodified site cache for host diagnostics; not live-board acceptance')
p.add_argument('--png', type=Path, help='libpng sources when using an isolated core workarea')
p.add_argument('--core-objects', type=Path, help='unchanged prepared host obj/ tree')
p.add_argument('--core-overrides', type=Path, help='rebuilt core objects from the isolated patched sources')
p.add_argument('--platform-source', type=Path, help='isolated, complete platform source snapshot')
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
platform = a.platform_source.resolve() if a.platform_source else root / 'platform/openvela'
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
ns = a.core / 'deps/netsurf-modern'
png = prepare_png(a.png or a.core.parent / 'external/libpng/libpng', out, a.core / 'deps/zlib')
apps = a.lvgl.parents[2]
flags = ['-std=gnu11', '-O1', '-g', '-ffunction-sections', '-fdata-sections', '-D_GNU_SOURCE',
         '-DLV_CONF_PATH=' + str(root / 'tests/openvela/lv_conf.h')]
if a.webjs: flags+=['-DOV_ENABLE_WEB_JS=1']
incs = [root / 'tests/openvela/host', platform, a.core / 'port-compat', a.core / 'build/gen',
        a.lvgl.parent, a.quickjs, ns / 'netsurf', ns / 'netsurf/include', ns / 'netsurf/frontends',
        ns / 'freetype/include', png, a.core / 'deps/zlib', apps / 'system/desktop', apps / 'include']
incs += [ns / lib / 'include' for lib in ['libcss', 'libdom', 'libwapcaplet', 'libparserutils', 'libhubbub']]
for inc in incs:
    flags += ['-I', str(inc)]
def compile_one(src):
    obj = out / (str(src).replace('/', '_') + '.o')
    extra = ['-DCONFIG_VERSION="host-test"', '-DCONFIG_BIGNUM',
             '-DOV_NETSURF_FONT_PATH="/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"']
    command=['gcc', *flags, *extra, '-Dnsmonkey', '-DWITH_NS_SVG', '-c', str(src), '-o', str(obj)]
    digest=hashlib.sha256(src.read_bytes()+json.dumps(command).encode()).hexdigest()
    if src.parent in [platform,root/'tests/openvela']:
        digest+=str(max(h.stat().st_mtime_ns for h in platform.glob('*.h')))
    stamp=obj.with_suffix('.digest')
    if not obj.exists() or not stamp.exists() or stamp.read_text()!=digest:
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        stamp.write_text(digest)
    return obj
sources = list((a.lvgl / 'src').rglob('*.c'))
sources += [src for src in platform.glob('*.c')
            if src.stem not in ['openvela_qpk_netsurf', 'openvela_desktop', 'openvela_http_nuttx', 'openvela_platform_test']]
sources += [root / 'tests/openvela/integration.c']
sources += [s for s in png.glob('png*.c') if s.stem != 'pngtest']
sources += [apps / 'system/desktop/qpk_tjpgd.c']
print('Building', len(sources), 'host units', flush=True)
with ThreadPoolExecutor(max_workers=8) as pool:
    objects = list(pool.map(compile_one, sources))
core_objects = a.core_objects or a.core / 'build-port/obj'
objects += [o for o in core_objects.rglob('*.o') if o.parent.name != 'app']
objects += [core_objects / 'app' / name for name in
            ['port_nsp_charset.c.o', 'port_nsp_libc.c.o', 'port_nsp_js_none.c.o']]
if a.core_overrides:
    replacements = {o.name: o for o in a.core_overrides.rglob('*.o')}
    missing = replacements.keys() - {o.name for o in objects}
    if missing: raise RuntimeError('unknown core replacement objects: ' + str(sorted(missing)))
    objects = [replacements.get(o.name, o) for o in objects]
if a.webjs:
    objects=[o for o in objects if 'javascript_none_none.' not in o.name and 'nsp_js_none.' not in o.name]
    objects += [Path(o) for o in json.loads((a.webjs/'manifest.json').read_text())['objects']]
rsp = out / 'objects.rsp'
rsp.write_text('\n'.join(str(o) for o in objects))
subprocess.run(['gcc', '@' + str(rsp), '-o', str(out / 'integration'), '-Wl,--gc-sections', '-lm', '-pthread'], check=True)
fixtures = out / 'image-fixtures'
fixtures.mkdir(exist_ok=True)
def chunk(name, data):
    return struct.pack('!I', len(data)) + name + data + struct.pack('!I', zlib.crc32(name + data))
def make_png(name, width, height, colour, pixels, extra=b''):
    (fixtures / name).write_bytes(b'\x89PNG\r\n\x1a\n' +
        chunk(b'IHDR', struct.pack('!IIBBBBB', width, height, 8, colour, 0, 0, 0)) + extra +
        chunk(b'IDAT', zlib.compress(pixels)) + chunk(b'IEND', b''))
make_png('blue.png', 2, 2, 6, (b'\x00' + b'\x00\x00\xff\xff' * 2) * 2)
make_png('alpha.png', 2, 2, 6, (b'\x00' + b'\x00\x00\xff\x80' * 2) * 2)
make_png('palette.png', 2, 2, 3, b'\x00\x00\x00' * 2, chunk(b'PLTE', b'\x00\x00\xff'))
make_png('oversize.png', 100000, 100000, 6, b'')
subprocess.run(['ffmpeg', '-loglevel', 'error', '-f', 'lavfi', '-i', 'color=c=red:s=16x16',
    '-frames:v', '1', '-threads', '1', '-c:v', 'mjpeg', '-q:v', '2', '-y', str(fixtures / 'red.jpg')], check=True)
subprocess.run([str(out / 'integration'), str(fixtures)], check=True, timeout=90)
if a.html5test_replay:
    if not a.webjs:p.error('site replay requires the real webpage VM')
    cache=a.html5test_replay.resolve()
    for row in json.loads((cache/'manifest.json').read_text()):
        file=cache/row['file']
        if file.parent!=cache or hashlib.sha256(file.read_bytes()).hexdigest()!=row['sha256']:
            raise RuntimeError('site replay input differs from its download manifest')
    subprocess.run([str(out/'integration'),str(fixtures)],check=True,timeout=90,
                   env={**os.environ,'OV_HTML5TEST_REPLAY':str(cache)})
native = [platform / 'openvela_desktop.c', platform / 'openvela_qpk_netsurf.c',
          root / 'tests/openvela/desktop.c']
native += [a.quickjs / name for name in ['quickjs.c', 'cutils.c', 'libregexp.c', 'libunicode.c', 'libbf.c']]
with ThreadPoolExecutor(max_workers=4) as pool:
    native_objects = list(pool.map(compile_one, native))
native_objects += [obj for obj in objects if 'integration.c.o' not in obj.name]
rsp.write_text('\n'.join(str(o) for o in native_objects))
subprocess.run(['gcc', '@' + str(rsp), '-o', str(out / 'desktop'), '-Wl,--gc-sections', '-lm', '-pthread', '-ldl'], check=True)
subprocess.run([str(out / 'desktop')], check=True, timeout=90)
