#!/usr/bin/env python3
"""Compile the frontend against an existing prepared NetSurf and NuttX tree.

This only writes --output. It never changes the prepared core or firmware tree.
"""
import argparse
import json
from pathlib import Path
import subprocess
from openvela_common import prepare_png

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--core', type=Path, required=True)
p.add_argument('--target', type=Path, required=True, help='parent of nuttx/ and apps/')
p.add_argument('--cc', required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--png', type=Path, help='existing libpng source with pnglibconf.h')
p.add_argument('--webjs', action='store_true', help='enable the separately built webpage VM')
p.add_argument('--network-trace',action='store_true',help='log host/stage/timing only, never request paths or bodies')
p.add_argument('--platform-source', type=Path, help='isolated, complete platform source snapshot')
a = p.parse_args()
source = Path(__file__).resolve().parents[1]
platform = a.platform_source.resolve() if a.platform_source else source / 'platform/openvela'
core = a.core.resolve()
target = a.target.resolve()
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
ns = core / 'deps/netsurf-modern'
png = prepare_png(a.png or target.parent / 'external/libpng/libpng', out, core / 'deps/zlib')
includes = [platform, core / 'port-compat',
            ns / 'netsurf', ns / 'netsurf/include', ns / 'netsurf/frontends',
            ns / 'netsurf/content/handlers', core / 'build/gen',
            ns / 'freetype/include', core / 'deps/zlib',
            target / 'apps/graphics/lvgl', target / 'apps/include',
            target / 'apps/interpreters/quickjs/quickjs',
            target / 'apps/system/desktop', png]
includes += [ns / lib / 'include' for lib in
             ['libcss', 'libdom', 'libparserutils', 'libhubbub', 'libwapcaplet', 'libnsutils']]
flags = ['-std=gnu11', '-O2', '-ffunction-sections', '-fdata-sections',
         '-Wall', '-Wextra', '-Werror=implicit-function-declaration',
         '-Werror=incompatible-pointer-types', '-Dnsmonkey', '-DWITH_NS_SVG',
         '-DSTMTEXPR=1', '-D_GNU_SOURCE', '-D__NuttX__',
         '-march=rv32imafc_zicsr_zifencei', '-mabi=ilp32f',
         '-isystem', str(target / 'nuttx/include'), '-include', 'nuttx/config.h',
         '-include', str(core / 'port-compat/nsw_libc.h'),
         '-include', str(core / 'port-compat/nsw_config.h')]
for inc in includes:
    flags += ['-I', str(inc)]
if a.webjs: flags+=['-DOV_ENABLE_WEB_JS=1']
if a.network_trace:flags+=['-DOV_HTTP_NETWORK_TRACE=1']
commands = []
failed = False
sources = sorted(platform.glob('*.c'))
sources += sorted(png.glob('png*.c'))
for src in sources:
    if src.name.endswith('_test.c'):
        continue
    if src.parent == png and src.stem in ['pngtest']: continue
    name = ('libpng_' if src.parent == png else '') + src.stem
    cmd = [a.cc, *flags, '-DPNG_ARM_NEON_OPT=0', '-DPNG_POWERPC_VSX_OPT=0', '-DPNG_INTEL_SSE_OPT=0',
           '-c', str(src), '-o', str(out / (name + '.o'))]
    commands.append(cmd)
    print('CC', src.name, flush=True)
    result = subprocess.run(cmd, text=True, capture_output=True)
    (out / (src.stem + '.log')).write_text(result.stdout + result.stderr)
    print(result.stdout + result.stderr, end='', flush=True)
    failed |= result.returncode != 0
(out / 'commands.json').write_text(json.dumps(commands, indent=2))
raise SystemExit(int(failed))
