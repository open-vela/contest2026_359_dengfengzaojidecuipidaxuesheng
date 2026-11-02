#!/usr/bin/env python3
"""Test real service+ASR from manifest-bound WAVs, with fake audio/card state."""
import argparse,hashlib,json,shutil,subprocess,tempfile
from pathlib import Path


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--artifacts',type=Path,required=True);p.add_argument('--asr-release',type=Path,required=True);p.add_argument('--release',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    desktop=root/'app/espdl-quickapp/overlay/apps/system/desktop';exp=root/'app/espdl-quickapp/experimental'
    source=[desktop/('glass_local_'+v+'.c') for v in ('model','sha','tokenizer','assets','llm','tts','frontend','voice','asr')]
    source.extend(exp/(v+'.c') for v in ('local_asr','local_asr_graph','local_asr_ops','local_asr_fbank'));source.append(root/'app/espdl-quickapp/tests/local_asr_worker_host.c')
    with tempfile.TemporaryDirectory(prefix='glv-asr-worker-') as tmp:
        tmp=Path(tmp);shutil.copytree(a.asr_release,tmp/'asr')
        definitions={'GLV_RELEASE_ROOT':a.release.resolve(),'GLV_ASR_RELEASE_ROOT':tmp/'asr','GLV_CONFIG_PATH':tmp/'wake-target','GLV_SESSION_ROOT':tmp/'sessions'}
        exe=out/'asr-worker'
        cmd=['gcc','-std=gnu11','-O2','-g','-ffp-contract=off','-fno-pie','-no-pie','-fsanitize=address,undefined','-DGLASS_AI_CONFIG_ENABLED','-DGLV_ASR_EVALUATION','-I'+str(desktop),'-I'+str(exp)]
        cmd.extend('-D'+key+'="'+str(value)+'"' for key,value in definitions.items());cmd.extend(str(s) for s in source);cmd.extend(['-lm','-pthread','-o',str(exe)]);subprocess.run(cmd,check=True)
        prior=json.loads((a.artifacts/'asr-c-transcription1/VERIFICATION.json').read_text(encoding='utf-8'));hashes=[]
        for case in prior['cases']:
            digest=2166136261
            for byte in case['text'].encode():digest=((digest^byte)*16777619)&0xffffffff
            hashes.append('%08x'%digest)
        result=subprocess.run([str(exe),*hashes],capture_output=True,text=True,timeout=240)
        (out/'run.log').write_text(result.stdout+result.stderr,encoding='utf-8');print((result.stdout+result.stderr)[-2400:],flush=True);result.check_returncode()
    result=dict(passed=True,status='passed',scope='Host actual persistent local-voice worker, manifest/16kHz WAV parser and full C models; fake platform/audio/card. Not P4 proof and ASR microphone remains unready',asr_ready=False,source_sha256={str(s.relative_to(root)):hashlib.sha256(s.read_bytes()).hexdigest() for s in source},asr_resource_manifest_sha256=hashlib.sha256((a.asr_release/'manifest.bin').read_bytes()).hexdigest())
    (out/'VERIFICATION.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')


if __name__=='__main__':main()
