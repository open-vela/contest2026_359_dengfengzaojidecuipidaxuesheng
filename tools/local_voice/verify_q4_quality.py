#!/usr/bin/env python3
"""Qualify Q4/A16 approximation on actual local model, without claiming PIE speed."""
import argparse
import hashlib
import json
import math
import shutil
from pathlib import Path
import struct
import subprocess


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--release',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--a16-only',action='store_true')
    a=p.parse_args()
    root=Path(__file__).resolve().parents[2]
    d=root/'app/espdl-quickapp/overlay/apps/system/desktop'
    exp=root/'app/espdl-quickapp/experimental'
    harness=root/'app/espdl-quickapp/tests/local_q4_quality_host.c'
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    sources=[d/('glass_local_'+n+'.c') for n in ('model','sha','tokenizer','llm')]+[exp/'local_q4_pie.c',harness]
    unit=root/'app/espdl-quickapp/tests/local_q4_unit_host.c'
    deps=sources+[d/('glass_local_'+n+'.h') for n in ('model','llm','tokenizer')]+[exp/'local_q4_pie.h',unit,Path(__file__)]
    hashes={str(f.relative_to(root)):hashlib.sha256(f.read_bytes()).hexdigest() for f in deps}
    for f in deps:
        target=out/'source'/f.relative_to(root);target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(f,target)
    frozen=out/'source';d=frozen/d.relative_to(root);exp=frozen/exp.relative_to(root)
    sources=[frozen/f.relative_to(root) for f in sources]
    unit=frozen/unit.relative_to(root)
    command=['gcc','-std=gnu11','-O2','-g','-ffp-contract=off','-fsanitize=address,undefined',
             '-fno-pie','-no-pie','-I'+str(d),'-I'+str(exp)]+list(map(str,sources))+[
             '-Wl,--wrap=glv_matvec,--wrap=glv_matbatch','-lm','-o',str(out/'quality')]
    subprocess.run(command,check=True)
    subprocess.run(command[:command.index(str(sources[0]))]+[str(sources[0]),str(sources[1]),str(exp/'local_q4_pie.c'),str(unit),'-lm','-o',str(out/'unit')],check=True)
    test=subprocess.run([str(out/'unit')],capture_output=True,text=True,check=True)
    (out/'unit.log').write_text(test.stdout+test.stderr)
    prompts=['你好','请介绍一下你自己。','一加一等于几？','用一句话解释什么是人工智能。',
             '请给我三个保持健康的建议。','为什么天空是蓝色的？',
             '把“谢谢”翻译成英文。','请写一句关于春天的诗。',
             '我今天有点不开心，你能安慰我吗？','请按步骤说明如何煮一杯茶。']
    cases=[]
    for i,text in enumerate(prompts):
        prompt=out/f'prompt-{i}.txt'
        prompt.write_text('<|im_start|>user\n'+text+'<|im_end|>\n<|im_start|>assistant\n',encoding='utf-8')
        modes=[('prefill',32),('generate',32)]
        if i==0:modes += [('context',33),('context',512)]
        for mode,count in modes:
            outputs={};metrics={}
            for bits in ((0,16) if a.a16_only else (0,8,16)):
                tag=f'{i}-{mode}-{count}-a{bits}'
                target=out/(tag+'.bin')
                r=subprocess.run([str(out/'quality'),str(bits),str(a.release/'edgeidiot'),str(prompt),mode,str(target),str(count)],
                                 capture_output=True,text=True,timeout=600)
                (out/(tag+'.log')).write_text(r.stdout+r.stderr)
                r.check_returncode();metrics[bits]=json.loads(r.stdout);outputs[bits]=target.read_bytes()
            comparison={}
            for bits in ((16,) if a.a16_only else (8,16)):
                if mode=='generate':
                    comparison[bits]={'reply_equal':outputs[0]==outputs[bits],
                                      'reference':outputs[0].decode('utf-8'),
                                      'reply':outputs[bits].decode('utf-8')}
                else:
                    ref=struct.unpack('<8192f',outputs[0]);test=struct.unpack('<8192f',outputs[bits])
                    assert all(map(math.isfinite,test))
                    error=math.sqrt(sum((x-y)**2 for x,y in zip(ref,test))/sum(x*x for x in ref))
                    comparison[bits]={'relative_l2':error,'max_abs':max(abs(x-y) for x,y in zip(ref,test)),
                                      'top1_equal':max(range(8192),key=ref.__getitem__)==max(range(8192),key=test.__getitem__)}
            cases.append(dict(prompt=i,mode=mode,count=count,metrics=metrics,comparison=comparison))
            (out/'PROGRESS.json').write_text(json.dumps(cases,ensure_ascii=False,indent=2))
            print('QUALITY',i,mode,count,json.dumps(comparison,ensure_ascii=True),flush=True)
    assert hashes=={name:hashlib.sha256((frozen/name).read_bytes()).hexdigest() for name in hashes}
    def passes(bits):
        return all(c['comparison'][bits].get('reply_equal',True) and
                   c['comparison'][bits].get('top1_equal',True) and
                   c['comparison'][bits].get('relative_l2',0)<0.001 for c in cases)
    result=dict(passed=passes(16),a16_passed=passes(16),a8_passed=None if a.a16_only else passes(8),unit_passed=True,
                scope='Host ASAN/UBSAN approximation qualification; scalar integer dot, not P4 execution or speed',
                gate='10 prompts x 32 generated tokens must match reference; prompt/context logits top1 match and relative L2 < 0.001',
                source_sha256=hashes,cases=cases)
    (out/'VERIFICATION.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
    print('A16_PASSED',result['a16_passed'],'A8_PASSED',result['a8_passed'],flush=True)


if __name__=='__main__':main()
