#!/usr/bin/env python3
"""Package an audited local microphone checkpoint, preserving pending acceptance."""
import argparse,hashlib,json,zipfile
from pathlib import Path
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--artifacts',type=Path,required=True);a=p.parse_args();r=a.artifacts.resolve();c=r/'candidate11';docs=r/'delivery11-docs';docs.mkdir(exist_ok=False)
 meta=json.loads((c/'firmware/BUILD-METADATA.json').read_text(encoding='utf-8'))
 gate=json.loads((c/'MICROPHONE-VERIFICATION.json').read_text(encoding='utf-8'));assert gate['passed'] and gate['image_sha256']==sha(c/'firmware/nuttx.bin')
 deployment=json.loads((r/'device-flash11/DEPLOYMENT.json').read_text(encoding='utf-8'))
 assert deployment['image_sha256']==meta['sha256']['nuttx.bin'] and all(deployment[k] for k in ('independent_readback_verified','data_partition_preserved','runtime_checked'))
 microphone=json.loads((r/'device-microphone11-r5/BOARD-MICROPHONE.json').read_text(encoding='utf-8'));assert microphone['passed'] and microphone['firmware_sha256']==meta['sha256']['nuttx.bin']
 public={k:deployment[k] for k in ('image_sha256','ble_identity','rollback_sha256','independent_readback_verified','data_partition_preserved','warm_boot_captured','runtime_checked')}
 (docs/'DEPLOYMENT.json').write_text(json.dumps(public,indent=2)+'\n',encoding='utf-8')
 acceptance=dict(plan_complete=False,screen_and_backlight='用户暂时无法确认；未通过',native_microphone_cancel='通过短采集与触摸停止；不等于真人中文链路通过',human_wake=False,live_chinese_chain=False,speaker_listening=False,cold_boot=False,new_ble_pairing=False,physical_card_removal=False)
 (docs/'HARDWARE-ACCEPTANCE.json').write_text(json.dumps(acceptance,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
 files={}
 def add(name,path):
  path=Path(path);assert path.is_file() and '.private' not in path.parts and name not in files;files[name]=path
 for name in ('DEPLOYMENT.json','HARDWARE-ACCEPTANCE.json'):add(name,docs/name)
 add('INSTALL.txt',c/'INSTALL.txt')
 for name in ('nuttx.bin','BUILD-METADATA.json','BLE-IDENTITY.json','resolved.config'):add('firmware/'+name,c/'firmware'/name)
 for name in ('ELF-AUDIT.json','XIP-REVIEW.json','RESET-ELF-AUDIT.json','HOTLOOP-AUDIT.json','EXPECTED-XIP-HASHES.json','MICROPHONE-VERIFICATION.json'):add('candidate11/'+name,c/name)
 for name in gate['evidence_sha256']:assert sha(r/name)==gate['evidence_sha256'][name];add('evidence/'+name,r/name)
 add('evidence/device-microphone11-r5/BOARD-MICROPHONE.json',r/'device-microphone11-r5/BOARD-MICROPHONE.json')
 add('evidence/host-real-chain.log',r/'microphone-worker11-final/run.log')
 for name in ('light-local-listening.png','dark-local-reading.png','light-local-settings.png'):add('ui-preview/'+name,r/'microphone10-ui-final'/name)
 scope=json.loads((c/'scope.json').read_text(encoding='utf-8'));add('source/scope.json',c/'scope.json')
 for row in scope['files']:
  f=c/'source'/row['path'];assert sha(f)==row['sha256'] and not f.name.endswith(('.env','factory.h'));add('source/candidate11/'+row['path'],f)
 resources=r/'asr-device-model1/zipformer-14m-mixed-board8-20261031'
 for row in json.loads((resources/'manifest.json').read_text(encoding='utf-8'))['files']:assert sha(resources/row['path'])==row['sha256']
 for f in resources.rglob('*'):
  if f.is_file():add('sd-models/'+resources.name+'/'+f.relative_to(resources).as_posix(),f)
 add('local-voice-sd-models-20261031.zip',r/'local-voice-sd-models-20261031.zip')
 for n in (9,7):add('rollback/nuttx-candidate'+str(n)+'.bin',r/('candidate'+str(n))/'firmware/nuttx.bin')
 for f in (r/'v2-ten').iterdir():
  if f.suffix in ('.wav','.html','.json'):add('v2-ten/'+f.name,f)
 report=dict(plan_complete=False,firmware_sha256=meta['sha256']['nuttx.bin'],native_microphone_cancel_passed=True,live_chinese_chain_accepted=False,files=[dict(path=n,bytes=f.stat().st_size,sha256=sha(f)) for n,f in sorted(files.items())])
 manifest=docs/'DELIVERY-MANIFEST.json';manifest.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8');add(manifest.name,manifest)
 target=r/'local-voice-app-candidate11-20261031.zip';assert not target.exists()
 with zipfile.ZipFile(target,'x',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as z:
  for n,f in sorted(files.items()):z.write(f,n,compress_type=zipfile.ZIP_STORED if f.suffix=='.zip' else zipfile.ZIP_DEFLATED)
 with zipfile.ZipFile(target) as z:
  assert z.testzip() is None
  for row in report['files']:assert hashlib.sha256(z.read(row['path'])).hexdigest()==row['sha256']
 (r/'DELIVERY11.json').write_text(json.dumps(dict(file=target.name,bytes=target.stat().st_size,sha256=sha(target),plan_complete=False),indent=2)+'\n',encoding='utf-8');print('CANDIDATE11_CHECKPOINT_PACKAGE_VERIFIED',flush=True)
if __name__=='__main__':main()
