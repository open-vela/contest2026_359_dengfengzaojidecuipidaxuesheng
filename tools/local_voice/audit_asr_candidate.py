#!/usr/bin/env python3
"""Bind candidate 8 to the tested ASR worker and preserved native baseline."""
import argparse,hashlib,json,re,subprocess
from pathlib import Path


def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--candidate',type=Path,required=True);p.add_argument('--toolchain',type=Path,required=True);p.add_argument('--worker-gates',default='asr-worker-gates1/VERIFICATION.json');p.add_argument('--native-gates',default='asr-native-audit3/VERIFICATION.json');a=p.parse_args()
    c=a.candidate.resolve();r=c.parent;prior=r/'candidate7';source=c/'source'
    meta=json.loads((c/'firmware/BUILD-METADATA.json').read_text(encoding='utf-8'));assert meta['local_asr']['evaluation_enabled'] and not meta['local_asr']['microphone_ready']
    assert sha(c/'firmware/nuttx.bin')==meta['sha256']['nuttx.bin']
    for path,digest in meta['source_sha256'].items():assert sha(source/path)==digest,path
    for name in ('ELF-AUDIT.json','XIP-REVIEW.json','RESET-ELF-AUDIT.json','HOTLOOP-AUDIT.json'):
        assert json.loads((c/name).read_text(encoding='utf-8'))['passed']
    assert (c/'firmware/resolved.config').read_bytes()==(prior/'firmware/resolved.config').read_bytes(),'Unplanned kernel config change'
    old_meta=json.loads((prior/'firmware/BUILD-METADATA.json').read_text(encoding='utf-8'))
    assert meta['ble_identity']['address']!=old_meta['ble_identity']['address'] and meta['ble_identity']['name']!=old_meta['ble_identity']['name']
    desktop='app/espdl-quickapp/overlay/apps/system/desktop/'
    unchanged={}
    for name in ('glass_local_model.c','glass_local_tokenizer.c','glass_local_llm.c','glass_local_frontend.c','glass_local_tts.c','glass_local_wake.cpp','glass_custom_wake.c','glass_local_platform.c'):
        path=desktop+name;assert (source/path).read_bytes()==(prior/'source'/path).read_bytes(),name;unchanged[path]=sha(source/path)
    def foreground(path):return path.read_text(encoding='utf-8').split('static int run_models(',1)[1].split('static int save_target(',1)[0]
    assert foreground(source/desktop/'glass_local_voice.c')==foreground(prior/'source'/desktop/'glass_local_voice.c')
    ui=(source/desktop/'glass_local_voice_ui.inc').read_text(encoding='utf-8')
    assert ui.replace(',"加载离线转写","离线转写中"','')==(prior/'source'/desktop/'glass_local_voice_ui.inc').read_text(encoding='utf-8')
    evidence=[a.worker_gates,'service8-regression/VERIFICATION.json','regression8/VERIFICATION.json',a.native_gates]
    for rel in evidence:
        report=json.loads((r/rel).read_text(encoding='utf-8'));assert report.get('passed') or report.get('status')=='passed'
        if rel==a.worker_gates:
            for path,digest in report['source_sha256'].items():assert sha(source/path)==digest,path
    assets=json.loads((r/'device-asr-install8/DEPLOYMENT.json').read_text(encoding='utf-8'));assert assets['published'] and not assets['overwrite']
    worker=json.loads((r/evidence[0]).read_text(encoding='utf-8'));assert assets['source_manifest_sha256']==worker['asr_resource_manifest_sha256']
    elf=c/'firmware/nuttx.elf';objdump=a.toolchain/'riscv32-esp-elf-objdump';nm=a.toolchain/'riscv32-esp-elf-nm'
    symbols=subprocess.check_output([str(nm),'-n',str(elf)],text=True);names={v[2]:int(v[0],16) for v in re.findall(r'^([0-9a-f]+)\s+(\S)\s+(\S+)$',symbols,re.M)}
    disassemblies={};double_calls={}
    required=('glv_asr_test_file','glv_asr_open','glv_asr_transcribe','glv_asr_graph_run','glv_asr_fbank_pcm','glv_asr_matmul','glv_asr_conv','glass_local_voice_asr_test')
    for name in required:
        assert 0x48000000<=names[name]<0x4c000000,(name,names.get(name))
        asm=subprocess.check_output([str(objdump),'-d','--disassemble='+name,str(elf)],text=True);disassemblies[name]=asm
        assert not re.search(r'\bf(?:madd|msub|nmadd|nmsub)\.s\b',asm),(name,'unreviewed fused arithmetic')
        calls=re.findall(r'<(__[^>]*(?:df|sfdf)[^>]*)>',asm);assert not calls,(name,calls);double_calls[name]=calls
    assert len(disassemblies)>0
    (c/'ASR-LINK-DISASSEMBLY.txt').write_text('\n'.join(disassemblies.values()),encoding='utf-8')
    old_size=(prior/'firmware/nuttx.bin').stat().st_size;new_size=(c/'firmware/nuttx.bin').stat().st_size
    assert new_size-old_size<256*1024,'Unexpected image growth; inspect for embedded model data'
    result=dict(passed=True,image_sha256=meta['sha256']['nuttx.bin'],elf_sha256=sha(elf),scope='ASR sample evaluation linked; no microphone readiness or native transcription claimed yet',asr_ready=False,board_tested=False,unchanged_math_wake_platform=unchanged,foreground_inference_body_unchanged=True,ui_only_adds_asr_phase_labels=True,kernel_config_identical_to_candidate7=True,additional_image_bytes=new_size-old_size,linked_asr_symbols={v:hex(names[v]) for v in required},native_float_no_fma_or_double_helpers=double_calls,candidate_source_sha256={v['path']:v['sha256'] for v in json.loads((c/'scope.json').read_text(encoding='utf-8'))['files']},evidence_sha256={v:sha(r/v) for v in evidence},asr_sd_manifest_sha256=assets['source_manifest_sha256'])
    (c/'ASR-EVALUATION-VERIFICATION.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print('ASR_CANDIDATE_FINAL_LINK_AND_BASELINE_OK',new_size-old_size,flush=True)


if __name__=='__main__':main()
