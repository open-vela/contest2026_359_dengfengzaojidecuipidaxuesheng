#!/usr/bin/env python3
"""Bind experimental Q4 proof to frozen P4 image, retaining all display gates."""
import argparse,hashlib,json,re
from pathlib import Path

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def read(p):return json.loads(p.read_text(encoding='utf-8'))

def main():
 p=argparse.ArgumentParser();p.add_argument('--candidate',type=Path,required=True);a=p.parse_args()
 c=a.candidate.resolve();old=c.parent/'candidate16';src=c/'source'
 meta=read(c/'firmware/BUILD-METADATA.json');prior=read(old/'firmware/BUILD-METADATA.json')
 assert sha(c/'firmware/nuttx.bin')==meta['sha256']['nuttx.bin']
 assert meta['local_q4_pie']==dict(included=True,default_bits=0,selftest_required=True,board_tested=False)
 assert (c/'firmware/resolved.config').read_bytes()==(old/'firmware/resolved.config').read_bytes()
 assert meta['prepared_kernel_sources']==prior['prepared_kernel_sources']
 assert meta['sd_read_diagnostics']==prior['sd_read_diagnostics']
 assert all(meta['ble_identity'][k]!=prior['ble_identity'][k] for k in ('name','address'))
 d='app/espdl-quickapp/overlay/apps/system/desktop/'
 expected={d+n for n in ('desktop_main.c','glass_local_model.c','glass_local_model.h','glass_local_llm.c','glass_local_voice.c','glass_local_voice.h')}
 expected|={'app/espdl-quickapp/experimental/'+n for n in ('local_q4_pie.c','local_q4_pie.h','local_q4_dot_p4.S')}
 changed={n for n in set(meta['source_sha256'])|set(prior['source_sha256']) if meta['source_sha256'].get(n)!=prior['source_sha256'].get(n)}
 assert changed==expected,changed
 for n,h in meta['source_sha256'].items():assert sha(src/n)==h,n
 # Only guarded dispatch may change generic matrix implementation/allocator.
 model=(src/d/'glass_local_model.c').read_text()
 model=re.sub(r'#ifdef GLV_Q4_PIE\n.*?\n#endif\n','',model,flags=re.S)
 assert model==(old/'source'/d/'glass_local_model.c').read_text()
 evidence=('q4-quality17-final/VERIFICATION.json','q4-service17/VERIFICATION.json','q4-dispatch17/VERIFICATION.json','q4-scalar17/VERIFICATION.json')
 for n in evidence:
  proof=read(c.parent/n);assert proof['passed'],n
  for path,h in proof['source_sha256'].items():assert sha(src/path)==h,(n,path)
 quality=read(c.parent/evidence[0]);assert quality['a16_passed'] and quality['unit_passed'] and len(quality['cases'])==22
 for n in ('ELF-AUDIT.json','XIP-REVIEW.json','RESET-ELF-AUDIT.json','HOTLOOP-AUDIT.json'):
  proof=read(c/n);assert proof['passed']
  if 'image_sha256' in proof:assert proof['image_sha256']==meta['sha256']['nuttx.bin']
  if 'elf_sha256' in proof:assert proof['elf_sha256']==sha(c/'firmware/nuttx.elf')
 asm=(c/'linked-disassembly.txt').read_text();bodies={}
 for block in re.split(r'(?=^[0-9a-f]+ <[^>]+>:\n)',asm,flags=re.M):
  m=re.match(r'([0-9a-f]+) <([^>]+)>:\n',block)
  if m:bodies[m[2]]=(int(m[1],16),block)
 address,body=bodies['glv_q4_dot32_pie'];assert address%4==0
 assert body.count('esp.vld.128.ip')==8 and body.count('esp.vmulas.s16.xacc')==4
 assert body.count('esp.zero.xacc')==1 and body.count('esp.srs.s.xacc')==1
 assert not re.search(r'\b(?:call|jal|csrw|csrrw|loop)\b',body)
 assert '<glv_q4_dot32_pie>' in bodies['glv_q4_matbatch'][1]
 assert all('<glv_q4_matbatch>' in bodies[n][1] for n in ('glv_matvec','glv_matbatch'))
 assert '<glv_q4_dot32_pie>' in bodies['glv_q4_selftest'][1]
 for n in ('glv_q4_matbatch','glv_q4_quantize32'):
  assert not re.search(r'__(?:mul|div|add|sub)df3|fmadd\.s',bodies[n][1]),n
 scope=read(c/'scope.json')
 for row in scope['files']:assert sha(src/row['path'])==row['sha256']
 report=dict(passed=True,image_sha256=meta['sha256']['nuttx.bin'],kernel_config_unchanged=True,
  scalar_default=True,weights_unchanged=True,native_gate_pending=True,
  scope='Host Q4/A16 approximation and service gates, actual linked PIE instructions; no native acceleration claim yet',
  candidate_source_sha256={r['path']:r['sha256'] for r in scope['files']},
  evidence_sha256={n:sha(c.parent/n) for n in evidence},compiled_change_paths=sorted(changed))
 (c/'Q4-VERIFICATION.json').write_text(json.dumps(report,indent=2)+'\n')
 print('Q4_P4_LINK_VERIFIED')

if __name__=='__main__':main()
