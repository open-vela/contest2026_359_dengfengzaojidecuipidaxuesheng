#!/usr/bin/env python3
"""Observe stage/cancel diagnostics during one explicit real fixed-text test; no microphone."""
import argparse,json,re,sys,time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from music_device_console import Console
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--candidate',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--q4',action='store_true');a=p.parse_args()
 out=a.output.resolve();out.mkdir(exist_ok=False);private=out/'.private';private.mkdir();(private/'.gitignore').write_text('*\n')
 meta=json.loads((a.candidate/'firmware/BUILD-METADATA.json').read_text());expected=json.loads((a.candidate/'EXPECTED-XIP-HASHES.json').read_text());owned=0
 report=dict(observations=[],passed=False,firmware_sha256=meta['sha256']['nuttx.bin'],scope='P4 real text LLM partial answer -> New-session immediate clear -> cancel/retire -> readiness recheck. No microphone, human wake or speaker acceptance.')
 def save():(out/'BOARD-DIAGNOSTICS.json').write_text(json.dumps(report,indent=2)+'\n')
 save()
 with (private/'console.log').open('w',encoding='utf-8') as log:
  c=Console(log);c.serial.dtr=True
  def status():
   for _ in range(5):
    raw=c.command('desktop local-voice status')+c.collect(.2);m=re.search(r'(?m)^LOCAL_VOICE (request=[^\n]+)',raw);t=re.search(r'(?m)^LOCAL_VOICE_TEXT bytes=(\d+) fnv=([0-9a-f]+)',raw)
    d=re.search(r'(?m)^LOCAL_VOICE_DIAG (request=[^\n]+)',raw)
    if not m or not t or not d:continue
    s={k:int(v) for k,v in re.findall(r'(\w+)=(-?\d+)',m[1])};s['reply_bytes']=int(t[1]);s['reply_fnv']=t[2]
    diag={k:int(v) for k,v in re.findall(r'(\w+)=(-?\d+)',d[1])}
    if diag.get('request')!=s.get('request') or not {'cancel_source','cancel_phase','cancel_ms','prefill_ms','load_ms','check_ms','progress','total'}<=diag.keys():continue
    s['diagnostics']=diag
    if a.q4:
     q=re.search(r'(?m)^LOCAL_VOICE_Q4 (bits=[^\n]+)',raw)
     if not q:continue
     s['q4']={k:int(v) for k,v in re.findall(r'(\w+)=(-?\d+)',q[1])}
     if not {'bits','selftest','matrices','dots'}<=s['q4'].keys():continue
    if {'request','phase','busy','pending','ready','asr','enabled','error','first_audio_ms','tokens'}<=s.keys():return s
   raise RuntimeError('Incomplete read-only status; inference not replayed')
  def idle(seconds=300):
   until=time.monotonic()+seconds
   while time.monotonic()<until:
    s=status()
    if not s['busy'] and not s['pending']:return s
    c.collect(.3)
   raise TimeoutError('Worker not idle')
  try:
   raw=c.command('desktop xip');assert all(k+'_hash='+expected[k] in raw for k in ('text','rodata'))
   s=idle();report['before']=s;save();assert s['ready'] and s['asr'] and not s['enabled']
   if a.q4:
    assert not s['q4']['bits']
    raw=c.command('desktop local-voice q4-test');m=re.search(r'accepted=1 request=(\d+) error=0',raw);assert m,'Selftest acknowledgement missing; no replay'
    owned=int(m[1]);s=idle(30);report['native_q4_selftest']=s;save();assert s['request']==owned and not s['error'] and s['q4']['selftest'];owned=0
    raw=c.command('desktop local-voice backend pie16');assert 'accepted=1 request=0 error=0' in raw
    assert status()['q4']['bits']==16
   raw=c.command('desktop local-voice chat "你好"')+c.collect(.2);m=re.search(r'accepted=1 request=(\d+) error=0',raw);assert m,'Missing acknowledgment; no replay';owned=int(m[1]);report['request']=owned;save()
   until=time.monotonic()+600
   while time.monotonic()<until:
    s=status();report['observations'].append(s);save()
    assert s['request']==owned and s['busy'] and not s['error'] and not s['first_audio_ms'], 'Request retired before first token; see diagnostics'
    if s['tokens'] and s['reply_bytes']:break
    c.collect(.5)
   else:raise TimeoutError('No partial answer; inference not replayed')
   report['partial_answer']=s;save();began=time.monotonic();c.command('desktop local-voice new');s=status()
   if a.q4:
    partial=report['partial_answer'];assert partial['q4']['bits']==16 and partial['q4']['matrices']>0 and partial['q4']['dots']>0
    assert partial['reply_bytes']==6 and partial['reply_fnv']=='86938da3','Accelerated first text differs from frozen native reference'
   assert s['request']==owned and not s['reply_bytes'] and s['reply_fnv']=='811c9dc5' and not s['first_audio_ms'];report['immediate_clear']=s;report['observed_clear_seconds']=time.monotonic()-began;save()
   s=idle(30);assert s['request']==owned and s['error']==-125 and not s['reply_bytes'] and not s['first_audio_ms'];assert s['diagnostics']['cancel_source']==5 and s['diagnostics']['cancel_phase']==3
   report['retired']=s;owned=0;save()
   raw=c.command('desktop local-voice check');m=re.search(r'accepted=1 request=(\d+) error=0',raw);assert m;owned=int(m[1]);s=idle();assert s['request']==owned and s['ready'] and s['asr'] and not s['error'] and not s['reply_bytes'];assert s['diagnostics']['cancel_source']==0
   report['recovered']=s;owned=0
   if a.q4:
    assert 'accepted=1 request=0 error=0' in c.command('desktop local-voice backend scalar')
    report['restored_scalar']=status();assert report['restored_scalar']['q4']['bits']==0
   report['backlight_logical']=c.command('desktop ui backlight-status');report['passed']=True;save();print('P4_REAL_LLM_DIAGNOSTICS_OK',flush=True)
  except BaseException as error:
   report['error']=type(error).__name__+': '+str(error)[:160]
   try:report['last_observation']=status()
   except Exception as observation_error:report['observation_error']=type(observation_error).__name__
   save();raise
  finally:
   if owned:
    try:
     s=status()
     if s['request']==owned and s['busy']:c.command('desktop local-voice stop');idle(30)
    except Exception as error:report['cleanup_error']=type(error).__name__;save()
   if a.q4:
    try:
     s=status()
     if not s['busy']:c.command('desktop local-voice backend scalar')
    except Exception as error:report['backend_cleanup_error']=type(error).__name__;save()
   c.close()
if __name__=='__main__':main()
