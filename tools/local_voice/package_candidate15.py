#!/usr/bin/env python3
"""Package adaptive prefill checkpoint with actual native timing and rollback."""
import argparse,hashlib,json,zipfile
from pathlib import Path
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--artifacts',type=Path,required=True);a=p.parse_args();r=a.artifacts.resolve();c=r/'candidate15';docs=r/'delivery15-docs';docs.mkdir(exist_ok=False)
 meta=json.loads((c/'firmware/BUILD-METADATA.json').read_text(encoding='utf-8'));gate=json.loads((c/'PREFILL-VERIFICATION.json').read_text(encoding='utf-8'));assert gate['passed'] and gate['image_sha256']==sha(c/'firmware/nuttx.bin')
 scope=json.loads((c/'scope.json').read_text(encoding='utf-8'));assert gate['candidate_source_sha256']=={row['path']:row['sha256'] for row in scope['files']}
 deployment=json.loads((r/'device-flash15/DEPLOYMENT.json').read_text(encoding='utf-8'));assert deployment['image_sha256']==meta['sha256']['nuttx.bin'] and all(deployment[k] for k in ('independent_readback_verified','data_partition_preserved','runtime_checked'))
 native=json.loads((r/'device-new-session15/BOARD-NEW-SESSION.json').read_text(encoding='utf-8'));assert native['passed'] and native['firmware_sha256']==meta['sha256']['nuttx.bin']
 public={k:deployment[k] for k in ('image_sha256','ble_identity','rollback_sha256','independent_readback_verified','data_partition_preserved','warm_boot_captured','runtime_checked')};(docs/'DEPLOYMENT.json').write_text(json.dumps(public,indent=2)+'\n',encoding='utf-8')
 acceptance=dict(plan_complete=False,screen_and_backlight='用户暂时无法确认；未通过',candidate13_native_new_session_clear_and_retire='历史候选13通过；本候选保留相同会话服务源码',candidate14_native_sd_model_reads='历史候选14三次校验通过',candidate15_native_partial_llm_new_session=True,native_microphone='候选11实测短采集/触摸停止；采集与DMA实现字节保留，非新真人测试',human_voice_test='用户已选择稍后再测',human_wake=False,live_chinese_chain=False,speaker_listening=False,cold_boot=False,new_ble_pairing=False,physical_card_removal=False,SD_transport='历史候选发生data-token ff读取错误；本候选沿用失败时的ticks/polls诊断，短期读取通过不代表根因修复')
 (docs/'HARDWARE-ACCEPTANCE.json').write_text(json.dumps(acceptance,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
 files={}
 def add(name,path):
  path=Path(path);assert path.is_file() and '.private' not in path.parts and name not in files;files[name]=path
 for name in ('DEPLOYMENT.json','HARDWARE-ACCEPTANCE.json'):add(name,docs/name)
 add('INSTALL.txt',c/'INSTALL.txt')
 add('known-issues/SD-FAILURE13.json',r/'SD-FAILURE13.json')
 add('known-issues/initial-native-attempt.json',r/'device-new-session13/BOARD-NEW-SESSION.json')
 add('known-issues/explicit-readonly-recheck.json',r/'device-sd-probe13-once/PROBE.json')
 for name in ('nuttx.bin','BUILD-METADATA.json','BLE-IDENTITY.json','resolved.config'):add('firmware/'+name,c/'firmware'/name)
 for name in ('ELF-AUDIT.json','XIP-REVIEW.json','RESET-ELF-AUDIT.json','HOTLOOP-AUDIT.json','EXPECTED-XIP-HASHES.json','PREFILL-VERIFICATION.json'):add('candidate15/'+name,c/name)
 for name,digest in gate['evidence_sha256'].items():assert sha(r/name)==digest;add('evidence/'+name,r/name)
 for rel in ('device-new-session13-recovered/BOARD-NEW-SESSION.json','device-microphone11-r5/BOARD-MICROPHONE.json','device-asr9-sample0/BOARD-ASR.json','device-asr9-sample1/BOARD-ASR.json'):
  if 'evidence/'+rel not in files:add('evidence/'+rel,r/rel)
 for name in ('light-local-asr-fault.png','light-local-listening.png','dark-local-reading.png'):add('ui-preview/'+name,r/'request-ui13-final'/name)
 if 'evidence/device-sd-read14/BOARD-SD-READ.json' not in files:add('evidence/device-sd-read14/BOARD-SD-READ.json',r/'device-sd-read14/BOARD-SD-READ.json')
 add('candidate14/DRIVER-REVIEW.txt',r/'candidate14/DRIVER-REVIEW.txt')
 add('sd-diagnostics/prepare_sd_read_diagnostics.py',c/'source/tools/prepare_sd_read_diagnostics.py')
 for name in ('mmcsd_spi-before.c','mmcsd_spi-diagnostic.c','receive-differential.c','result.log','before-prepared-final.checkpatch.log','diagnostic-prepared-final.checkpatch.log','VERIFICATION.json','STYLE-VERIFICATION.json'):
  add('sd-diagnostics/'+name,r/'sd-read-diagnostics14-host-final'/name)
 add('evidence/device-new-session15/BOARD-NEW-SESSION.json',r/'device-new-session15/BOARD-NEW-SESSION.json')
 add('PREFILL-PERFORMANCE.json',r/'PREFILL-PERFORMANCE15.json')
 add('source/scope.json',c/'scope.json')
 for row in scope['files']:
  f=c/'source'/row['path'];assert sha(f)==row['sha256'] and not f.name.endswith(('.env','factory.h'));add('source/candidate15/'+row['path'],f)
 for name in ('glass_local_voice.c','glass_local_voice.h'):add('source/reference-candidate12/'+name,r/'candidate12/source/app/espdl-quickapp/overlay/apps/system/desktop'/name)
 resources=r/'asr-device-model1/zipformer-14m-mixed-board8-20261031'
 for row in json.loads((resources/'manifest.json').read_text(encoding='utf-8'))['files']:assert sha(resources/row['path'])==row['sha256']
 for f in resources.rglob('*'):
  if f.is_file():add('sd-models/'+resources.name+'/'+f.relative_to(resources).as_posix(),f)
 add('local-voice-sd-models-20261031.zip',r/'local-voice-sd-models-20261031.zip')
 for n in (14,7):add('rollback/nuttx-candidate'+str(n)+'.bin',r/('candidate'+str(n))/'firmware/nuttx.bin')
 for f in (r/'v2-ten').iterdir():
  if f.suffix in ('.wav','.html','.json'):add('v2-ten/'+f.name,f)
 report=dict(plan_complete=False,firmware_sha256=meta['sha256']['nuttx.bin'],native_partial_llm_new_session_passed=True,first_token_ms=native['partial_answer']['first_token_ms'],sd_root_cause_cured=False,live_chinese_chain_accepted=False,files=[dict(path=n,bytes=f.stat().st_size,sha256=sha(f)) for n,f in sorted(files.items())]);manifest=docs/'DELIVERY-MANIFEST.json';manifest.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8');add(manifest.name,manifest)
 target=r/'local-voice-app-candidate15-20261031.zip';assert not target.exists()
 with zipfile.ZipFile(target,'x',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as z:
  for n,f in sorted(files.items()):z.write(f,n,compress_type=zipfile.ZIP_STORED if f.suffix=='.zip' else zipfile.ZIP_DEFLATED)
 with zipfile.ZipFile(target) as z:
  assert z.testzip() is None
  for row in report['files']:assert hashlib.sha256(z.read(row['path'])).hexdigest()==row['sha256']
 (r/'DELIVERY15.json').write_text(json.dumps(dict(file=target.name,bytes=target.stat().st_size,sha256=sha(target),plan_complete=False),indent=2)+'\n',encoding='utf-8');print('CANDIDATE15_ADAPTIVE_PREFILL_PACKAGE_VERIFIED',flush=True)
if __name__=='__main__':main()
