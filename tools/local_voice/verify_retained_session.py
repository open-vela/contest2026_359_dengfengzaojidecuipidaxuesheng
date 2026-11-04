#!/usr/bin/env python3
"""Real Q4/worker retained-session equivalence, invalidation and retirement."""
import argparse,hashlib,json,shutil,subprocess,tempfile
from pathlib import Path

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    p=argparse.ArgumentParser();p.add_argument('--release',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    repo=Path(__file__).resolve().parents[2];d=repo/'app/espdl-quickapp/overlay/apps/system/desktop'
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    engine=[d/f'glass_local_{n}.c' for n in ('model','sha','tokenizer','llm','w8a8')]
    worker=engine+[d/f'glass_local_{n}.c' for n in ('assets','frontend','tts','voice')]
    vector=repo/'app/espdl-quickapp/tests/local_llm_session_host.c'
    service=repo/'app/espdl-quickapp/tests/local_retained_service_host.c'
    deps=[*worker,vector,service,d/'glass_local_model.h',d/'glass_local_assets.h',d/'glass_local_voice.h',d/'glass_local_platform.h',Path(__file__)]
    hashes={str(f.relative_to(repo)):sha(f) for f in deps}
    flags=['gcc','-std=gnu11','-O2','-g','-ffp-contract=off','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(d)]
    exe=out/'llm-session'
    subprocess.run([*flags,*map(str,engine),str(vector),'-lm','-o',str(exe)],check=True)
    for mode in ('cold','warm'):
        run=subprocess.run([str(exe),str(a.release/'edgeidiot'),mode,str(out/(mode+'.bin'))],capture_output=True,text=True,timeout=300)
        (out/(mode+'.log')).write_text(run.stdout+run.stderr);print(run.stdout+run.stderr,flush=True);run.check_returncode()
    assert (out/'cold.bin').read_bytes()==(out/'warm.bin').read_bytes()
    report=dict(passed=False,source_sha256=hashes,vector_replies_bit_exact=True,cases=[],scope='ASAN/UBSAN real resident W8A8 engine and worker; inert audio, deterministic storage stamp. No native speed claim.')
    with tempfile.TemporaryDirectory(prefix='glv-session-') as tmp:
        models=Path(tmp)/'models';config=Path(tmp)/'wake';exe=out/'retained-service'
        subprocess.run([*flags,'-DGLASS_AI_CONFIG_ENABLED',f'-DGLV_RELEASE_ROOT="{models}"',f'-DGLV_CONFIG_PATH="{config}"',
                        *map(str,worker),str(service),'-Wl,--wrap=glv_llm_generate_tool','-lm','-pthread','-o',str(exe)],check=True)
        for mode in ('cold','warm','release','new','audio','config','writer','media','corrupt','cancel','close-active','pressure','idle-yield'):
            if models.exists():shutil.rmtree(models)
            shutil.copytree(a.release,models)
            if config.exists():config.unlink()
            target=out/(mode+'-service.bin')
            run=subprocess.run([str(exe),mode,str(target)],capture_output=True,text=True,timeout=180)
            (out/(mode+'-service.log')).write_text(run.stdout+run.stderr);print(run.stdout+run.stderr,flush=True);run.check_returncode()
            report['cases'].append(dict(mode=mode,passed=True))
            (out/'VERIFICATION.json').write_text(json.dumps(report,indent=2)+'\n')
        assert (out/'cold-service.bin').read_bytes()==(out/'warm-service.bin').read_bytes()
    assert hashes=={str(f.relative_to(repo)):sha(f) for f in deps}
    report['passed']=True;report['worker_replies_bit_exact']=True
    (out/'VERIFICATION.json').write_text(json.dumps(report,indent=2)+'\n')
if __name__=='__main__':main()
