#!/usr/bin/env python3
"""Bind ASR-check fix to targeted tests and preserved full model chain."""
import argparse,hashlib,json,re,subprocess
from pathlib import Path
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--candidate',type=Path,required=True);p.add_argument('--toolchain',type=Path,required=True);a=p.parse_args()
 c=a.candidate.resolve();r=c.parent;s=c/'source';prior=r/'candidate11';d='app/espdl-quickapp/overlay/apps/system/desktop/'
 meta=json.loads((c/'firmware/BUILD-METADATA.json').read_text(encoding='utf-8'));assert sha(c/'firmware/nuttx.bin')==meta['sha256']['nuttx.bin']
 assert meta['local_asr']['microphone_enabled'] and (c/'firmware/resolved.config').read_bytes()==(prior/'firmware/resolved.config').read_bytes()
 for path,digest in meta['source_sha256'].items():assert sha(s/path)==digest,path
 for name in ('ELF-AUDIT.json','XIP-REVIEW.json','RESET-ELF-AUDIT.json','HOTLOOP-AUDIT.json'):
  report=json.loads((c/name).read_text(encoding='utf-8'));assert report['passed']
  if 'image_sha256' in report:assert report['image_sha256']==meta['sha256']['nuttx.bin']
 old=json.loads((prior/'firmware/BUILD-METADATA.json').read_text(encoding='utf-8'));assert all(meta['ble_identity'][k]!=old['ble_identity'][k] for k in ('address','name'))
 allowed={d+'glass_local_voice.c',d+'glass_local_voice.h',d+'glass_local_voice_ui.inc'}
 changed={path for path in set(old['source_sha256'])|set(meta['source_sha256']) if old['source_sha256'].get(path)!=meta['source_sha256'].get(path)}
 assert changed==allowed,changed
 old_voice=(prior/'source'/d/'glass_local_voice.c').read_text(encoding='utf-8');new_voice=(s/d/'glass_local_voice.c').read_text(encoding='utf-8')
 marker='#ifdef GLV_ASR_MICROPHONE\n    if(job==2)'
 def except_check(text):
  assert text.count(marker)==1
  before,after=text.split(marker);return before,after.split('#endif',1)[1]
 assert except_check(old_voice)==except_check(new_voice),'Unexpected capture/LLM/TTS/service change'
 assert (s/d/'glass_local_voice.h').read_text(encoding='utf-8').replace(',GLV_CHECKING_ASR','')==(prior/'source'/d/'glass_local_voice.h').read_text(encoding='utf-8')
 # The old full-chain test remains historical evidence. Reuse requires exact
 # source equality for every other compiled engine and the above whole-file
 # exclusion proof for the one changed check block.
 inherited=['microphone-worker11-final/VERIFICATION.json','frontend11/VERIFICATION.json','capture-native11/VERIFICATION.json']
 for rel in inherited:
  report=json.loads((r/rel).read_text(encoding='utf-8'));assert report['passed']
  for path,digest in report['source_sha256'].items():
   if path==d+'glass_local_voice.c':assert sha(prior/'source'/path)==digest
   else:assert sha(s/path)==digest,(rel,path)
 for name in ('glass_local_assets.c','glass_local_platform.c','glass_voice_capture.inc','glass_local_capture.c','glass_local_capture_device.c','glass_wake_mini.inc'):
  assert (s/d/name).read_bytes()==(prior/'source'/d/name).read_bytes(),name
 evidence=['resource-check12-final/VERIFICATION.json','resource-ui12-final/VERIFICATION.json','regression12/VERIFICATION.json']
 for rel in evidence:
  report=json.loads((r/rel).read_text(encoding='utf-8'));assert report['passed']
  for path,digest in report.get('source_sha256',{}).items():assert sha(s/path)==digest,(rel,path)
 syms=subprocess.check_output([str(a.toolchain/'riscv32-esp-elf-nm'),'-n',str(c/'firmware/nuttx.elf')],text=True)
 names={n:int(v,16) for v,_,n in re.findall(r'^([0-9a-f]+)\s+(\S)\s+(\S+)$',syms,re.M)}
 required=('glass_local_voice_check','glv_asr_assets_check','glass_local_voice_listen','glv_capture_platform','glv_asr_transcribe_pcm','glv_llm_generate','glv_tts_phones')
 for name in required:assert 0x48000000<=names[name]<0x4c000000
 result=dict(passed=True,image_sha256=meta['sha256']['nuttx.bin'],elf_sha256=sha(c/'firmware/nuttx.elf'),scope='ASR package-check stop/media handling and UI causes fixed; entire service source outside check block and all model/capture math preserved from candidate11. C11 full-chain evidence inherited, not relabeled as a new native live-voice test.',check_block_is_only_service_change=True,existing_phase_values_unchanged=True,model_chain_unchanged=True,kernel_config_identical_to_candidate11=True,board_tested=False,candidate_source_sha256={f['path']:f['sha256'] for f in json.loads((c/'scope.json').read_text(encoding='utf-8'))['files']},evidence_sha256={rel:sha(r/rel) for rel in evidence+inherited},linked_symbols={name:hex(names[name]) for name in required})
 (c/'RESOURCE-VERIFICATION.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print('RESOURCE_CHECK_CANDIDATE_LINK_AND_CHAIN_PRESERVATION_OK',flush=True)
if __name__=='__main__':main()
