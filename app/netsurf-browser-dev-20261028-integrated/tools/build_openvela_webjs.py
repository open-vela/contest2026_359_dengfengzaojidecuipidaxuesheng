#!/usr/bin/env python3
"""Build a namespaced webpage QuickJS-ng, separate from the QPK VM.

Only the output directory is written. The prepared NetSurf headers and core
objects must come from the same source revision as its QuickJS bindings.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import subprocess
import zlib

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--core',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--cc',default='gcc')
p.add_argument('--target',type=Path,help='enable RV32/ILP32F with these NuttX headers')
p.add_argument('--trace-exec',action='store_true',help='diagnostic timings for script evaluation, without URLs or source text')
a=p.parse_args()
root=Path(__file__).resolve().parents[1]
out=a.output.resolve(); out.mkdir(parents=True,exist_ok=True)
nsroot=a.core/'deps/netsurf-modern'; ns=nsroot/'netsurf'
ng=nsroot/'quickjs'
flags=['-Os','-g','-std=gnu11','-ffunction-sections','-fdata-sections','-D_GNU_SOURCE',
       '-DNDEBUG','-DSTMTEXPR=1','-Dnsmonkey','-DWITH_NS_SVG',
       '-DBUZZOS_QUICKJS_FETCH_BODY','-include',str(a.core/'port-compat/nsw_libc.h'),
       '-include',str(a.core/'port-compat/nsw_config.h')]
if a.target:
    flags+=['-march=rv32imafc_zicsr_zifencei','-mabi=ilp32f','-D__NuttX__',
            '-isystem',str(a.target/'nuttx/include'),'-include','nuttx/config.h']
incs=[out,root/'platform/openvela',a.core/'port-compat',nsroot,ns,ns/'include',
      ns/'content/handlers',ns/'frontends',a.core/'build/gen',a.core/'deps/zlib',
      nsroot/'libsvgtiny/include',nsroot/'freetype/include']
incs += [nsroot/lib/'include' for lib in ['libdom','libcss','libparserutils','libhubbub','libwapcaplet','libnsutils']]
for inc in incs: flags+=['-I',str(inc)]
# Canvas only needs the existing bounded PNG encoder, not another image library.
(out/'lodepng.h').write_text((root/'platform/openvela/openvela_canvas_png.h').read_text())
engine=[ng/name for name in ['quickjs.c','libregexp.c','libunicode.c','dtoa.c']]
bindings=[ns/'content/handlers/javascript/quickjs'/name for name in
          ['quickjs.c','qjs_dom.c','qjs_native.c','qjs_module.c','qjs_canvas.c']]
patched=out/'backend.c'
backend=bindings[0].read_text()
old='JS_SetMaxStackSize(heap->rt, 512 * 1024);'
if backend.count(old)!=1: p.error('unsupported backend stack-limit site')
if backend.count('JS_NewRuntime()')!=1: p.error('unsupported backend allocator site')
backend=backend.replace(old,'JS_SetMaxStackSize(heap->rt, 64 * 1024);').replace(
    'JS_NewRuntime()','JS_NewRuntime2(&ov_web_alloc_functions, NULL)')
if a.trace_exec:
    backend=backend.replace('#include <nsutils/time.h>',
        '#include <nsutils/time.h>\n#include "html/private.h"\n#include "utils/talloc.h"')
    checkpoint='\t\tstruct jsthread *active = heap->active_thread;'
    if backend.count(checkpoint)!=1:p.error('unexpected task completion site')
    backend=backend.replace(checkpoint,checkpoint+'''
\t\tstatic uint64_t ov_memory_report=0;
\t\tuint64_t ov_memory_now=0;
\t\tnsu_getmonotonic_ms(&ov_memory_now);
\t\tif (active && active->htmlc && ov_memory_now-ov_memory_report>=2000) {
\t\t\tJSMemoryUsage usage;
\t\t\t/* A script may finish before the page has its first box tree.
\t\t\t * Unlike talloc_total_size, talloc_total_blocks rejects NULL. */
\t\t\tvoid *ov_box_context=active->htmlc->bctx;
\t\t\tJS_ComputeMemoryUsage(heap->rt,&usage);
\t\t\tfprintf(stderr,"[js-memory] used=%llu objects=%llu properties=%llu box_bytes=%zu box_blocks=%zu\\n",
\t\t\t\t(unsigned long long)usage.memory_used_size,(unsigned long long)usage.obj_count,
\t\t\t\t(unsigned long long)usage.prop_count,
\t\t\t\tov_box_context ? talloc_total_size(ov_box_context) : 0,
\t\t\t\tov_box_context ? talloc_total_blocks(ov_box_context) : 0);
\t\t\tov_memory_report=ov_memory_now;
\t\t}''')
    start='\tqjs_deadline_start_script(thread);\n\n\tv = JS_Eval'
    end='\tqjs_deadline_stop(thread);\n\tfree(buf);'
    if backend.count(start)!=1 or backend.count(end)!=1:p.error('unexpected script evaluation site')
    backend=backend.replace(start,'''\tuint64_t ov_eval_start=0,ov_eval_end=0;
\tnsu_getmonotonic_ms(&ov_eval_start);
\tfprintf(stderr,"[js-eval] begin bytes=%zu\\n",txtlen);
\tqjs_deadline_start_script(thread);

\tv = JS_Eval''').replace(end,'''\tnsu_getmonotonic_ms(&ov_eval_end);
\tfprintf(stderr,"[js-eval] end bytes=%zu elapsed_ms=%llu exception=%d\\n",txtlen,
\t\t(unsigned long long)(ov_eval_end-ov_eval_start),JS_IsException(v));
\tqjs_deadline_stop(thread);
\tfree(buf);''')
patched.write_text('#include "openvela_web_alloc.h"\n'+backend)
bindings[0]=patched
runtime=(ns/'content/handlers/javascript/quickjs/runtime.js').read_bytes()
packed=zlib.compress(runtime,9)
runtime_dir=out/'quickjs'; runtime_dir.mkdir(exist_ok=True)
runtime_header='#include <zlib.h>\nstatic const unsigned char runtime_js_z[] = {'+','.join(map(str,packed))+'};\n'
runtime_header+='static const unsigned runtime_js_len = '+str(len(runtime))+';\n'
runtime_header+='''static char *ov_runtime_source(void) {
    char *text = malloc(runtime_js_len + 1);
    if (!text) return NULL;
    uLongf length = runtime_js_len;
    if (uncompress((Bytef *)text, &length, runtime_js_z, sizeof(runtime_js_z)) != Z_OK || length != runtime_js_len) {
        free(text); return NULL;
    }
    text[length] = 0; return text;
}
'''
(runtime_dir/'runtime.js.inc').write_text(runtime_header)
dom=bindings[1].read_text()
copy='memcpy(src, runtime_js, runtime_js_len);'
if dom.count(copy)!=1 or dom.count('malloc(runtime_js_len + 1)')!=1: p.error('unexpected runtime embedding')
dom=dom.replace('malloc(runtime_js_len + 1)','ov_runtime_source()').replace(copy,'/* runtime source was decompressed with a fixed output bound */')
patched_dom=out/'qjs_dom.c'; patched_dom.write_text(dom); bindings[1]=patched_dom
commands=[]
def compile_one(item):
    group,source=item
    dest=out/(group+'_'+source.stem+'.raw.o')
    command=[a.cc,*flags,'-D__STDC_NO_ATOMICS__=1','-Dlodepng_encode32=ov_canvas_png_encode32',
             '-Dlodepng_error_text=ov_canvas_png_error_text','-c',str(source),'-o',str(dest)]
    commands.append(command)
    result=subprocess.run(command,capture_output=True,text=True)
    (out/(group+'_'+source.stem+'.log')).write_text(result.stdout+result.stderr)
    if result.returncode: raise RuntimeError(result.stderr)
    return dest
with ThreadPoolExecutor(max_workers=4) as pool:
    raw=list(pool.map(compile_one,[('engine',src) for src in engine]+[('binding',src) for src in bindings]))
prefix=a.cc[:-3] if a.cc.endswith('gcc') else ''
nm=prefix+'nm'; objcopy=prefix+'objcopy'
defined=set()
for obj in raw[:len(engine)]:
    for line in subprocess.check_output([nm,'-g','--defined-only',str(obj)],text=True).splitlines():
        fields=line.split()
        if len(fields)==3: defined.add(fields[2])
mapping=out/'namespace.map'
mapping.write_text(''.join(name+' ovweb_'+name+'\n' for name in sorted(defined)))
objects=[]
for obj in raw:
    dest=out/('webjs_'+obj.name.replace('.raw',''))
    subprocess.run([objcopy,'--redefine-syms='+str(mapping),str(obj),str(dest)],check=True)
    objects.append(str(dest))
(out/'manifest.json').write_text(json.dumps({'objects':objects,'namespace':'ovweb_',
    'trace_exec':a.trace_exec,
    'engine_symbols':len(defined),'js_heap_limit_bytes':6*1024*1024,'js_stack_limit_bytes':64*1024,
    'runtime_bytes':len(runtime),'runtime_packed_bytes':len(packed),
    'source_sha256':{str(s):hashlib.sha256(s.read_bytes()).hexdigest() for s in engine+bindings}},indent=2)+'\n')
(out/'commands.json').write_text(json.dumps(commands,indent=2))
print('WEB_JS_BUILT',len(objects),'objects;',len(defined),'engine symbols namespaced',flush=True)
