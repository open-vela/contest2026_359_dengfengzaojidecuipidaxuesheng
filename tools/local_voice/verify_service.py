#!/usr/bin/env python3
"""Exercise real worker/engines with a fake DMA owner, never the board."""
from pathlib import Path
import errno
import argparse,json,subprocess,shutil,os

def main():
 p=argparse.ArgumentParser();p.add_argument('--release',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--save-only',action='store_true');p.add_argument('--tts-benchmark',action='store_true');p.add_argument('--tts-pie',action='store_true');p.add_argument('--sha-hw',action='store_true');p.add_argument('--tts-parallel',action='store_true');p.add_argument('--tts-fma',action='store_true');a=p.parse_args()
 if a.tts_fma and not a.tts_parallel:p.error('--tts-fma requires --tts-parallel')
 repo=Path(__file__).resolve().parents[2];desktop=repo/'app/espdl-quickapp/overlay/apps/system/desktop';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
 root=out/'models';root.mkdir(exist_ok=True)
 for f in a.release.resolve().rglob('*'):
  if not f.is_file():continue
  target=root/f.relative_to(a.release.resolve());target.parent.mkdir(parents=True,exist_ok=True)
  if target.exists():continue
  if f.name=='manifest.bin':shutil.copyfile(f,target)
  else:
   try:os.link(f,target)
   except OSError as error:
    if error.errno!=errno.EXDEV:raise
    shutil.copyfile(f,target)
 sources=['model','sha','tokenizer','assets','llm','tts','frontend','voice']
 cmd=['gcc','-std=gnu11','-O2','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie','-ffp-contract=off','-DGLASS_AI_CONFIG_ENABLED',
      '-DGLV_RELEASE_ROOT="'+str(root)+'"','-DGLV_CONFIG_PATH="'+str(out/'wake-target')+'"','-DGLV_SESSION_ROOT="'+str(out/'sessions')+'"','-DGLV_SCRATCH_ROOT="'+str(out)+'"','-I'+str(desktop)]
 cmd += [str(desktop/('glass_local_'+s+'.c')) for s in sources]
 if a.tts_pie:
  experimental=repo/'app/espdl-quickapp/experimental'
  cmd+=['-DGLV_TTS_PIE','-I'+str(experimental),str(experimental/'local_tts_pie.c')]
 if a.sha_hw:
  experimental=repo/'app/espdl-quickapp/experimental';mock=repo/'app/espdl-quickapp/tests/local_sha_mock'
  cmd+=['-DGLV_SHA_HW','-Wno-deprecated-declarations','-I'+str(experimental),'-I'+str(mock),str(experimental/'local_sha_hw.c'),str(mock/'mock.c')]
 if a.tts_parallel:
  experimental=repo/'app/espdl-quickapp/experimental'
  cmd+=['-DGLV_TTS_PARALLEL','-I'+str(experimental),str(experimental/'local_tts_parallel.c')]
 if a.tts_fma:cmd+=['-DGLV_TTS_FMA']
 cmd += [str(repo/'app/espdl-quickapp/tests/local_voice_service_host.c'),'-lm','-pthread',*(['-lcrypto'] if a.sha_hw else []),'-o',str(out/'service-host')]
 subprocess.run(cmd,check=True)
 env=dict(os.environ)
 if a.save_only:env['GLV_TEST_SAVE_ONLY']='1'
 if a.tts_benchmark:env['GLV_TEST_TTS_BENCH']='1'
 r=subprocess.run([str(out/'service-host')],capture_output=True,text=True,timeout=300,env=env)
 (out/'run.log').write_text(r.stdout+r.stderr)
 print((r.stdout+r.stderr)[-4000:]);r.check_returncode()
 (out/'VERIFICATION.json').write_text(json.dumps(dict(status='passed',scope='host real models, fake audio hardware',tts_pie=a.tts_pie,sha_hw_adapter_mock=a.sha_hw,tts_parallel=a.tts_parallel,tts_fma=a.tts_fma,tests='TTS benchmark no audio device open' if a.tts_benchmark else 'explicit SD save/no implicit write/new-session' if a.save_only else 'cleanup cancel stale-request corruption low-memory wake-switch 24k'),indent=2)+'\n')

if __name__=='__main__':main()
