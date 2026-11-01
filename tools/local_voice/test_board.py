#!/usr/bin/env python3
"""Real-model board test; no user audio capture, no appliance control."""
import argparse,json,re,sys,time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from music_device_console import Console
def main():
 p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--candidate',type=Path);p.add_argument('--resume-chat',action='store_true');p.add_argument('--skip-standalone-read',action='store_true');a=p.parse_args();out=a.output.resolve();out.mkdir(exist_ok=True)
 report=json.loads((out/'BOARD-TESTS.json').read_text()) if a.resume_chat else dict(passed=False,physical_screen_accepted=False,speaker_accepted=False,real_wake_accepted=False,asr_ready=False,steps=[])
 def save():(out/'BOARD-TESTS.json').write_text(json.dumps(report,indent=2)+'\n')
 save()
 with (out/'board.log').open('a' if a.resume_chat else 'w',encoding='utf-8') as log:
  c=Console(log);c.serial.dtr=True
  def status():
   required={'request','phase','busy','ready','asr','local_wake','enabled','pending','error','peak','elapsed_ms','first_token_ms','first_audio_ms','tokens'}
   for attempt in range(4):
    text=c.command('desktop local-voice status');m=re.search(r'LOCAL_VOICE (request=[^\n]+)',text)
    if m:
     s={k:int(v) for k,v in re.findall(r'(\w+)=(-?\d+)',m[1])}
     if required<=s.keys():return s
   raise RuntimeError('Incomplete status after read-only retries')
  def wait(label,timeout=900):
   began=time.monotonic();previous=None;last_heap=began
   while time.monotonic()-began<timeout:
    s=status();key=(s['phase'],s['busy'],s['tokens'])
    if key!=previous:
     print(label,'phase='+str(s['phase']),'tokens='+str(s['tokens']),'seconds='+str(round(time.monotonic()-began)),flush=True);previous=key
     report['current']=dict(label=label,**s);save()
    if not s['busy'] and not s['pending']:
     report['steps'].append(dict(label=label,**s));save();return s
    if time.monotonic()-last_heap>=60:
     heap=c.command('free');m=re.search(r'(?m)^\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+\d+\s+\d+\s+Umem',heap)
     if m:
      sample=dict(label=label,phase=s['phase'],tokens=s['tokens'],free=int(m[3]),largest=int(m[5]),elapsed_since_attach=int(time.monotonic()-began))
      report.setdefault('resource_samples',[]).append(sample);save();assert sample['free']>=4*1024*1024,'PSRAM reserve violated'
     last_heap=time.monotonic()
    c.collect(1)
   c.command('desktop local-voice stop');raise TimeoutError(label)
  try:
   if a.candidate:
    meta=json.loads((a.candidate/'firmware/BUILD-METADATA.json').read_text());expected=json.loads((a.candidate/'EXPECTED-XIP-HASHES.json').read_text())
    xip=c.command('desktop xip',20)
    assert 'ready=1 protected_cpus=3' in xip and 'text_hash='+expected['text'] in xip and 'rodata_hash='+expected['rodata'] in xip,'Wrong running firmware'
    report['firmware_sha256']=meta['sha256']['nuttx.bin'];report['xip_binding']=True;save()
   if not a.resume_chat:
    initial=wait('startup-sha',180);assert initial['ready']==1 and initial['asr']==0 and initial['error']==0
    c.command('desktop ui apps');c.collect(.5)
    c.command('desktop ui pointer-tap:524:566');c.collect(.5)
    opened=c.command('desktop ui click:本地语音');assert '-> 1' in opened
    report['app_open_submitted']=True;report['heap_before']=c.command('free');save()
    c.command('desktop local-voice read "你好。"');c.collect(.1);c.command('desktop local-voice stop')
    stopped=wait('cancel-read',30);assert stopped['busy']==0 and stopped['error']!=0
    if not a.skip_standalone_read:
     c.command('desktop local-voice read "你好。"');spoken=wait('v2-read',900)
     assert not spoken['error'] and spoken['first_audio_ms']>0 and spoken['peak']<=12*1024*1024
     report['v2_native_24k_completed']=True
    else:report['standalone_read_skipped']='V2 engine unchanged from candidate2 board read; full answer playback still required below'
    save();c.command('desktop local-voice chat "你好"')
   else:
    assert status()['request']==report['current']['request'],'Request changed; do not replay a conversation'
    report.setdefault('task_snapshots',[]).append(c.command('ps'));save()
   answer=wait('edgeidiot-v2',3600)
   assert not answer['error'] and answer['first_token_ms']>0 and answer['first_audio_ms']>0 and answer['tokens']>0 and answer['peak']<=12*1024*1024
   report['text_llm_v2_completed']=True;report['v2_native_24k_completed']=True;save()
   for enabled in ('off','on'):
    c.command('desktop local-voice wake '+enabled);s=wait('wake-'+enabled,30);assert not s['error'] and s['enabled']==(enabled=='on')
    if enabled=='off':assert 'active=0 ' in c.command('desktop speech status')
   for target in ('assistant','local'):
    c.command('desktop local-voice wake '+target);s=wait('wake-target-'+target,30);assert s['error']==0 and s['local_wake']==(target=='local')
   c.command('desktop local-voice new');report['heap_after']=c.command('free')
   report['backlight_after']=c.command('desktop ui backlight-status');assert 'percent=100 ' in report['backlight_after'] and 'error=0' in report['backlight_after']
   report['display_after']=c.command('desktop cameras');report['passed']=True;save()
   print('LOCAL_VOICE_BOARD_SERVICE_PASSED; physical screen, speaker and real-person wake still separate',flush=True)
  finally:c.close()
if __name__=='__main__':main()
