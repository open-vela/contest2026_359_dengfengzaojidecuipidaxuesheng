#!/usr/bin/env python3
"""Compare all C graph outputs before/after SD I/O batching/cache changes."""
import argparse,ctypes as ct,hashlib,json
from pathlib import Path
import numpy as np
from compile_asr_graph import build_native,run
from verify_asr_ops import Memory,CANCEL


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--baseline-source',type=Path,required=True);p.add_argument('--assets',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    roots=[a.baseline_source.resolve(),root];natives=[]
    for name,source in zip(('before','after'),roots):
        folder=out/name;folder.mkdir();natives.append(build_native(source,folder,None))
    natives[1].glv_asr_graph_cache.argtypes=[ct.c_void_p,ct.c_size_t]
    never=CANCEL(lambda _:False);rng=np.random.default_rng(621);cases=[]
    for file in sorted(a.assets.glob('*-avg-1.json')):
        report=json.loads(file.read_text(encoding='utf-8'));descs={v['name']:v for v in report['tensors']}
        handle=[ct.c_void_p(),ct.c_void_p()];memory=[Memory(0,0,12*1024*1024,never,None),Memory(0,0,12*1024*1024,never,None)]
        inputs=[]
        for _ in range(2):
            inputs.append({name:np.zeros(descs[name]['shape'],dtype={1:np.float32,7:np.int64,6:np.int32,9:np.bool_}[descs[name]['type']]) for name in report['inputs']})
        try:
            for i in range(2):assert natives[i].glv_asr_graph_open(ct.byref(handle[i]),str(file.with_suffix('')).encode(),ct.byref(memory[i]))==0
            cached=file.name.startswith('joiner')
            if cached:assert natives[1].glv_asr_graph_cache(handle[1],2*1024*1024)==0
            for step in range(3):
                for name in inputs[0]:
                    if name=='y':value=rng.integers(0,5537,inputs[0][name].shape,dtype=np.int64)
                    elif 'cached' not in name:value=rng.standard_normal(inputs[0][name].shape).astype(np.float32)
                    else:continue
                    for i in range(2):inputs[i][name]=value
                actual=[]
                for i in range(2):actual.append(run(natives[i],handle[i],memory[i],report,inputs[i])[0])
                for name in report['outputs']:assert np.array_equal(actual[0][name],actual[1][name],equal_nan=True),(file.name,step,name)
                if file.name.startswith('encoder'):
                    for name in report['inputs']:
                        if name!='x':
                            for i in range(2):inputs[i][name]=actual[i]['new_'+name]
                cases.append(dict(graph=file.name,chunk=step,all_output_bytes_equal=True,after_packed_joiner_cache=cached))
                print('ASR_IO_BIT_EXACT',file.name,step,flush=True)
        finally:
            for i in range(2):natives[i].glv_asr_graph_close(handle[i]);assert memory[i].used==0
    sources=[root/'app/espdl-quickapp/experimental'/name for name in ('local_asr.c','local_asr.h','local_asr_graph.c','local_asr_graph.h','local_asr_ops.c','local_asr_ops.h')]
    result=dict(passed=True,scope='All C encoder/state, decoder and cached joiner outputs bit-identical over three independent state chunks vs frozen candidate8 C source; not P4 timing or SD fault cure',asr_ready=False,cases=cases,source_sha256={str(s.relative_to(root)):hashlib.sha256(s.read_bytes()).hexdigest() for s in sources})
    (out/'VERIFICATION.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')


if __name__=='__main__':main()
