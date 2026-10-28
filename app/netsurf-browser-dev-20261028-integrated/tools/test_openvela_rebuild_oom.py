#!/usr/bin/env python3
"""Inject box allocation failures into a real host core's layout rebuild.

Reuse an already validated test_openvela.py build, replace only the four
patched core objects, and link an independent executable. No board access.
"""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--core', type=Path, required=True)
p.add_argument('--lvgl', type=Path, required=True)
p.add_argument('--host-build', type=Path, required=True)
p.add_argument('--source-root', type=Path, required=True,
               help='patched source root containing netsurf/content/handlers/html')
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
ns = a.core.resolve() / 'deps/netsurf-modern'
incs = [root/'tests/openvela/host', root/'platform/openvela',
        a.core/'port-compat', a.core/'build/gen', a.lvgl.parent,
        a.lvgl.parents[2]/'interpreters/quickjs/quickjs',
        ns/'netsurf', ns/'netsurf/include', ns/'netsurf/frontends',
        ns/'netsurf/content/handlers',
        ns/'freetype/include']
incs += [d/'include' for d in ns.iterdir() if (d/'include').is_dir()]
flags = ['-std=gnu11', '-O1', '-g', '-ffunction-sections', '-fdata-sections',
         '-D_GNU_SOURCE', '-Dnsmonkey', '-DWITH_NS_SVG', '-DOV_ENABLE_WEB_JS=1',
         '-DLV_CONF_PATH='+str(root/'tests/openvela/lv_conf.h')]
for inc in incs:
    flags += ['-I', str(inc.resolve())]
names = ['rebuild.c', 'box_manipulate.c', 'box_special.c', 'box_construct.c']
objects = [Path(line) for line in (a.host_build/'objects.rsp').read_text().splitlines() if line]
objects = [o for o in objects if not any(o.name.endswith('_html_'+name+'.o') for name in names)
           and not o.name.endswith('_tests_openvela_desktop.c.o')
           and not o.name.endswith('_tests_openvela_integration.c.o')]
sources = [a.source_root/'netsurf/content/handlers/html'/name for name in names]
sources += [root/'tests/openvela/rebuild_oom.c']
for source in sources:
    obj = out/(source.name+'.o')
    subprocess.run(['gcc', *flags, '-c', str(source.resolve()), '-o', str(obj)], check=True)
    objects.append(obj)
rsp = out/'objects.rsp'
rsp.write_text('\n'.join(str(o.resolve()) for o in objects))
exe = out/'rebuild-oom'
subprocess.run(['gcc', '@'+str(rsp), '-o', str(exe), '-Wl,--gc-sections',
                '-Wl,--wrap=talloc_named_const', '-Wl,--wrap=calloc',
                '-Wl,--wrap=form_add_option', '-Wl,--wrap=malloc', '-Wl,--wrap=talloc_free',
                '-lm', '-pthread', '-ldl'], check=True)
result = subprocess.run([str(exe), str((a.host_build/'image-fixtures').resolve())],
                        capture_output=True, text=True, timeout=60)
(out/'verification.log').write_text(result.stdout + result.stderr)
print(result.stdout + result.stderr, end='')
result.check_returncode()
