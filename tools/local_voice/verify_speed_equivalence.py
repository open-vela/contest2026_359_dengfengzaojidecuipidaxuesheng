#!/usr/bin/env python3
"""Compare production scalar arithmetic with the deployed frozen candidate."""
import argparse,hashlib,json,subprocess
from pathlib import Path
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    p=argparse.ArgumentParser();p.add_argument('--reference',type=Path,required=True);p.add_argument('--release',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    repo=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    rel=Path('app/espdl-quickapp/overlay/apps/system/desktop');harness=repo/'app/espdl-quickapp/tests/local_llm_prefill_host.c'
    exes={};deps=[]
    for name,tree in [('before',a.reference.resolve()),('after',repo)]:
        d=tree/rel;sources=[d/f'glass_local_{n}.c' for n in ('model','sha','tokenizer','llm')]
        exes[name]=out/name
        subprocess.run(['gcc','-std=gnu11','-O2','-g','-ffp-contract=off','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(d),*map(str,sources),str(harness),'-Wl,--wrap=fread,--wrap=fseek','-lm','-o',str(exes[name])],check=True)
        if name=='after':deps=sources+[d/'glass_local_model.h',harness,Path(__file__)]
    hashes={str(f.relative_to(repo)):sha(f) for f in deps};cases=[]
    for mode,count,budget in [('prefill',1,'normal'),('prefill',20,'normal'),('prefill',33,'normal'),('prefill',512,'normal'),('prefill',31,'tight'),('prefill',64,'tiny'),('cancel',64,'normal'),('generate',8,'normal')]:
        values={};outputs={}
        for version in exes:
            tag=f'{version}-{mode}-{count}-{budget}';target=out/f'{tag}.bin'
            run=subprocess.run([str(exes[version]),mode,str(a.release.resolve()/'edgeidiot'),str(count),str(target),budget],capture_output=True,text=True,timeout=300)
            (out/f'{tag}.log').write_text(run.stdout+run.stderr);run.check_returncode();values[version]=json.loads(run.stdout)
            outputs[version]=target.read_bytes() if target.exists() else None
        assert outputs['before']==outputs['after'],(mode,count,budget)
        assert values['before']['error']==values['after']['error']
        assert values['before']['fread_bytes']==values['after']['fread_bytes']
        assert all(not v['remaining_bytes'] for v in values.values())
        cases.append(dict(mode=mode,tokens=count,budget=budget,bit_exact=True,stats=values));print('SPEED_ARITHMETIC_EXACT',mode,count,budget,flush=True)
    assert hashes=={str(f.relative_to(repo)):sha(f) for f in deps}
    (out/'VERIFICATION.json').write_text(json.dumps(dict(passed=True,cases=cases,source_sha256=hashes,scope='Host ASAN/UBSAN actual scalar LLM, exact logits/replies, memory and cancellation; host timings are not board speed measurements'),indent=2)+'\n')
if __name__=='__main__':main()
