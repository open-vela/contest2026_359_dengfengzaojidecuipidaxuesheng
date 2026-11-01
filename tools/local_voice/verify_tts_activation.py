#!/usr/bin/env python3
"""Numeric and CPU-host timing gate; never infer board speed from host results."""
import argparse, hashlib, json, os, subprocess, time
from pathlib import Path

def main():
    p=argparse.ArgumentParser()
    for name in ('reference','release','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();repo=Path(__file__).resolve().parents[2]
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    desktop=Path('app/espdl-quickapp/overlay/apps/system/desktop')
    tests=repo/'app/espdl-quickapp/tests'
    sources=['glass_local_'+n+'.c' for n in ('model','sha','tokenizer','llm','tts','frontend')]
    hashes={str(desktop/s):hashlib.sha256((repo/desktop/s).read_bytes()).hexdigest() for s in sources}
    report=dict(passed=False,scope='Host exact FP32 and CPU timing; no board latency claim',cases=[])
    def run(cmd,log,**kw):
        r=subprocess.run(list(map(str,cmd)),capture_output=True,text=True,timeout=300,**kw)
        (out/log).write_text(r.stdout+r.stderr);r.check_returncode();return r
    flags=['gcc','-std=gnu11','-O2','-g','-ffp-contract=off','-fno-pie','-no-pie']
    san=['-fsanitize=address,undefined']
    run([*flags,*san,'-I'+str(repo/desktop),tests/'local_tts_activation_host.c',
         repo/desktop/'glass_local_model.c',repo/desktop/'glass_local_sha.c',
         '-Wl,--wrap=calloc','-lm','-o',out/'unit'],'unit-build.log')
    run([out/'unit'],'unit.log');report['synthetic_cases']=432
    for name,base in (('before',a.reference),('after',repo)):
        run([*flags,'-I'+str(base/desktop),*[base/desktop/s for s in sources],
             tests/'local_voice_host.c','-lm','-o',out/name],name+'-build.log')
    for budget in (12,5):
        for repeat in range(3):
            for name in (('before','after') if repeat%2==0 else ('after','before')):
                raw=out/f'{name}-{budget}-{repeat}.f32';began=time.monotonic()
                r=run([out/name,'synthesize',a.release,'你好。',raw],raw.stem+'.log',
                      env=dict(os.environ,GLV_TEST_MEMORY_LIMIT=str(budget*1024*1024)))
                report['cases'].append(dict(version=name,budget_mib=budget,repeat=repeat,
                    host_seconds=time.monotonic()-began,sha256=hashlib.sha256(raw.read_bytes()).hexdigest(),
                    diagnostics=r.stderr.strip()))
    assert len({r['sha256'] for r in report['cases']})==1
    assert hashes=={str(desktop/s):hashlib.sha256((repo/desktop/s).read_bytes()).hexdigest() for s in sources}
    report['source_sha256']=hashes
    report['passed']=True
    (out/'VERIFICATION.json').write_text(json.dumps(report,indent=2)+'\n')
    print('TTS_ACTIVATION_EXACT_HOST_GATE_OK',flush=True)
    for row in report['cases']:print(json.dumps(row),flush=True)

if __name__=='__main__':main()
