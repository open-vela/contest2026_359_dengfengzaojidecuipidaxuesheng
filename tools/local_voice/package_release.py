#!/usr/bin/env python3
"""Package explicit delivery files; never include raw device Flash data."""
import argparse,hashlib,json,zipfile
from pathlib import Path

def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser();p.add_argument('--artifacts',type=Path,required=True);a=p.parse_args();root=a.artifacts.resolve()
 board=json.loads((root/'device-tests4/BOARD-TESTS.json').read_text());cycles=json.loads((root/'device-cycles4-last/BOARD-CYCLES.json').read_text())
 assert board['passed'] and cycles['passed'],'Complete board software tests first'
 meta=json.loads((root/'candidate4/firmware/BUILD-METADATA.json').read_text())
 assert digest(root/'candidate4/firmware/nuttx.bin')==meta['sha256']['nuttx.bin']
 files={}
 def add(rel,source):
  assert '.private' not in Path(source).parts
  source=Path(source);assert source.is_file(),str(source);files[rel]=source
 for name in ('INSTALL.txt','PERFORMANCE.txt','ASR-NEXT.txt','ROLLBACK.txt','VERIFICATION.json','HARDWARE-ACCEPTANCE-PENDING.json','HOST-SERVICE-TIMING.json','MEMORY-RECOVERY-ANALYSIS.json'):
  add(name,root/name)
 for name in ('nuttx.bin','BUILD-METADATA.json','BLE-IDENTITY.json'):
  add('candidate4/firmware/'+name,root/'candidate4/firmware'/name)
 for name in ('ELF-AUDIT.json','XIP-REVIEW.json','RESET-ELF-AUDIT.json','HOTLOOP-AUDIT.json','EXPECTED-XIP-HASHES.json','MODEL-EQUIVALENCE.json','RAM-PCM-VERIFICATION.json'):
  add('candidate4/'+name,root/'candidate4'/name)
 for name in ('BOARD-CYCLES.json','MEMORY-FOLLOWUP.json'):
  add('device-cycles4/'+name,root/'device-cycles4'/name)
 for folder,name in (('device-cycles4-final','BOARD-CYCLES.json'),('device-cycles4-final','MEMDUMP-TOP.json'),('device-cycles4-last','BOARD-CYCLES.json'),('device-cycles4-drained','RECHECK.json')):
  add(folder+'/'+name,root/folder/name)
 add('local-voice-sd-models-20261031.zip',root/'local-voice-sd-models-20261031.zip')
 rollback=root.parent/'reset-wifi-fix-20261030/candidate2/firmware/nuttx.bin'
 assert digest(rollback)=='c3633017ff92da16d053801cc5dd436917c10b1b2ff7a14e8f73d17c9423b019'
 add('rollback/nuttx-known-good.bin',rollback)
 for directory,name in (('device-tests4','BOARD-TESTS.json'),('device-cycles4-last','BOARD-CYCLES.json'),('asr-evaluation','ASSESSMENT.json')):
  add(directory+'/'+name,root/directory/name)
 for source in sorted((root/'v2-ten').iterdir()):
  if source.suffix in ('.wav','.html','.json'):add('v2-ten/'+source.name,source)
 for name in ('light-local-dialogue.png','dark-local-reading.png'):
  add('ui-preview/'+name,root/'ui-review'/name)
 for directory in ('candidate3-engine-gates','candidate4-service-gates','candidate4-session-gates','wake-adapter-gates'):
  add(directory+'/VERIFICATION.json',root/directory/'VERIFICATION.json')
  fixture=root/directory/'test-source.c'
  if fixture.exists():add(directory+'/test-source.c',fixture)
 for folder,name in (('device-cycles4-final','BOARD-CYCLES.json'),('device-cycles4-drained','BOARD-CYCLES.json'),('device-cycles4-drained','RECHECK.json')):
  add(folder+'/'+name,root/folder/name)
 scope=json.loads((root/'candidate4/scope.json').read_text())
 add('candidate4/scope.json',root/'candidate4/scope.json')
 for item in scope['files']:
  source=root/'candidate4/source'/item['path'];assert digest(source)==item['sha256'],item['path'];add('source/'+item['path'],source)
 add('THIRD_PARTY.md',Path(__file__).resolve().parents[2]/'THIRD_PARTY.md')
 for source in sorted(Path(__file__).parent.glob('*.py')):add('tools/local_voice/'+source.name,source)
 manifest=dict(firmware_sha256=meta['sha256']['nuttx.bin'],ble_identity=meta['ble_identity'],asr_ready=False,physical_acceptance='See HARDWARE-ACCEPTANCE-PENDING.json',files=[dict(path=k,bytes=v.stat().st_size,sha256=digest(v)) for k,v in sorted(files.items())])
 out=root/'local-voice-app-candidate4-20261031.zip';assert not out.exists(),'Do not overwrite a published bundle'
 with zipfile.ZipFile(out,'x',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as z:
  for rel,source in sorted(files.items()):z.write(source,rel,compress_type=zipfile.ZIP_STORED if source.suffix=='.zip' else zipfile.ZIP_DEFLATED)
  z.writestr('DELIVERY-MANIFEST.json',json.dumps(manifest,indent=2)+'\n')
 with zipfile.ZipFile(out) as z:
  assert z.testzip() is None
  for item in manifest['files']:assert hashlib.sha256(z.read(item['path'])).hexdigest()==item['sha256']
 (root/'DELIVERY.json').write_text(json.dumps(dict(file=out.name,bytes=out.stat().st_size,sha256=digest(out),entries=len(files)+1),indent=2)+'\n')
 print('LOCAL_DELIVERY_VERIFIED',out.stat().st_size)
if __name__=='__main__':main()
