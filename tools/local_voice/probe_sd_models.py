#!/usr/bin/env python3
"""Read-only SD package stress; stop at first error, preserve wake preference."""
import argparse,json,re,sys,time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from music_device_console import Console

def main():
 p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--candidate',type=Path,required=True);p.add_argument('--cycles',type=int,default=6);a=p.parse_args();assert 1<=a.cycles<=20
 out=a.output.resolve();out.mkdir(exist_ok=False);expected=json.loads((a.candidate/'EXPECTED-XIP-HASHES.json').read_text());meta=json.loads((a.candidate/'firmware/BUILD-METADATA.json').read_text());report=dict(firmware_sha256=meta['sha256']['nuttx.bin'],checks=[],complete=False)
 def save():(out/'PROBE.json').write_text(json.dumps(report,indent=2)+'\n')
 save()
 with (out/'serial.log').open('w',encoding='utf-8') as log:
  c=Console(log);c.serial.dtr=True;original=None;changed=False
  def status():
   for _ in range(4):
    text=c.command('desktop local-voice status');m=re.search(r'LOCAL_VOICE (request=[^\n]+)',text)
    if m:
     s={k:int(v) for k,v in re.findall(r'(\w+)=(-?\d+)',m[1])}
     if {'request','busy','pending','error','ready','enabled'}<=s.keys():return s
   raise RuntimeError('Incomplete status')
  def wait():
   end=time.monotonic()+180
   while time.monotonic()<end:
    s=status()
    if not s['busy'] and not s['pending']:return s
    c.collect(.5)
   raise TimeoutError('SD check did not finish; no request replay')
  try:
   original=status();assert not original['busy'] and not original['pending'],'Another local request is active'
   assert re.search(r'RECORDER_INPUT active=0\b',c.command('desktop ui recorder-status')),'Recorder is active'
   speech=c.command('desktop speech status');assert 'active=0 ' in speech or '请说：你好 openvela' in speech,'Foreground speech is active'
   xip=c.command('desktop xip');assert 'text_hash='+expected['text'] in xip and 'rodata_hash='+expected['rodata'] in xip
   if original['enabled']:
    c.command('desktop local-voice wake off');wait();changed=True
   assert 'active=0 ' in c.command('desktop speech status'),'Wake capture did not yield'
   for i in range(a.cycles):
    c.command('desktop local-voice check');s=wait();report['checks'].append(s);save()
    print('SD_CHECK',i+1,'error='+str(s['error']),'elapsed_ms='+str(s['elapsed_ms']),flush=True)
    if s['error']:
     report['fault_stops_task']=not s['busy'];report['fault_invalidates_ready']=not s['ready'];break
   report['complete']=len(report['checks'])==a.cycles and all(not s['error'] for s in report['checks']);save()
  finally:
   if changed:
    s=status()
    if not s['busy'] and not s['pending']:
     c.command('desktop local-voice wake on');report['restored']=wait();save()
   c.close()
if __name__=='__main__':main()
