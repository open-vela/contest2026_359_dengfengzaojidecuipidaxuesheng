#!/usr/bin/env python3
"""Exercise transcript ownership and new-session isolation with late fake engines."""
import argparse,hashlib,json,subprocess
from pathlib import Path
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--release',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--case',choices=('retag','new','diagnostics','empty','q4','all'),default='all');p.add_argument('--q4',action='store_true');a=p.parse_args()
 root=Path(__file__).resolve().parents[2];d=root/'app/espdl-quickapp/overlay/apps/system/desktop';out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
 sources=[d/('glass_local_'+n+'.c') for n in ('model','sha','tokenizer','assets','voice')]+[root/'app/espdl-quickapp/tests/local_request_isolation_host.c'];deps=sources+[d/'glass_local_voice.h',d/'glass_local_model.h',Path(__file__)]
 hashes={str(s.relative_to(root)):hashlib.sha256(s.read_bytes()).hexdigest() for s in deps};exe=out/'request-isolation'
 cmd=['gcc','-std=gnu11','-O2','-g','-ffp-contract=off','-fsanitize=address,undefined','-fno-pie','-no-pie','-DGLASS_AI_CONFIG_ENABLED','-DGLV_ASR_EVALUATION','-DGLV_ASR_MICROPHONE','-I'+str(d),'-DGLV_RELEASE_ROOT="'+str(a.release.resolve())+'"','-DGLV_CONFIG_PATH="'+str(out/'wake')+'"']
 if a.q4:
  # The service gate supplies a deterministic selftest double; matrix code is
  # separately qualified with real models. Exclude model dispatch here.
  sources[0]=out/'model.c';sources[0].write_text((d/'glass_local_model.c').read_text().replace('#ifdef GLV_Q4_PIE','#if 0'))
  cmd+=['-DGLV_Q4_PIE','-I'+str(root/'app/espdl-quickapp/experimental')]
 cmd.extend(str(s) for s in sources);cmd.extend(['-lm','-pthread','-o',str(exe)]);subprocess.run(cmd,check=True)
 for case in (('retag','new','diagnostics','empty','q4') if a.q4 else ('retag','new','diagnostics','empty')) if a.case=='all' else (a.case,):
  run=subprocess.run([str(exe),case],capture_output=True,text=True,timeout=60);(out/(case+'.log')).write_text(run.stdout+run.stderr,encoding='utf-8');print(run.stdout+run.stderr,flush=True);run.check_returncode()
 assert hashes=={str(s.relative_to(root)):hashlib.sha256(s.read_bytes()).hexdigest() for s in deps}
 (out/'VERIFICATION.json').write_text(json.dumps(dict(passed=True,scope='Actual worker, real base-package checks, deterministic fake capture/ASR/LLM; request ownership and immediate new-session isolation only, no model/hardware claim',source_sha256=hashes),indent=2)+'\n',encoding='utf-8')
if __name__=='__main__':main()
