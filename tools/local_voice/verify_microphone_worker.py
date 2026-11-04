#!/usr/bin/env python3
"""Real offline ASR and text reply, fake capture/device; never records or plays audio."""
import argparse,hashlib,json,shutil,subprocess,tempfile,wave
from pathlib import Path

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('artifacts','asr-release','release','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    d=root/'app/espdl-quickapp/overlay/apps/system/desktop';e=root/'app/espdl-quickapp/experimental'
    sources=[d/('glass_local_'+v+'.c') for v in ('model','sha','tokenizer','assets','llm','w8a8','tts','frontend','voice','asr')]
    sources.extend(e/(v+'.c') for v in ('local_asr','local_asr_graph','local_asr_ops','local_asr_fbank'))
    sources.append(root/'app/espdl-quickapp/tests/local_microphone_worker_host.c')
    source_hashes={str(s.relative_to(root)):hashlib.sha256(s.read_bytes()).hexdigest() for s in sources}
    with tempfile.TemporaryDirectory(prefix='glv-microphone-worker-') as temp:
        temp=Path(temp);shutil.copytree(a.asr_release,temp/'asr')
        with wave.open(str(temp/'asr/checks/0.wav'),'rb') as wav:
            assert (wav.getnchannels(),wav.getsampwidth(),wav.getframerate())==(1,2,16000)
            (temp/'fixture.pcm').write_bytes(wav.readframes(wav.getnframes()))
        defs={'GLV_RELEASE_ROOT':a.release.resolve(),'GLV_ASR_RELEASE_ROOT':temp/'asr','GLV_CONFIG_PATH':temp/'wake-target','GLV_SESSION_ROOT':temp/'sessions','GLV_TEST_PCM':temp/'fixture.pcm'}
        exe=out/'microphone-worker';cmd=['gcc','-std=gnu11','-O2','-g','-ffp-contract=off','-fno-pie','-no-pie','-fsanitize=address,undefined','-DGLASS_AI_CONFIG_ENABLED','-DGLV_ASR_EVALUATION','-DGLV_ASR_MICROPHONE','-I'+str(d),'-I'+str(e)]
        cmd.extend('-D'+k+'="'+str(v)+'"' for k,v in defs.items());cmd.extend(str(s) for s in sources);cmd.extend(['-lm','-pthread','-o',str(exe)])
        subprocess.run(cmd,check=True)
        prior=json.loads((a.artifacts/'asr-c-transcription1/VERIFICATION.json').read_text(encoding='utf-8'));digest=2166136261
        for byte in prior['cases'][0]['text'].encode('utf-8'):digest=((digest^byte)*16777619)&0xffffffff
        with (out/'run.log').open('w',encoding='utf-8') as log:
            subprocess.run([str(exe),'%08x'%digest],stdout=log,stderr=subprocess.STDOUT,check=True,timeout=600)
        print((out/'run.log').read_text(encoding='utf-8')[-2400:],flush=True)
    assert source_hashes=={str(s.relative_to(root)):hashlib.sha256(s.read_bytes()).hexdigest() for s in sources},'Test sources changed while running; repeat against an immutable snapshot'
    report=dict(passed=True,scope='ASAN/UBSAN actual persistent worker -> real manifest-bound Zipformer sample -> real resident W8A8 EdgeIdiot text only; capture/card are fakes; zero playback, no physical/human acceptance',source_sha256=source_hashes)
    (out/'VERIFICATION.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')

if __name__=='__main__':main()
