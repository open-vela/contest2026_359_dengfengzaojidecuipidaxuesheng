#!/usr/bin/env python3
"""Trace actual model file opens and execute real text/TTS jobs on the host."""
import argparse,hashlib,json,shutil,subprocess,tempfile
from pathlib import Path
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    p=argparse.ArgumentParser();p.add_argument('--release',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    repo=Path(__file__).resolve().parents[2];d=repo/'app/espdl-quickapp/overlay/apps/system/desktop'
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    sources=[d/f'glass_local_{n}.c' for n in ('model','sha','assets','tokenizer','llm','tts','frontend','voice')]+[repo/'app/espdl-quickapp/tests/local_lazy_service_host.c']
    deps=[*sources,d/'glass_local_assets.h',d/'glass_local_model.h',d/'glass_local_voice.h',Path(__file__)]
    hashes={str(f.relative_to(repo)):sha(f) for f in deps};cases=[]
    manifest=json.loads((a.release/'manifest.json').read_text())
    llm=sum(f['bytes'] for f in manifest['files'] if f['path'].startswith('edgeidiot/'))
    tts=sum(f['bytes'] for f in manifest['files'] if f['path'].startswith('tts-v2/') or f['path']=='edgeidiot/tokenizer.bin')
    with tempfile.TemporaryDirectory(prefix='glv-lazy-') as temporary:
        folder=Path(temporary);models=folder/'models';config=folder/'wake-target';exe=out/'lazy-service'
        cmd=['gcc','-std=gnu11','-O2','-g','-ffp-contract=off','-fsanitize=address,undefined','-fno-pie','-no-pie','-DGLASS_AI_CONFIG_ENABLED','-DGLV_ASR_EVALUATION','-DGLV_ASR_MICROPHONE','-I'+str(d),'-DGLV_RELEASE_ROOT="'+str(models)+'"','-DGLV_CONFIG_PATH="'+str(config)+'"',f'-DGLV_EXPECT_LLM_BYTES={llm}u',f'-DGLV_EXPECT_TTS_BYTES={tts}u',*map(str,sources),'-Wl,--wrap=fopen','-lm','-pthread','-o',str(exe)]
        subprocess.run(cmd,check=True)
        for case in ('cold','chat','chat-idle-card','read','read-card'):
            if config.exists():config.unlink()
            if models.exists():shutil.rmtree(models)
            if case!='cold':
                shutil.copytree(a.release,models)
                for unused in (('tts-v2','wake') if case.startswith('chat') else ('edgeidiot','wake')):shutil.rmtree(models/unused)
                if not case.startswith('chat'):
                    (models/'edgeidiot').mkdir();shutil.copyfile(a.release/'edgeidiot/tokenizer.bin',models/'edgeidiot/tokenizer.bin')
            run=subprocess.run([str(exe),case],capture_output=True,text=True,timeout=180)
            (out/f'{case}.log').write_text(run.stdout+run.stderr);print(run.stdout+run.stderr,flush=True);run.check_returncode()
            cases.append(dict(case=case,passed=True))
    assert hashes=={str(f.relative_to(repo)):sha(f) for f in deps}
    (out/'VERIFICATION.json').write_text(json.dumps(dict(passed=True,cases=cases,source_sha256=hashes,llm_only_sha_bytes=llm,tts_only_sha_bytes=tts,
        scope='ASAN/UBSAN actual worker/manifest/LLM/V2; traced file opens, no startup model access, missing unused models, explicit read and card loss; ASR/hardware are inert fakes'),indent=2)+'\n')
if __name__=='__main__':main()
