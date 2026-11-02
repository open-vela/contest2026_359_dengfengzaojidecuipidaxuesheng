#!/usr/bin/env python3
"""Cancel real board inference after load; audit repeated user-heap recovery."""
import argparse,json,re,sys,time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from music_device_console import Console

def main():
 p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args()
 out=a.output.resolve();out.mkdir(exist_ok=False);report=dict(passed=False,cycles=[])
 def save():(out/'BOARD-CYCLES.json').write_text(json.dumps(report,indent=2)+'\n')
 save()
 with (out/'board.log').open('w',encoding='utf-8') as log:
  c=Console(log);c.serial.dtr=True
  def status():
   for _ in range(4):
    text=c.command('desktop local-voice status');m=re.search(r'LOCAL_VOICE (request=[^\n]+)',text)
    if m:
     s={k:int(v) for k,v in re.findall(r'(\w+)=(-?\d+)',m[1])}
     if {'request','phase','busy','pending','error','peak','ready','enabled'}<=s.keys():return s
   raise RuntimeError('Incomplete status')
  def wait_until(predicate,timeout):
   end=time.monotonic()+timeout
   while time.monotonic()<end:
    s=status()
    if predicate(s):return s
    c.collect(.3)
   raise TimeoutError('Board stage did not settle')
  def heap():
   text=c.command('free');m=re.search(r'(?m)^\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+\d+\s+\d+\s+Umem',text)
   if not m:raise RuntimeError('User-heap sample incomplete')
   return dict(free=int(m[3]),largest=int(m[5]))
  original=None;owned=set();configured=False
  try:
   original=status();assert not original['busy'] and not original['pending']
   c.command('desktop local-voice wake off');wait_until(lambda s:not s['busy'] and not s['pending'],30);c.collect(2);configured=True
   report['baseline_deferred_free_sync']=c.command('memdump biggest');report['baseline']=heap();save()
   for mode,phase in (('read',6),('chat',3),('read',6)):
    c.command('desktop local-voice '+mode+' "你好。"');owned.add(status()['request'])
    active=wait_until(lambda s:s['phase']==phase or (not s['busy'] and not s['pending']),300)
    assert active['busy'] and active['phase']==phase,'Inference failed before cancellation stage'
    live=heap();assert live['free']>=4*1024*1024
    began=time.monotonic();c.command('desktop local-voice stop')
    done=wait_until(lambda s:not s['busy'] and not s['pending'],30);elapsed=time.monotonic()-began
    assert done['request']==active['request'] and done['error']==-125
    c.collect(.5);immediate=heap();deferred_sync=c.command('memdump biggest');recovery=[];stable=0;until=time.monotonic()+10
    while time.monotonic()<until:
     recovered=heap();recovery.append(recovered)
     stable=stable+1 if recovered['free']>=report['baseline']['free']-16384 else 0
     if stable>=2:break
     c.collect(1)
    assert stable>=2,'Heap did not recover after stable sampling'
    report['cycles'].append(dict(mode=mode,cancel_phase=phase,cleanup_seconds=elapsed,active=live,immediate=immediate,deferred_free_sync=deferred_sync,recovered=recovered,recovery_samples=recovery,status=done));save()
    print('CYCLE',mode,phase,'cleanup_seconds='+str(round(elapsed,3)),flush=True)
   c.command('desktop local-voice check');owned.add(status()['request']);checked=wait_until(lambda s:not s['busy'] and not s['pending'],180);assert checked['ready']==1 and checked['error']==0
   report['final_check']=checked;report['directory_after']=c.command('ls /sdcard/local-voice')
  finally:
   try:
    current=status()
    if current['request'] in owned and current['busy']:
     c.command('desktop local-voice stop');wait_until(lambda s:not s['busy'] and not s['pending'],30)
    if configured and original and original['enabled']:
     c.command('desktop local-voice wake on');wait_until(lambda s:not s['busy'] and not s['pending'],30)
    report['restored']=status();save()
    if configured:assert report['restored']['enabled']==original['enabled']
   finally:c.close()
  report['passed']=True;save();print('BOARD_INFERENCE_CANCELLATION_CYCLES_PASSED',flush=True)
if __name__=='__main__':main()
