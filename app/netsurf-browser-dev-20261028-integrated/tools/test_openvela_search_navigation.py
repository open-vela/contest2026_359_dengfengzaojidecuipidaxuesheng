#!/usr/bin/env python3
"""Run many-resource navigation against the real prepared core and VM."""
import argparse,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--core',type=Path,required=True)
p.add_argument('--lvgl',type=Path,required=True)
p.add_argument('--host-build',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--expect-queue-bug',action='store_true')
p.add_argument('--rebuild-http',action='store_true')
a=p.parse_args();root=Path(__file__).resolve().parents[1]
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
ns=a.core.resolve()/'deps/netsurf-modern'
incs=[root/'tests/openvela',root/'tests/openvela/host',root/'platform/openvela',
    a.core/'port-compat',a.core/'build/gen',a.lvgl.parent,
    a.lvgl.parents[2]/'interpreters/quickjs/quickjs',ns/'netsurf',ns/'netsurf/include',
    ns/'netsurf/frontends',ns/'netsurf/content/handlers',ns/'freetype/include']
incs += [d/'include' for d in ns.iterdir() if (d/'include').is_dir()]
flags=['-std=gnu11','-O1','-g','-ffunction-sections','-fdata-sections','-D_GNU_SOURCE',
    '-Dnsmonkey','-DWITH_NS_SVG','-DOV_ENABLE_WEB_JS=1','-DLV_CONF_PATH='+str(root/'tests/openvela/lv_conf.h')]
for inc in incs:flags+=['-I',str(inc.resolve())]
skip=('_platform_openvela_openvela_desktop.c.o','_platform_openvela_openvela_qpk_netsurf.c.o',
    '_tests_openvela_desktop.c.o','_tests_openvela_integration.c.o','_apps_interpreters_quickjs_quickjs_')
objects=[s for s in (a.host_build/'objects.rsp').read_text().splitlines() if not any(x in s for x in skip)]
sources=[root/'tests/openvela/search_navigation.c']
if a.rebuild_http:
    objects=[s for s in objects if not s.endswith('_platform_openvela_openvela_http.c.o')]
    sources+=[root/'platform/openvela/openvela_http.c']
for source in sources:
    obj=out/(source.name+'.o')
    subprocess.run(['gcc',*flags,'-c',str(source),'-o',str(obj)],check=True);objects.append(str(obj))
rsp=out/'objects.rsp';rsp.write_text('\n'.join(objects))
exe=out/'search-navigation'
subprocess.run(['gcc','@'+str(rsp),'-o',str(exe),'-Wl,--gc-sections','-lm','-pthread'],check=True)
env=dict(os.environ);env.pop('OV_EXPECT_SEARCH_QUEUE_BUG',None)
if a.expect_queue_bug:env['OV_EXPECT_SEARCH_QUEUE_BUG']='1'
r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=15,env=env)
(out/'verification.log').write_text(r.stdout+r.stderr)
print(r.stdout+r.stderr,end='');r.check_returncode()
