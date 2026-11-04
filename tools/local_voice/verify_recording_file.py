from pathlib import Path
import subprocess,os,wave
import argparse
p=argparse.ArgumentParser();p.add_argument('--asr-release',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();release=a.asr_release.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);r=Path(__file__).resolve().parents[2];d=r/'app/espdl-quickapp/overlay/apps/system/desktop';e=r/'app/espdl-quickapp/experimental'
a=release
files=[d/('glass_local_'+n+'.c') for n in ['model','sha','tokenizer','assets','llm','w8a8','tts','frontend','voice','asr']]+[e/(n+'.c') for n in ['local_asr','local_asr_graph','local_asr_ops','local_asr_fbank','local_asr_accel','local_asr_parallel']]+[r/'app/espdl-quickapp/tests/local_recording_file_host.c']
cmd=['gcc','-std=gnu11','-O2','-g','-ffp-contract=off','-fsanitize=address,undefined','-fno-pie','-no-pie','-DGLASS_AI_CONFIG_ENABLED','-DGLV_ASR_EVALUATION','-DGLV_ASR_MICROPHONE','-DGLV_ASR_STREAMING','-DGLV_ASR_ACCEL_ENABLED','-DGLV_ASR_RELEASE_ROOT="'+str(a)+'"','-DGLV_RELEASE_ROOT="'+str(out/'no-llm')+'"','-DGLV_CONFIG_PATH="'+str(out/'host-setting')+'"','-I'+str(d),'-I'+str(e),*map(str,files),'-lm','-pthread','-o',str(out/'asr-file-host')]
subprocess.run(cmd,check=True)
with wave.open(str(a/'checks/0.wav'),'rb') as f:
 params=f.getparams();pcm=f.readframes(f.getnframes())
with wave.open(str(out/'recorded-format.wav'),'wb') as f:f.setparams(params);f.writeframes(pcm)
subprocess.run([str(out/'asr-file-host'),str(out/'recorded-format.wav')],check=True,timeout=180,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
