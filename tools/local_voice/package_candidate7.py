#!/usr/bin/env python3
"""Package the verified partial-plan candidate; never include private Flash/logs."""
import argparse,hashlib,json,zipfile
from pathlib import Path

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()

def main():
 p=argparse.ArgumentParser();p.add_argument('--artifacts',type=Path,required=True);a=p.parse_args();root=a.artifacts.resolve();candidate=root/'candidate7';docs=root/'delivery7-docs';project=Path(__file__).resolve().parents[2]
 meta=json.loads((candidate/'firmware/BUILD-METADATA.json').read_text(encoding='utf-8'))
 assert sha(candidate/'firmware/nuttx.bin')==meta['sha256']['nuttx.bin']
 for rel in ['device-wake7-final/BOARD-WAKE.json','device-wake-memory7-normalized/WAKE-MEMORY.json','device-tests5/BOARD-TESTS.json']:
  assert json.loads((root/rel).read_text(encoding='utf-8'))['passed'],rel
 deploy=json.loads((root/'device-flash7/DEPLOYMENT.json').read_text(encoding='utf-8'))
 assert deploy['independent_readback_verified'] and deploy['data_partition_preserved'] and deploy['runtime_checked']
 files={}
 def add(rel,source):
  source=Path(source);assert '.private' not in source.parts and source.is_file(),str(source)
  assert rel not in files;files[rel]=source
 for name in ['INSTALL.txt','PERFORMANCE.txt','ASR-PROGRESS.txt','HARDWARE-ACCEPTANCE.json','VALIDATION.json','MEMORY-RECOVERY.txt']:
  add(name,docs/name)
 public_deploy={key:deploy[key] for key in ['image_sha256','rollback_sha256','ble_identity','board_written','data_partition_preserved','independent_readback_verified','warm_boot_captured','runtime_checked']}
 public_deploy['source_report_sha256']=sha(root/'device-flash7/DEPLOYMENT.json')
 (docs/'DEPLOYMENT.json').write_text(json.dumps(public_deploy,indent=2)+'\n',encoding='utf-8');add('device-flash7/DEPLOYMENT.json',docs/'DEPLOYMENT.json')
 for name in ['nuttx.bin','BUILD-METADATA.json','BLE-IDENTITY.json']:add('candidate7/firmware/'+name,candidate/'firmware'/name)
 for name in ['ELF-AUDIT.json','XIP-REVIEW.json','RESET-ELF-AUDIT.json','HOTLOOP-AUDIT.json','EXPECTED-XIP-HASHES.json','OPTIMIZATION-VERIFICATION.json','NATIVE-ALIGNMENT-VERIFICATION.json']:
  add('candidate7/'+name,candidate/name)
 for rel in ['device-wake7-final/BOARD-WAKE.json','device-wake7-final/test-source.py','device-wake-memory7/WAKE-MEMORY.json','device-wake-memory7/test-source.py','device-wake-memory7-followup/FOLLOWUP.json','device-wake-memory7-normalized/WAKE-MEMORY.json','device-storage6/BOARD-STORAGE.json','device-storage6-final/BOARD-STORAGE.json','device-fd6/FD-SUMMARY.json']:
  add(rel,root/rel)
 board=json.loads((root/'device-tests5/BOARD-TESTS.json').read_text(encoding='utf-8'))
 public_board={key:board[key] for key in ['passed','firmware_sha256','steps','resource_samples','asr_ready','physical_screen_accepted','speaker_accepted','real_wake_accepted']}
 public_board['source_report_sha256']=sha(root/'device-tests5/BOARD-TESTS.json')
 (docs/'BOARD-FULL-CHAIN5.json').write_text(json.dumps(public_board,indent=2)+'\n',encoding='utf-8');add('device-tests5/BOARD-TESTS.json',docs/'BOARD-FULL-CHAIN5.json')
 for rel in ['optimization5/fast-gates/VERIFICATION.json','optimization5/ten-fast-exact/VERIFICATION.json','optimization5/kernel-benchmark/BENCHMARK.json','optimization7/wake-alignment-gates/VERIFICATION.json','optimization7/wake-alignment-gates/run.log','optimization7/service-gates/VERIFICATION.json','optimization7/service-gates/test-source.c','asr-evaluation/ASSESSMENT.json','asr-streaming-evaluation3/STREAMING-ASSESSMENT.json','asr-q4-evaluation/Q4-ASSESSMENT.json','asr-mixed-rows-evaluation/Q4-ASSESSMENT.json','asr-arithmetic-rows-port/VERIFICATION.json','asr-arithmetic-rows-port/PORT-STATUS.json','ASR-STREAMING-NEXT.txt','SD-PERFORMANCE-NEXT.txt']:
  add(rel,root/rel)
 for source in (root/'v2-ten').iterdir():
  if source.suffix in ('.wav','.html','.json'):add('v2-ten/'+source.name,source)
 for name in ['light-local-dialogue.png','dark-local-reading.png','light-local-settings.png']:add('ui-preview/'+name,root/'optimization6/ui-final'/name)
 add('local-voice-sd-models-20261031.zip',root/'local-voice-sd-models-20261031.zip')
 rollback=root.parent/'reset-wifi-fix-20261030/candidate2/firmware/nuttx.bin'
 assert sha(rollback)=='c3633017ff92da16d053801cc5dd436917c10b1b2ff7a14e8f73d17c9423b019'
 add('rollback/nuttx-known-good.bin',rollback)
 scope=json.loads((candidate/'scope.json').read_text(encoding='utf-8'));add('candidate7/scope.json',candidate/'scope.json')
 for item in scope['files']:
  source=candidate/'source'/item['path'];assert sha(source)==item['sha256'];assert not source.name.endswith(('.env','factory.h'))
  add('source/'+item['path'],source)
 for source in (project/'app/espdl-quickapp/experimental').glob('*'):
  if source.is_file():add('asr-experimental/'+source.name,source)
 add('asr-experimental/local_asr_ops_host.c',project/'app/espdl-quickapp/tests/local_asr_ops_host.c')
 for source in Path(__file__).parent.glob('*.py'):add('tools/local_voice/'+source.name,source)
 add('THIRD_PARTY.md',project/'THIRD_PARTY.md')
 manifest=dict(firmware_sha256=meta['sha256']['nuttx.bin'],plan_complete=False,asr_ready=False,files=[dict(path=rel,bytes=f.stat().st_size,sha256=sha(f)) for rel,f in sorted(files.items())])
 target=root/'local-voice-app-candidate7-20261031.zip';assert not target.exists(),'Do not replace an existing delivery'
 with zipfile.ZipFile(target,'x',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as z:
  for rel,source in sorted(files.items()):z.write(source,rel,compress_type=zipfile.ZIP_STORED if source.suffix=='.zip' else zipfile.ZIP_DEFLATED)
  z.writestr('DELIVERY-MANIFEST.json',json.dumps(manifest,indent=2)+'\n')
 with zipfile.ZipFile(target) as z:
  assert z.testzip() is None
  for item in manifest['files']:assert hashlib.sha256(z.read(item['path'])).hexdigest()==item['sha256']
 result=dict(file=target.name,bytes=target.stat().st_size,sha256=sha(target),entries=len(files)+1,plan_complete=False)
 (root/'DELIVERY7.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print('CANDIDATE7_DELIVERY_VERIFIED',result['bytes'])

if __name__=='__main__':main()
