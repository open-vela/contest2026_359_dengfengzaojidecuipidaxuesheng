#!/usr/bin/env python3
"""Evaluate W8A8 vocoder convolutions with original SD Q8 assets; never flash.

Host timing is not ESP32-P4 timing. Quality thresholds screen waveform error,
not perceptual acceptance. Intermediate FP32 operators remain unchanged.
"""
import argparse,array,hashlib,json,math,os,subprocess,sys
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser();p.add_argument('--models',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--cases',type=Path);p.add_argument('--cross-prefix');a=p.parse_args()
 repo=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(exist_ok=False)
 d=repo/'app/espdl-quickapp/overlay/apps/system/desktop';e=repo/'app/espdl-quickapp/experimental';t=repo/'app/espdl-quickapp/tests'
 models=a.models.resolve();original=sha(models/'tts-v2/weights.bin');assert sys.byteorder=='little'
 def run(cmd,name,**kw):
  r=subprocess.run(list(map(str,cmd)),capture_output=True,text=True,timeout=600,**kw);(out/name).write_text(r.stdout+r.stderr);r.check_returncode();return r
 flags=['gcc','-std=gnu11','-O2','-g','-ffp-contract=off','-fno-pie','-no-pie','-DGLV_TTS_PARALLEL','-DGLV_TTS_FMA','-I'+str(d),'-I'+str(e)]
 run([*flags,'-fsanitize=address,undefined',t/'local_tts_int8_host.c',d/'glass_local_model.c',d/'glass_local_sha.c',e/'local_tts_parallel.c','-Wl,--wrap=calloc','-lm','-pthread','-o',out/'unit'],'unit-build.log')
 print(run([out/'unit'],'unit.log').stdout,flush=True)
 print(run([sys.executable,repo/'tools/local_voice/test_tts_int8_pipeline.py'],'assembly-reference.log').stdout,flush=True)
 cross=False
 if a.cross_prefix:
  run([a.cross_prefix+'gcc','-c','-march=rv32imafc','-mabi=ilp32f',e/'local_tts_int8_dot_p4.S','-o',out/'int8-pie.o'],'cross-assemble.log')
  z=run([a.cross_prefix+'objdump','-d',out/'int8-pie.o'],'int8-pie-disassembly.txt');assert 'esp.vmulas.s8.xacc' in z.stdout and 'esp.vmulas.s8.xacc.ld.ip' in z.stdout and 'esp.vmulas.s16' not in z.stdout;cross=True
 core=[d/('glass_local_'+n+'.c') for n in ('model','sha','tokenizer','llm','tts','frontend')]+[e/'local_tts_parallel.c',t/'local_voice_host.c']
 for backend,source,extra in [('w8a16','local_tts_pie.c',[]),('w8a8','local_tts_int8.c',['-Dglv_tts_pie_conv=glv_tts_int8_conv'])]:
  run([*flags,'-DGLV_TTS_PIE',*extra,*core,e/source,'-lm','-pthread','-o',out/backend],backend+'-build.log')
 rows=json.loads(a.cases.read_text())['cases'] if a.cases else [dict(id='hello',text='你好。')]
 env=dict({k:v for k,v in os.environ.items() if not k.startswith('GLV_TEST_')},GLV_TEST_MEMORY_LIMIT=str(12*1024*1024),GLV_TEST_TTS_WINDOWS='1',GLV_TEST_TTS_INPUT='1',GLV_TEST_TTS_PIE_PAR='1',GLV_TEST_TTS_COARSE='1')
 def case(row):
  paths=[]
  for backend in ('w8a16','w8a8'):
   raw=out/(row['id']+'-'+backend+'.f32');run([out/backend,'synthesize',models,row['text'],raw],raw.stem+'.log',env=env);paths.append(raw)
  x=array.array('f');x.frombytes(paths[0].read_bytes());y=array.array('f');y.frombytes(paths[1].read_bytes());assert len(x)==len(y) and len(x)>0 and all(math.isfinite(v) for v in x) and all(math.isfinite(v) for v in y)
  signal=math.fsum(v*v for v in x);noise=math.fsum((u-v)**2 for u,v in zip(x,y));other=math.fsum(v*v for v in y);dot=math.fsum(u*v for u,v in zip(x,y))
  snr=10*math.log10(signal/noise) if signal and noise else (300 if signal else 0);cosine=dot/math.sqrt(signal*other) if signal and other else 0
  result=dict(id=row['id'],text=row['text'],samples=len(x),sample_rate=24000,snr_db=snr,cosine=cosine,max_abs_error=max(abs(u-v) for u,v in zip(x,y)),reference_peak=max(map(abs,x)),int8_peak=max(map(abs,y)),reference_sha256=sha(paths[0]),int8_sha256=sha(paths[1]),numerical_screen_passed=snr>=30 and cosine>=.999)
  print('INT8_QUALITY',row['id'],'SNR',round(snr,2),'cosine',round(cosine,6),flush=True);return result
 with ThreadPoolExecutor(max_workers=2) as pool:checks=list(pool.map(case,rows))
 assert sha(models/'tts-v2/weights.bin')==original
 result=dict(implementation='vocoder convolution W8A8, INT32 block accumulation, FP32 scale/bias/layer outputs and remaining operators',host_execution_passed=True,unit_and_assembly_reference_passed=True,cross_assembled=cross,native_tested=False,flashed=False,quality_accepted=False,numerical_screen_passed=all(v['numerical_screen_passed'] for v in checks),quality_threshold=dict(snr_db=30,cosine=.999),scope='Numerical screening only; no perception, speaker, native timing or full-integer claim',original_model_sha256=original,original_model_unchanged=True,cases=checks,source_sha256={str(q.relative_to(repo)):sha(q) for q in [e/'local_tts_int8.c',e/'local_tts_int8.h',e/'local_tts_int8_dot_p4.S',t/'local_tts_int8_host.c',repo/'tools/local_voice/test_tts_int8_pipeline.py',Path(__file__).resolve()]})
 (out/'VERIFICATION.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n');print('INT8_HOST_EVALUATION_COMPLETE numerical_screen_passed='+str(result['numerical_screen_passed']),flush=True)
if __name__=='__main__':main()
