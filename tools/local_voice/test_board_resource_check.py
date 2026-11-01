#!/usr/bin/env python3
"""Cancel native ASR-package validation and verify readiness recovers; no audio."""
import argparse,json,re,sys,time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from music_device_console import Console
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--candidate',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
 out=a.output.resolve();out.mkdir(exist_ok=False);private=out/'.private';private.mkdir();(private/'.gitignore').write_text('*\n')
 meta=json.loads((a.candidate/'firmware/BUILD-METADATA.json').read_text());expected=json.loads((a.candidate/'EXPECTED-XIP-HASHES.json').read_text())
 report=dict(passed=False,firmware_sha256=meta['sha256']['nuttx.bin'],scope='P4 real SD model validation: cancel during ASR package SHA check, preserve base text readiness, recheck/recover; no audio capture/playback or physical screen claim')
 def save():(out/'BOARD-RESOURCE-CHECK.json').write_text(json.dumps(report,indent=2)+'\n')
 save();owned=0
 with (private/'console.log').open('w',encoding='utf-8') as log:
  c=Console(log);c.serial.dtr=True
  def status():
   for _ in range(5):
    raw=c.command('desktop local-voice status')+c.collect(.2);m=re.search(r'(?m)^LOCAL_VOICE (request=[^\n]+)',raw)
    if not m:continue
    s={k:int(v) for k,v in re.findall(r'(\w+)=(-?\d+)',m[1])}
    if {'request','phase','busy','pending','ready','asr','error'}<=s.keys():return s
   raise RuntimeError('Incomplete read-only status; check not replayed')
  def start():
   raw=c.command('desktop local-voice check');m=re.search(r'accepted=1 request=(\d+) error=0',raw)
   assert m,'Missing check acknowledgement; no replay';return int(m[1])
  def wait(phase=None,request=None):
   until=time.monotonic()+300
   while time.monotonic()<until:
    s=status()
    if request is not None:assert s['request']==request,'Another request superseded this check'
    if phase is None and not s['busy'] and not s['pending']:return s
    if phase is not None:
     if s['busy'] and s['phase']==phase:return s
     assert s['busy'] and s['phase']==1,'ASR stage not observed; no replacement check'
    c.collect(.3)
   raise TimeoutError('Model check not observed to finish')
  try:
   raw=c.command('desktop xip');assert all(k+'_hash='+expected[k] in raw for k in ('text','rodata'))
   s=status();report['before']=s;save()
   if s['busy']:
    assert s['request']==1 and s['phase'] in (1,13),'Do not interrupt an existing foreground request';owned=s['request']
   else:owned=start()
   s=wait(13,owned);assert s['ready'];report['checking_asr']=s;save()
   began=time.monotonic();c.command('desktop local-voice stop');s=wait(request=owned)
   assert s['error']==-125 and s['ready'] and not s['asr'];report['cancelled']=s;report['observed_stop_seconds']=time.monotonic()-began;save();owned=0
   owned=start();s=wait(request=owned);assert not s['error'] and s['ready'] and s['asr'];report['recovered']=s;owned=0
   report['backlight_logical']=c.command('desktop ui backlight-status');report['passed']=True;save();print('P4_ASR_MODEL_CHECK_CANCEL_AND_RECOVERY_OK',flush=True)
  finally:
   if owned:
    try:
     s=status()
     if s['request']==owned and s['busy']:c.command('desktop local-voice stop');wait(request=owned)
    except Exception as error:report['cleanup_error']=type(error).__name__;save()
   c.close()
if __name__=='__main__':main()
