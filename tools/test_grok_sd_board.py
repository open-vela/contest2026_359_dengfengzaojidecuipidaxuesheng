#!/usr/bin/env python3
"""Measure actual SD playback and exercise all public expressions on the board."""
import argparse,json,re,time,hashlib
from pathlib import Path
from grok_board_io import Console
def main():
    p=argparse.ArgumentParser();p.add_argument('--port',default='COM4');p.add_argument('--output',type=Path,required=True);p.add_argument('--firmware',type=Path,required=True);a=p.parse_args();a.output.mkdir(exist_ok=True,parents=True)
    sdk=(Path(__file__).resolve().parents[1]/'ouo/vendor/grok-ball.js').read_text(encoding='utf-8');registry=re.findall(r"id: '([0-9]{2})', name: '([^']+)', group: '([^']+)'",sdk);assert len(registry)==32
    results={'firmware_sha256':hashlib.sha256(a.firmware.read_bytes()).hexdigest(),'method':'actual board SD player counters / COM4','samples':[],'expressions':[]}
    with (a.output/'grok-performance-console.log').open('w',encoding='utf-8') as log:
        c=Console(a.port,log);c.command('')
        def status(name):
            text=c.command('desktop ui grok-status');row=next((v for v in text.splitlines() if 'grok-sd /sdcard/grok/'+name+'.gbs ' in v),None)
            assert row,'Missing player: '+name
            fields={k:int(v) for k,v in re.findall(r'(?:^|\s)(id|frames|reads|sd_reads|cache_hits|cache_bytes|cache_total|mean_us|max_us|errors|error|buffer|ready|visible)=(-?\d+)',row)};assert fields['ready']==1 and fields['error']==0,row;return fields
        def click(text):
            reply=c.command('desktop ui click:'+text);assert '-> 1' in reply,reply
        def sample(name,seconds=10):
            before=status(name);began=time.monotonic();time.sleep(seconds);after=status(name);elapsed=time.monotonic()-began;fps=(after['frames']-before['frames'])/elapsed
            assert fps>6,(name,fps)
            assert after['errors']==before['errors'],'New SD read error during measurement'
            results['samples'].append(dict(player=name,elapsed=elapsed,frames=after['frames']-before['frames'],fps=fps,status=after));print('BOARD_FPS',name,round(fps,2),'buffer',after['buffer'],flush=True)
        for theme,palette in [('light','white'),('dark','black')]:
            c.command('desktop ui home');c.command('desktop ui '+theme);time.sleep(2);sample('home-'+palette)
        c.command('desktop ui light');c.command('desktop ui ouo');time.sleep(2);original_id=status('app-white')['id'];sample('app-white')
        for id,name,group in registry:
            click('表情');click({'life':'生命周期','emo':'情绪反应','emotion':'情绪反应','agent':'工作状态'}.get(group,'情绪反应'))
            click(id+'  '+name);time.sleep(.25);state=status('app-white');assert state['id']==int(id),(id,state)
            results['expressions'].append({'id':id,'name':name,'passed':True});print('BOARD_EXPRESSION_OK',id,name,flush=True)
        click('表情');before=status('app-white');time.sleep(1);after=status('app-white');assert before['frames']==after['frames'],'Picker did not pause player';click('关闭')
        click('巡演');time.sleep(3);click('停止');sample('app-white',5)
        c.command('desktop ui touch:down:0.5:0.43');c.command('desktop ui touch:up:0.5:0.43');sample('app-white',4)
        for i in range(5):c.command('desktop ui home');c.command('desktop ui ouo');time.sleep(.4);status('app-white')
        original=next(v for v in registry if int(v[0])==original_id);click('表情');click({'life':'生命周期','agent':'工作状态'}.get(original[2],'情绪反应'));click(original[0]+'  '+original[1])
        c.command('desktop ui home');c.command('desktop ui dark');c.command('desktop ui ouo');time.sleep(1);sample('app-black',10)
        results['entry_cycles']=5;c.command('desktop ui home');c.command('desktop ui chat');time.sleep(1);sample('mini-white',5)
        results['chat_status']=c.command('desktop ui chat-status')
        c.command('desktop ui home');c.command('desktop ui dark');c.command('free');c.command('ps');c.close()
    (a.output/'grok-performance.json').write_text(json.dumps(results,ensure_ascii=False,indent=2));print('GROK_SD_BOARD_OK',flush=True)
if __name__=='__main__':main()
