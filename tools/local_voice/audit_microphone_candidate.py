#!/usr/bin/env python3
"""Bind live-input candidate to host lifecycle, preserved math and native link."""
import argparse,hashlib,json,re,subprocess
from pathlib import Path
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--candidate',type=Path,required=True);p.add_argument('--toolchain',type=Path,required=True);a=p.parse_args()
 c=a.candidate.resolve();r=c.parent;s=c/'source';prior=r/'candidate9'
 meta=json.loads((c/'firmware/BUILD-METADATA.json').read_text(encoding='utf-8'))
 assert meta['local_asr']['evaluation_enabled'] and meta['local_asr']['microphone_enabled'] and not meta['local_asr']['board_tested']
 assert sha(c/'firmware/nuttx.bin')==meta['sha256']['nuttx.bin']
 for path,digest in meta['source_sha256'].items():assert sha(s/path)==digest,path
 for name in ('ELF-AUDIT.json','XIP-REVIEW.json','RESET-ELF-AUDIT.json','HOTLOOP-AUDIT.json'):
  report=json.loads((c/name).read_text(encoding='utf-8'));assert report['passed']
  if 'image_sha256' in report:assert report['image_sha256']==meta['sha256']['nuttx.bin']
 assert (c/'firmware/resolved.config').read_bytes()==(prior/'firmware/resolved.config').read_bytes()
 old=json.loads((prior/'firmware/BUILD-METADATA.json').read_text(encoding='utf-8'))
 assert meta['ble_identity']['address']!=old['ble_identity']['address'] and meta['ble_identity']['name']!=old['ble_identity']['name']
 d='app/espdl-quickapp/overlay/apps/system/desktop/'
 unchanged=[d+n for n in ('glass_local_model.c','glass_local_sha.c','glass_local_tokenizer.c','glass_local_llm.c','glass_local_tts.c','glass_local_wake.cpp','glass_custom_wake.c','glass_local_platform.c','glass_voice_capture.inc','glass_music_pcm.c')]
 unchanged.extend('app/espdl-quickapp/experimental/'+n for n in ('local_asr.c','local_asr.h','local_asr_graph.c','local_asr_graph.h','local_asr_ops.c','local_asr_ops.h','local_asr_fbank.c','local_asr_fbank.h'))
 for path in unchanged:assert (s/path).read_bytes()==(prior/'source'/path).read_bytes(),path
 def foreground(path):return path.read_text(encoding='utf-8').split('static int run_models(',1)[1].split('static int save_target(',1)[0]
 assert foreground(s/d/'glass_local_voice.c')==foreground(prior/'source'/d/'glass_local_voice.c')
 evidence=['microphone-worker11-final/VERIFICATION.json','microphone10-ui-final/VERIFICATION.json','regression11/VERIFICATION.json','service11-regression/VERIFICATION.json','capture-collector2/VERIFICATION.json','capture-native11/VERIFICATION.json','frontend11/VERIFICATION.json']
 for rel in evidence:
  report=json.loads((r/rel).read_text(encoding='utf-8'));assert report.get('passed') or report.get('status')=='passed',rel
  for path,digest in report.get('source_sha256',{}).items():assert sha(s/path)==digest,(rel,path)
 board=['device-asr9-sample0/BOARD-ASR.json','device-asr9-sample1/BOARD-ASR.json']
 for rel in board:assert json.loads((r/rel).read_text(encoding='utf-8'))['passed']
 elf=c/'firmware/nuttx.elf';tool=a.toolchain
 syms=subprocess.check_output([str(tool/'riscv32-esp-elf-nm'),'-n',str(elf)],text=True)
 names={n:int(v,16) for v,_,n in re.findall(r'^([0-9a-f]+)\s+(\S)\s+(\S+)$',syms,re.M)}
 required=('glv_capture_collect','glv_capture_platform','glv_asr_assets_check','glv_asr_transcribe_pcm','glv_asr_transcribe','glass_local_voice_listen','glass_local_voice_finish_capture','glass_local_voice_get_transcript')
 for n in required:assert 0x48000000<=names[n]<0x4c000000,(n,names.get(n))
 asm='\n'.join(subprocess.check_output([str(tool/'riscv32-esp-elf-objdump'),'-d','--disassemble='+n,str(elf)],text=True) for n in required)
 (c/'MICROPHONE-LINK-DISASSEMBLY.txt').write_text(asm,encoding='utf-8')
 assert 0<(c/'firmware/nuttx.bin').stat().st_size-(prior/'firmware/nuttx.bin').stat().st_size<256*1024
 report=dict(passed=True,image_sha256=meta['sha256']['nuttx.bin'],elf_sha256=sha(elf),scope='Live pipeline linked; actual host full-model chain and UI. No candidate11 microphone/screen hardware acceptance',board_microphone_tested=False,physical_screen_accepted=False,foreground_inference_body_unchanged=True,kernel_config_identical_to_candidate9=True,unchanged_model_math_and_audio={p:sha(s/p) for p in unchanged},linked_symbols={n:hex(names[n]) for n in required},candidate_source_sha256={f['path']:f['sha256'] for f in json.loads((c/'scope.json').read_text(encoding='utf-8'))['files']},evidence_sha256={rel:sha(r/rel) for rel in evidence+board})
 (c/'MICROPHONE-VERIFICATION.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8');print('LOCAL_MICROPHONE_CANDIDATE_LINK_GATES_OK',flush=True)
if __name__=='__main__':main()
