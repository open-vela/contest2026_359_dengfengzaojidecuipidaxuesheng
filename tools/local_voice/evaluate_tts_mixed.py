#!/usr/bin/env python3
"""Build actual SD-policy inference and validate against frozen host references."""
import argparse,json,os,subprocess,sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from calibrate_tts_int8 import metric,sha
def main():
 p=argparse.ArgumentParser();p.add_argument('--models',type=Path,required=True);p.add_argument('--policy',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--cases',type=Path,required=True);p.add_argument('--prototype',type=Path,required=True);a=p.parse_args()
 repo=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(exist_ok=False)
 d=repo/'app/espdl-quickapp/overlay/apps/system/desktop';e=repo/'app/espdl-quickapp/experimental';t=repo/'app/espdl-quickapp/tests'
 fixture=out/'fixture.c';s=(t/'local_voice_host.c').read_text();s=s.replace('  memory.tts_parallel_input=',"  memory.tts_int8=getenv(\"GLV_TEST_TTS_INT8\")!=NULL;\n  memory.tts_parallel_input=",1)
 s=s.replace('  assert(memory.used==0);', '  fprintf(stderr,"MIXED_PROFILE calls=%u kept=%u selftests8=%u selftests16=%u\\n",memory.profile.tts_int8_calls,memory.profile.tts_int8_kept_calls,memory.profile.tts_int8_selftests,memory.profile.tts_int16_selftests);\n  assert(memory.used==0&&!memory.tts_int8_policy&&!memory.tts_int8_owner);')
 fixture.write_text(s)
 cmd=['gcc','-std=gnu11','-O2','-g','-ffp-contract=off','-fno-pie','-no-pie','-DGLV_TTS_PIE','-DGLV_TTS_INT8','-DGLV_TTS_PARALLEL','-DGLV_TTS_FMA','-DGLV_TTS_INT8_POLICY_ROOT="'+str(a.policy.resolve())+'"','-I'+str(d),'-I'+str(e)]
 files=[d/('glass_local_'+n+'.c') for n in ('model','sha','tokenizer','llm','tts','frontend','assets')]+[e/n for n in ('local_tts_parallel.c','local_tts_pie.c','local_tts_int8.c','local_tts_int8_policy.c')]+[fixture]
 def run(cmd,name,**kw):
  r=subprocess.run(list(map(str,cmd)),capture_output=True,text=True,timeout=600,**kw);(out/name).write_text(r.stdout+r.stderr);r.check_returncode();return r
 run([*cmd,*files,'-lm','-pthread','-o',out/'host'],'build.log')
 base={k:v for k,v in os.environ.items() if not k.startswith(('GLV_TEST_','CALIB_'))};base.update(GLV_TEST_MEMORY_LIMIT=str(12*1024*1024),GLV_TEST_TTS_WINDOWS='1',GLV_TEST_TTS_INPUT='1',GLV_TEST_TTS_PIE_PAR='1',GLV_TEST_TTS_COARSE='1')
 rows=json.loads(a.cases.read_text())['cases']
 def case(row):
  outputs=[]
  for enabled in (False,True):
   mode='mixed' if enabled else 'baseline';raw=out/(row['id']+'-'+mode+'.f32');env=dict(base)
   if enabled:env['GLV_TEST_TTS_INT8']='1'
   z=run([out/'host','synthesize',a.models,row['text'],raw],raw.stem+'.log',env=env)
   outputs.append(raw)
  import re
  counts={k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',next(v for v in z.stderr.splitlines() if v.startswith('MIXED_PROFILE ')))}
  assert counts['calls']>0 and counts['kept']>0 and counts['selftests8']==counts['selftests16']==1
  reference=a.prototype/(row['id']+'-mixed-affine.f32');assert reference.read_bytes()==outputs[1].read_bytes(),('prototype mismatch',row['id'])
  v=metric(*outputs);v.update(id=row['id'],prototype_byte_exact=True,counters=counts);v['numerical_screen_passed']=v['snr_db']>=30 and v['cosine']>=.999
  print('MIXED_ACTUAL',row['id'],round(v['snr_db'],2),v['numerical_screen_passed'],flush=True);return v
 with ThreadPoolExecutor(max_workers=2) as pool:checks=list(pool.map(case,rows))
 result=dict(passed=True,scope='Actual firmware sources with SD policy, host numerical only',numerical_screen_passed=all(v['numerical_screen_passed'] for v in checks),quality_accepted=False,native_tested=False,cases=checks,policy_manifest_sha256=sha(a.policy/'manifest.bin'),policy_sha256=sha(a.policy/'policy.bin'),source_sha256={str(p.relative_to(repo)):sha(p) for p in files if p.is_relative_to(repo)})
 (out/'VERIFICATION.json').write_text(json.dumps(result,indent=2)+'\n');print('ACTUAL_MIXED_HOST_COMPLETE',flush=True)
if __name__=='__main__':main()
