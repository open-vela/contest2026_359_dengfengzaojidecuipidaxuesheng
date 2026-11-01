#!/usr/bin/env python3
"""Verify actual EI listener progress, touch cancel and restart on the P4.

No audio is saved. Inference counters do not establish real-person accuracy.
"""
import argparse,json,math,re,sys,time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from music_device_console import Console

def main():
 p=argparse.ArgumentParser();p.add_argument('--candidate',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--playback',action='store_true');a=p.parse_args()
 out=a.output.resolve();out.mkdir(exist_ok=False);private=out/'.private';private.mkdir();report=dict(passed=False,samples=[],physical_wake_accuracy_accepted=False)
 def save():(out/'BOARD-WAKE.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
 save()
 with (private/'console.log').open('w',encoding='utf-8') as log:
  c=Console(log);c.serial.dtr=True
  def status(command,prefix,keys):
   for _ in range(5):
    raw=c.command(command);m=re.search(prefix+r'([^\n]+)',raw)
    if m:
     state={k:int(v) for k,v in re.findall(r'(\w+)=(-?\d+)',m[1])}
     score=re.search(r'\bscore=([^\s]+)',m[1])
     if score:
      state['score']=float(score[1]);assert math.isfinite(state['score']) and 0<=state['score']<=1
     if set(keys)<=state.keys():return state
   raise RuntimeError('Incomplete read-only status')
  def local():return status('desktop local-voice status',r'LOCAL_VOICE ',('request','busy','pending','ready','error','model_error','enabled','local_wake','phase'))
  def speech():return status('desktop speech status',r'(?m)^speech (?=active=)',('active','error','frames','evaluations','wakes'))
  def idle(timeout=180):
   until=time.monotonic()+timeout
   while time.monotonic()<until:
    s=local()
    if not s['busy'] and not s['pending']:return s
    c.collect(.3)
   raise TimeoutError('Local worker not idle')
  def listening(label,timeout=30):
   until=time.monotonic()+timeout;computed=False;active=False;last=None
   while time.monotonic()<until:
    s=speech();report['samples'].append(dict(label=label,**s));save()
    if s['error']:raise RuntimeError('Background wake error '+str(s['error']))
    active|=bool(s['active']);computed|=s['evaluations']>0 and s['frames']>=32000
    if active and computed and last is not None and (s['frames']>last['frames'] or s['wakes']>0):return s
    last=s;c.collect(1)
   raise TimeoutError('No actual classifier progress')
  try:
   expected=json.loads((a.candidate/'EXPECTED-XIP-HASHES.json').read_text());meta=json.loads((a.candidate/'firmware/BUILD-METADATA.json').read_text())
   xip=c.command('desktop xip',20);assert 'ready=1 protected_cpus=3' in xip and 'text_hash='+expected['text'] in xip and 'rodata_hash='+expected['rodata'] in xip
   report['firmware_sha256']=meta['sha256']['nuttx.bin'];s=idle();assert s['ready'] and not s['error'] and s['local_wake'] and s['enabled']
   assert 'active=0 ' in c.command('desktop ui recorder-status')
   listening('initial')
   c.command('desktop ui apps');c.collect(.3);c.command('desktop ui pointer-tap:524:566');c.collect(.3)
   assert '-> 1' in c.command('desktop ui click:本地语音')
   c.command('desktop local-voice read "你好。"');until=time.monotonic()+180
   while time.monotonic()<until:
    s=local()
    if s['phase']==6:break
    if not s['busy'] and s['error']:raise RuntimeError('Read start failed '+str(s['error']))
    c.collect(.3)
   else:raise TimeoutError('V2 synthesis not reached')
   assert not speech()['active'],'Old listener still owns audio'
   tree=c.command('desktop ui audit');assert '本地对话' in tree and '停止' in tree
   assert '-> 1' in c.command('desktop ui pointer-tap:376:492');s=idle(30)
   assert s['error']==-125 and s['ready'] and not s['busy'];report['touch_cancel']=s;save()
   listening('after-touch-cancel')
   for i in range(2):
    c.command('desktop local-voice wake off');s=idle(30);assert not s['enabled']
    until=time.monotonic()+10
    while speech()['active'] and time.monotonic()<until:c.collect(.2)
    assert not speech()['active']
    c.command('desktop local-voice wake on');s=idle(30);assert s['enabled'] and s['ready']
    listening('restart-'+str(i))
   if a.playback:
    c.command('desktop local-voice read "你好。"');s=idle(900)
    assert not s['error'] and s['first_audio_ms']>0 and s['peak']<=12*1024*1024
    report['v2_24k_completed']=s;save();listening('after-native-playback')
   report['final']=local();report['backlight_after']=c.command('desktop ui backlight-status')
   assert 'percent=100 ' in report['backlight_after'] and 'error=0' in report['backlight_after']
   report['passed']=True;save();print('LOCAL_EI_BOARD_WAKE_PROGRESS_OK',flush=True)
  except BaseException as error:
   report['error']=type(error).__name__+': '+str(error)[:200];save();raise
  finally:c.close()

if __name__=='__main__':main()
