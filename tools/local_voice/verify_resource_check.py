#!/usr/bin/env python3
"""Check production model-validation cancellation and isolated ASR failures."""
import argparse,hashlib,json,subprocess
from pathlib import Path
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--release',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
 root=Path(__file__).resolve().parents[2];d=root/'app/espdl-quickapp/overlay/apps/system/desktop';out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
 sources=[d/('glass_local_'+v+'.c') for v in ('model','sha','tokenizer','assets','llm','tts','frontend','voice')]+[root/'app/espdl-quickapp/tests/local_resource_check_host.c']
 dependencies=sources+[d/('glass_local_'+v+'.h') for v in ('model','voice','capture','asr')]
 hashes={str(s.relative_to(root)):hashlib.sha256(s.read_bytes()).hexdigest() for s in dependencies};exe=out/'resource-check'
 cmd=['gcc','-std=gnu11','-O2','-g','-ffp-contract=off','-fsanitize=address,undefined','-fno-pie','-no-pie','-DGLASS_AI_CONFIG_ENABLED','-DGLV_ASR_EVALUATION','-DGLV_ASR_MICROPHONE','-I'+str(d),'-DGLV_RELEASE_ROOT="'+str(a.release.resolve())+'"','-DGLV_CONFIG_PATH="'+str(out/'wake')+'"']
 cmd.extend(str(s) for s in sources);cmd.extend(['-lm','-pthread','-o',str(exe)]);subprocess.run(cmd,check=True)
 run=subprocess.run([str(exe)],capture_output=True,text=True,timeout=60);(out/'run.log').write_text(run.stdout+run.stderr,encoding='utf-8');print(run.stdout+run.stderr,flush=True);run.check_returncode()
 assert hashes=={str(s.relative_to(root)):hashlib.sha256(s.read_bytes()).hexdigest() for s in dependencies}
 (out/'VERIFICATION.json').write_text(json.dumps(dict(passed=True,scope='ASAN/UBSAN production worker with real base-package SHA checks; deterministic ASR-check/card/admission fakes, no microphone or playback',source_sha256=hashes),indent=2)+'\n',encoding='utf-8')
if __name__=='__main__':main()
