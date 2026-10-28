#!/usr/bin/env python3
"""Exercise the real VM's checkpoint budget and navigation recovery.

The platform budget clock advances deterministically while the scheduler keeps
real monotonic time. Reuse a matching, already-built host integration object set.
"""
import argparse
import os
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--core', type=Path, required=True)
p.add_argument('--lvgl', type=Path, required=True)
p.add_argument('--host-build', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--expect-reset-bug', action='store_true', help='negative control against the old platform object')
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
ns = a.core.resolve()/'deps/netsurf-modern'
incs = [root/'tests/openvela', root/'tests/openvela/host', root/'platform/openvela',
        a.core/'port-compat', a.core/'build/gen', a.lvgl.parent,
        a.lvgl.parents[2]/'interpreters/quickjs/quickjs', ns/'netsurf', ns/'netsurf/include',
        ns/'netsurf/frontends', ns/'netsurf/content/handlers', ns/'freetype/include']
incs += [d/'include' for d in ns.iterdir() if (d/'include').is_dir()]
flags = ['-std=gnu11', '-O1', '-g', '-ffunction-sections', '-fdata-sections', '-D_GNU_SOURCE',
         '-Dnsmonkey', '-DWITH_NS_SVG', '-DOV_ENABLE_WEB_JS=1',
         '-DLV_CONF_PATH='+str(root/'tests/openvela/lv_conf.h')]
for inc in incs: flags += ['-I', str(inc.resolve())]
obj = out/'microtask_budget.o'
subprocess.run(['gcc', *flags, '-c', str(root/'tests/openvela/microtask_budget.c'), '-o', str(obj)], check=True)
skip = ('_platform_openvela_openvela_desktop.c.o', '_platform_openvela_openvela_qpk_netsurf.c.o',
        '_tests_openvela_desktop.c.o', '_tests_openvela_integration.c.o', '_apps_interpreters_quickjs_quickjs_')
objects = [s for s in (a.host_build/'objects.rsp').read_text().splitlines() if not any(x in s for x in skip)]
if not any(s.endswith('webjs_binding_backend.o') for s in objects):
    p.error('host build must include the real namespaced webpage VM')
objects.append(str(obj))
rsp = out/'objects.rsp'
rsp.write_text('\n'.join(objects))
exe = out/'microtask-budget'
subprocess.run(['gcc','@'+str(rsp),'-o',str(exe),'-Wl,--gc-sections','-lm','-pthread'], check=True)
env = dict(os.environ)
env.pop('OV_EXPECT_MICROTASK_RESET_BUG', None)
if a.expect_reset_bug: env['OV_EXPECT_MICROTASK_RESET_BUG'] = '1'
result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10, env=env)
(out/'verification.log').write_text(result.stdout+result.stderr)
print(result.stdout+result.stderr, end='')
result.check_returncode()
