#!/usr/bin/env python3
"""Audit native wake allocation recovery after actual SDK inference cycles."""
import argparse,json,re,sys,time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from music_device_console import Console

def main():
 p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--candidate',type=Path,required=True);a=p.parse_args();out=a.output.resolve();out.mkdir(exist_ok=False);private=out/'.private';private.mkdir()
 report=dict(passed=False,cycles=[],scope='P4 EI cycles; identical read-only package check before baseline/post samples, then current-core memdump. This exercises normal allocation to drain per-CPU delayed frees; 16 KiB tolerance unchanged.')
 def save():(out/'WAKE-MEMORY.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
 save()
 with (private/'console.log').open('w',encoding='utf-8') as log:
  c=Console(log);c.serial.dtr=True;original=None
  def status(command,pattern,required):
   for _ in range(5):
    m=re.search(pattern,c.command(command))
    if m:
     s={k:int(v) for k,v in re.findall(r'(\w+)=(-?\d+)',m[1]) if k!='score'}
     if set(required)<=s.keys():return s
   raise RuntimeError('Incomplete status')
  def local():return status('desktop local-voice status',r'LOCAL_VOICE (request=[^\n]+)',('request','ready','busy','pending','enabled','local_wake','error'))
  def speech():return status('desktop speech status',r'(?m)^speech (active=[^\n]+)',('active','error','frames','evaluations'))
  def idle():
   end=time.monotonic()+30
   while time.monotonic()<end:
    s=local()
    if not s.get('busy',1) and not s.get('pending',1):return s
    c.collect(.2)
   raise TimeoutError('Configuration pending')
  def off():
   c.command('desktop local-voice wake off');assert not idle()['enabled'];end=time.monotonic()+10
   while time.monotonic()<end:
    if not speech().get('active',1):return
    c.collect(.2)
   raise TimeoutError('Capture did not retire')
  def on():
   c.command('desktop local-voice wake on');assert idle()['enabled'];end=time.monotonic()+30
   while time.monotonic()<end:
    s=speech()
    if s.get('error'):raise RuntimeError('Wake failed '+str(s['error']))
    if s.get('active') and s.get('evaluations',0)>0 and s.get('frames',0)>=32000:return s
    c.collect(.4)
   raise TimeoutError('No inference progress')
  def heap():
   raw=c.command('free');m=re.search(r'(?m)^\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+\d+\s+\d+\s+Umem',raw)
   assert m,'Incomplete heap';return dict(free=int(m[3]),largest=int(m[5]))
  def normalized():
   c.command('desktop local-voice check');c.collect(55)
   # Do not launch another desktop CLI task before measuring; it can leave a
   # fresh 32 KiB stack on the other CPU's delayed queue.
   c.command('memdump biggest');samples=[heap(),heap()];s=local()
   assert s['ready'] and not s['busy'] and not s['pending'] and not s['error']
   return samples,s
  try:
   expected=json.loads((a.candidate/'EXPECTED-XIP-HASHES.json').read_text());meta=json.loads((a.candidate/'firmware/BUILD-METADATA.json').read_text());xip=c.command('desktop xip',20)
   assert 'ready=1 protected_cpus=3' in xip and 'text_hash='+expected['text'] in xip and 'rodata_hash='+expected['rodata'] in xip
   report['firmware_sha256']=meta['sha256']['nuttx.bin']
   original=idle();assert original['ready'] and original['local_wake'] and not original['error']
   off();samples,checked=normalized();baseline=max(samples,key=lambda v:v['free']);report['baseline']=baseline;report['baseline_samples']=samples;report['baseline_check']=checked;save()
   for i in range(3):
    inference=on();active=heap();assert active['free']>=4*1024*1024
    off();samples,checked=normalized()
    report['current']=dict(index=i,inference=inference,active=active,recovered=samples,check=checked);save()
    assert all(v['free']>=baseline['free']-16384 for v in samples),'Wake heap did not recover'
    report['cycles'].append(report['current']);save()
   report['checks_passed']=True
  except BaseException as error:
   report['error']=type(error).__name__+': '+str(error)[:200];save();raise
  finally:
   try:
    if original and original['enabled']:report['restored_listener']=on()
    report['final']=local();save()
   finally:c.close()
 report['passed']=bool(report.get('checks_passed'));save();print('NATIVE_WAKE_HEAP_RECOVERY_OK',flush=True)

if __name__=='__main__':main()
