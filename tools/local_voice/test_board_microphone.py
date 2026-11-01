#!/usr/bin/env python3
"""Short native RAM capture/cancel only; no saved recording or full inference."""
import argparse,json,re,sys,time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from music_device_console import Console
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--candidate',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
 out=a.output.resolve();out.mkdir(exist_ok=False);private=out/'.private';private.mkdir();(private/'.gitignore').write_text('*\n')
 meta=json.loads((a.candidate/'firmware/BUILD-METADATA.json').read_text());expected=json.loads((a.candidate/'EXPECTED-XIP-HASHES.json').read_text())
 report=dict(passed=False,firmware_sha256=meta['sha256']['nuttx.bin'],scope='Short P4 native microphone capture -> touch cancel -> DMA/session cleanup; no stored audio, full live Chinese, human wake, speaker or screen acceptance',cycles=[],physical_screen_accepted=False,speaker_accepted=False,live_chinese_accepted=False)
 def save():(out/'BOARD-MICROPHONE.json').write_text(json.dumps(report,indent=2)+'\n')
 save()
 with (private/'console.log').open('w',encoding='utf-8') as log:
  c=Console(log);c.serial.dtr=True;active=None
  def status():
   for _ in range(5):
    raw=c.command('desktop local-voice status')+c.collect(.2);m=re.search(r'(?m)^LOCAL_VOICE (request=[^\n]+)',raw);t=re.search(r'(?m)^LOCAL_VOICE_ASR ([^\n]+)',raw)
    if not m or not t:continue
    s={k:int(v) for k,v in re.findall(r'(\w+)=(-?\d+)',m[1])};s['capture']={k:int(v) for k,v in re.findall(r'(\w+)=(-?\d+)',t[1])}
    if {'request','busy','pending','ready','asr','enabled','error','phase'}<=s.keys() and 'text_bytes' in s['capture']:return s
   raise RuntimeError('Incomplete read-only status; capture not replayed')
  def idle(seconds=300):
   until=time.monotonic()+seconds
   while time.monotonic()<until:
    s=status()
    if not s['busy'] and not s['pending']:return s
    c.collect(.2)
   raise TimeoutError('Worker not idle')
  def heap():
   raw=c.command('free');m=re.search(r'(?m)^\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+\d+\s+\d+\s+Umem',raw);assert m
   value=dict(free=int(m[3]),largest=int(m[5]));assert value['free']>=4*1024*1024;return value
  try:
   xip=c.command('desktop xip');assert all(k+'_hash='+expected[k] in xip for k in ('text','rodata'))
   s=idle();assert s['ready'] and s['asr'] and not s['enabled'],'Keep existing global wake off for this isolated short capture test'
   assert 'active=0 ' in c.command('desktop ui recorder-status')
   c.command('desktop ui apps');opened=c.command('desktop ui click:本地语音')
   if not re.search(r'click .* -> 1',opened):
    c.command('desktop ui pointer-tap:524:566');opened=c.command('desktop ui click:本地语音')
   assert re.search(r'click .* -> 1',opened),'App entry was not opened; no capture started'
   for _ in range(5):
    raw=c.command('desktop ui audit')+c.collect(.3);rows=[]
    # Independent Wi-Fi diagnostics may interleave individual JSON rows. Only
    # use complete, strictly decoded control records; retry missing decisions.
    for line in raw.splitlines():
     try:row=json.loads(line.strip().strip(','))
     except json.JSONDecodeError:continue
     if isinstance(row,dict) and {'id','parent','type','x','y','w','h','text'}<=row.keys():rows.append(row)
    by_id={row['id']:row for row in rows}
    def button(text):
     label=next(row for row in rows if row['type']=='label' and row['text']==text);parent=by_id[label['parent']];assert parent['type']=='button'
     return 'desktop ui pointer-tap:'+str(parent['x']+parent['w']//2)+':'+str(parent['y']+parent['h']//2)
    try:stop_button,mic_button=button('停止'),button('语音输入');break
    except (KeyError,StopIteration,AssertionError):continue
   else:raise RuntimeError('Incomplete native control geometry; no recording started')
   report['native_buttons']={'stop':stop_button,'microphone':mic_button}
   report['heap_before']=heap();save()
   for i in range(2):
    previous=s['request']
    active=-1 # Lost command acknowledgments still require cancel/cleanup.
    if not i:
     raw=c.command('desktop local-voice listen');m=re.search(r'accepted=1 request=(\d+) error=0',raw);assert m;active=int(m[1])
    else:c.command(mic_button)
    c.collect(.3);s=status();assert s['request']!=previous and s['busy'] and s['phase']==12;snapshot=s
    active=s['request'];report['cycles'].append(dict(recording=snapshot));save()
    if i:
     raw=c.command('desktop local-voice finish '+str(previous));assert 'accepted=0 ' in raw
    began=time.monotonic();c.command(stop_button);s=idle(20)
    assert s['request']==active and s['error']==-125 and s['ready'] and s['asr'] and s['capture']['text_bytes']==0
    report['cycles'][-1].update(stopped=s,observed_stop_seconds=time.monotonic()-began,heap=heap());save();active=None
   assert report['cycles'][-1]['heap']['free']>=report['heap_before']['free']-128*1024
   c.command('desktop local-voice check');s=idle();assert s['ready'] and s['asr'] and not s['error'];report['final']=s
   report['backlight_logical']=c.command('desktop ui backlight-status');report['passed']=True;save();print('P4_LOCAL_MICROPHONE_CAPTURE_AND_TOUCH_CANCEL_OK',flush=True)
  finally:
   if active:
    try:c.command('desktop local-voice stop');idle(20)
    except Exception as error:report['cleanup_error']=type(error).__name__;save()
   c.close()
if __name__=='__main__':main()
