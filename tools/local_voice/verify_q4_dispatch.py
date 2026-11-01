#!/usr/bin/env python3
"""Compare actual GLV_Q4_PIE matrix dispatch with frozen approximation outputs."""
import argparse,hashlib,json,subprocess
from pathlib import Path

def main():
 p=argparse.ArgumentParser();p.add_argument('--release',type=Path,required=True);p.add_argument('--quality',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
 root=Path(__file__).resolve().parents[2];d=root/'app/espdl-quickapp/overlay/apps/system/desktop';exp=root/'app/espdl-quickapp/experimental'
 out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
 harness=root/'app/espdl-quickapp/tests/local_q4_quality_host.c'
 transformed=harness.read_text().replace('assert(!bits||calls);','assert(!bits||memory.q4_matrix_calls);').replace('bits,calls,','bits,memory.q4_matrix_calls,')
 # Discard unreferenced linker-wrapper sections: all live calls now go through
 # the real conditional dispatcher, using the request's memory backend flag.
 (out/'dispatch.c').write_text(transformed)
 sources=[d/('glass_local_'+n+'.c') for n in ('model','sha','tokenizer','llm')]+[exp/'local_q4_pie.c']
 deps=sources+[harness,d/'glass_local_model.h',d/'glass_local_llm.h',d/'glass_local_tokenizer.h',exp/'local_q4_pie.h',Path(__file__)]
 hashes={str(f.relative_to(root)):hashlib.sha256(f.read_bytes()).hexdigest() for f in deps}
 subprocess.run(['gcc','-std=gnu11','-O2','-g','-ffp-contract=off','-fsanitize=address,undefined','-fno-pie','-no-pie','-ffunction-sections','-DGLV_Q4_PIE','-I'+str(d),'-I'+str(exp),*map(str,sources),str(out/'dispatch.c'),'-Wl,--gc-sections','-lm','-o',str(out/'dispatch')],check=True)
 cases=[]
 for mode,count in [('prefill',32),('context',33),('generate',32)]:
  for bits in (0,16):
   tag=f'0-{mode}-{count}-a{bits}';target=out/(tag+'.bin')
   run=subprocess.run([str(out/'dispatch'),str(bits),str(a.release/'edgeidiot'),str(a.quality/'prompt-0.txt'),mode,str(target),str(count)],capture_output=True,text=True,timeout=300)
   (out/(tag+'.log')).write_text(run.stdout+run.stderr);run.check_returncode()
   assert target.read_bytes()==(a.quality/(tag+'.bin')).read_bytes()
   stats=json.loads(run.stdout);assert (stats['matrix_calls']>0)==(bits==16)
   cases.append(dict(mode=mode,bits=bits,bit_exact_to_quality_path=True,stats=stats));print('DISPATCH_EXACT',mode,bits,flush=True)
 assert hashes=={str(f.relative_to(root)):hashlib.sha256(f.read_bytes()).hexdigest() for f in deps}
 (out/'VERIFICATION.json').write_text(json.dumps(dict(passed=True,source_sha256=hashes,cases=cases,scope='Actual guarded dispatch with host integer reference dot; no P4 speed claim'),indent=2)+'\n')

if __name__=='__main__':main()
