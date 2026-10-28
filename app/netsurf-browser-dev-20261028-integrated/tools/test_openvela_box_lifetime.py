#!/usr/bin/env python3
"""Check real box ownership, hidden allocations and hide/show rebuilds.

Reuse a matching host build. --expect-bug supplies a negative control using
the old core sources; --sanitize instruments the changed files and the CSS
results/scrollbar destruction sites in addition to the test programs.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--core',type=Path,required=True)
p.add_argument('--lvgl',type=Path,required=True)
p.add_argument('--host-build',type=Path,required=True)
p.add_argument('--source-root',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--expect-bug',action='store_true')
p.add_argument('--sanitize',action='store_true')
a=p.parse_args()
root=Path(__file__).resolve().parents[1]
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
ns=a.core.resolve()/'deps/netsurf-modern'
incs=[root/'tests/openvela',root/'tests/openvela/host',root/'platform/openvela',
    a.core/'port-compat',a.core/'build/gen',a.lvgl.parent,
    a.lvgl.parents[2]/'interpreters/quickjs/quickjs',ns/'netsurf',ns/'netsurf/include',
    ns/'netsurf/frontends',ns/'netsurf/content/handlers',ns/'freetype/include',ns/'libcss/src']
incs += [d/'include' for d in ns.iterdir() if (d/'include').is_dir()]
flags=['-std=gnu11','-O1','-g','-ffunction-sections','-fdata-sections','-D_GNU_SOURCE',
    '-Dnsmonkey','-DWITH_NS_SVG','-DOV_ENABLE_WEB_JS=1','-DLV_CONF_PATH='+str(root/'tests/openvela/lv_conf.h')]
flags += ['-include',str(a.core/'port-compat/nsw_config.h')]
for inc in incs:flags+=['-I',str(inc.resolve())]
san=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie'] if a.sanitize else []
sources=[a.source_root/'netsurf/content/handlers/html'/name for name in ['box_manipulate.c','box_construct.c']]
skip=['_html_box_manipulate.c.o','_html_box_construct.c.o',
    '_platform_openvela_openvela_desktop.c.o','_platform_openvela_openvela_qpk_netsurf.c.o',
    '_tests_openvela_desktop.c.o','_tests_openvela_integration.c.o','_apps_interpreters_quickjs_quickjs_']
if a.sanitize:
    sources += [ns/'libcss/src/select/select.c',ns/'netsurf/desktop/scrollbar.c']
    skip += ['_libcss_src_select_select.c.o','_netsurf_desktop_scrollbar.c.o']
base=[s for s in (a.host_build/'objects.rsp').read_text().splitlines() if not any(x in s for x in skip)]
def compile_source(source):
    obj=out/(source.stem+'.o')
    # LibCSS and NetSurf both have utils/utils.h. Resolve the library's own
    # header first when rebuilding its destruction site for sanitizers.
    local=['-I',str(ns/'libcss/src')] if source == ns/'libcss/src/select/select.c' else []
    subprocess.run(['gcc',*local,*flags,*san,'-c',str(source.resolve()),'-o',str(obj)],check=True)
    return str(obj)
base += [compile_source(source) for source in sources]
results=[]
for name,cases in [('box_lifetime',['styles','scroll','plain','clone']),('hidden_box_memory',[None])]:
    obj=compile_source(root/'tests/openvela'/(name+'.c'))
    rsp=out/(name+'.rsp');rsp.write_text('\n'.join(base+[obj]))
    exe=out/name
    subprocess.run(['gcc',*san,'@'+str(rsp),'-o',str(exe),'-Wl,--gc-sections','-lm','-pthread'],check=True)
    for case in cases:
        env=dict(os.environ);env.pop('OV_EXPECT_RETAINED_HIDDEN',None)
        if a.expect_bug:env['OV_EXPECT_RETAINED_HIDDEN']='1'
        if a.sanitize:env['ASAN_OPTIONS']='detect_leaks=1:halt_on_error=1'
        result=subprocess.run([str(exe)]+([case] if case else []),capture_output=True,text=True,timeout=60,env=env)
        log=out/((case or name)+'.log');log.write_text(result.stdout+result.stderr)
        expected_failure=a.expect_bug and case in ('styles','scroll')
        if expected_failure:
            assert result.returncode!=0 and 'BOX_LIFETIME_CASE '+case in result.stderr,result.stderr
            if a.sanitize:assert 'ERROR: AddressSanitizer' in result.stderr,result.stderr
        elif result.returncode:
            raise RuntimeError(f'{case or name} failed ({result.returncode}):\n'+result.stdout+result.stderr)
        results.append({'case':case or name,'returncode':result.returncode,'expected_failure':expected_failure,
            'log':str(log),'sanitized':a.sanitize})
(out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
print(json.dumps(results,indent=2))
