#!/usr/bin/env python3
"""Bind request isolation to targeted, full-model, UI and final native link tests."""
import argparse,hashlib,json,re,subprocess
from pathlib import Path
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--candidate',type=Path,required=True);p.add_argument('--toolchain',type=Path,required=True);a=p.parse_args()
 c=a.candidate.resolve();r=c.parent;s=c/'source';prior=r/'candidate12';d='app/espdl-quickapp/overlay/apps/system/desktop/'
 meta=json.loads((c/'firmware/BUILD-METADATA.json').read_text(encoding='utf-8'));old=json.loads((prior/'firmware/BUILD-METADATA.json').read_text(encoding='utf-8'))
 assert sha(c/'firmware/nuttx.bin')==meta['sha256']['nuttx.bin'] and meta['local_asr']['microphone_enabled']
 assert (c/'firmware/resolved.config').read_bytes()==(prior/'firmware/resolved.config').read_bytes()
 changed={path for path in set(old['source_sha256'])|set(meta['source_sha256']) if old['source_sha256'].get(path)!=meta['source_sha256'].get(path)};assert changed=={d+'glass_local_voice.c'},changed
 for path,digest in meta['source_sha256'].items():assert sha(s/path)==digest,path
 assert all(meta['ble_identity'][key]!=old['ble_identity'][key] for key in ('name','address'))
 for name in ('ELF-AUDIT.json','XIP-REVIEW.json','RESET-ELF-AUDIT.json','HOTLOOP-AUDIT.json'):
  report=json.loads((c/name).read_text(encoding='utf-8'));assert report['passed']
  if 'image_sha256' in report:assert report['image_sha256']==meta['sha256']['nuttx.bin']
 evidence=['request-isolation13-final-r2/VERIFICATION.json','resource-check13-final/VERIFICATION.json','request-ui13-final/VERIFICATION.json','microphone-worker13-final/VERIFICATION.json','service13-regression/VERIFICATION.json','regression13/VERIFICATION.json']
 for rel in evidence:
  report=json.loads((r/rel).read_text(encoding='utf-8'));assert report.get('passed') or report.get('status')=='passed',rel
  for path,digest in report.get('source_sha256',{}).items():assert sha(s/path)==digest,(rel,path)
 old_voice=(prior/'source'/d/'glass_local_voice.c').read_text(encoding='utf-8');new_voice=(s/d/'glass_local_voice.c').read_text(encoding='utf-8')
 def check_block(text):return text.split('#ifdef GLV_ASR_MICROPHONE\n    if(job==2)',1)[1].split('#endif',1)[0]
 assert check_block(old_voice)==check_block(new_voice)
 inherited=['device-resource-check12/BOARD-RESOURCE-CHECK.json','device-microphone11-r5/BOARD-MICROPHONE.json','device-asr9-sample0/BOARD-ASR.json','device-asr9-sample1/BOARD-ASR.json']
 for rel in inherited:assert json.loads((r/rel).read_text(encoding='utf-8'))['passed']
 elf=c/'firmware/nuttx.elf';syms=subprocess.check_output([str(a.toolchain/'riscv32-esp-elf-nm'),'-n',str(elf)],text=True);names={n:int(v,16) for v,_,n in re.findall(r'^([0-9a-f]+)\s+(\S)\s+(\S+)$',syms,re.M)}
 required=('glass_local_voice_new','glass_local_voice_get','glass_local_voice_get_transcript','glass_local_voice_listen','glv_asr_transcribe_pcm','glv_llm_generate','glv_tts_phones')
 for name in required:assert 0x48000000<=names[name]<0x4c000000
 result=dict(passed=True,image_sha256=meta['sha256']['nuttx.bin'],elf_sha256=sha(elf),scope='Request/transcript ownership and new-session isolation: deterministic late-engine tests, actual full C model chain, UI and service regressions; historical native records retain their original versions, not a new human test.',compiled_change_paths=sorted(changed),numeric_models_and_capture_bytes_unchanged=True,kernel_config_identical_to_candidate12=True,resource_check_block_unchanged=True,board_tested=False,candidate_source_sha256={v['path']:v['sha256'] for v in json.loads((c/'scope.json').read_text(encoding='utf-8'))['files']},evidence_sha256={rel:sha(r/rel) for rel in evidence+inherited},linked_symbols={name:hex(names[name]) for name in required})
 (c/'ISOLATION-VERIFICATION.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print('REQUEST_ISOLATION_CANDIDATE_FINAL_LINK_OK',flush=True)
if __name__=='__main__':main()
