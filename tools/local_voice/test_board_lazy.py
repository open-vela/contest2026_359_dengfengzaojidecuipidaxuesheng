#!/usr/bin/env python3
"""One complete native text answer; verify idle startup and zero speech stages."""
import argparse,json,re,sys,time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from music_device_console import Console

def main():
    p=argparse.ArgumentParser();p.add_argument('--candidate',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    out=a.output.resolve();out.mkdir(exist_ok=False);private=out/'.private';private.mkdir();(private/'.gitignore').write_text('*\n')
    cache_metrics=a.candidate.name=='candidate22'
    gate=json.loads((a.candidate/('CACHE-VERIFICATION.json' if cache_metrics else 'LAZY-VERIFICATION.json')).read_text());assert gate['passed']
    reference=a.candidate.parent/gate.get('text_reference','lazy-final')
    golden=re.search(r'LAZY_CHAT_REPLY tokens=(\d+) bytes=(\d+) fnv=([0-9a-f]+)',(reference/'chat.log').read_text());assert golden
    meta=json.loads((a.candidate/'firmware/BUILD-METADATA.json').read_text());assert meta['sha256']['nuttx.bin']==gate['image_sha256']
    llm_bytes=json.loads((reference/'VERIFICATION.json').read_text())['llm_only_sha_bytes']
    report=dict(passed=False,firmware_sha256=gate['image_sha256'],observations=[],scope='Native warm-boot idle startup -> one complete fixed-text answer with no TTS/ASR/wake request; no microphone or playback',sd_root_cause_cured=False)
    def save():(out/'BOARD-LAZY.json').write_text(json.dumps(report,indent=2)+'\n')
    owned=0;save()
    with (private/'console.log').open('w',encoding='utf-8') as log:
        c=Console(log);c.serial.dtr=True
        def status():
            for _ in range(5):
                raw=c.command('desktop local-voice status')+c.collect(.12);values={}
                for key,prefix in [('state','LOCAL_VOICE'),('diagnostics','LOCAL_VOICE_DIAG'),('io','LOCAL_VOICE_IO'),('q4','LOCAL_VOICE_Q4'),('lazy','LOCAL_VOICE_LAZY'),('asr_diagnostics','LOCAL_VOICE_ASR')]:
                    m=re.search(r'(?m)^'+prefix+r' (\w+=[^\n]+)',raw)
                    if m:values[key]={k:int(v) for k,v in re.findall(r'(\w+)=(-?\d+)',m[1])}
                if cache_metrics:
                    m=re.search(r'(?m)^LOCAL_VOICE_CACHE (\w+=[^\n]+)',raw)
                    if m:values['cache']={k:int(v) for k,v in re.findall(r'(\w+)=(-?\d+)',m[1])}
                text=re.search(r'(?m)^LOCAL_VOICE_TEXT bytes=(\d+) fnv=([0-9a-f]+)',raw)
                expected=set(('state','diagnostics','io','q4','lazy','asr_diagnostics'))
                if cache_metrics:expected.add('cache')
                if set(values)!=expected or not text:continue
                s=values.pop('state');s.update(values);s['reply_bytes']=int(text[1]);s['reply_fnv']=text[2]
                s['capture_ms']=s['asr_diagnostics'].get('capture_ms',-1)
                if not {'request','phase','busy','pending','ready','asr','enabled','error','tokens','first_audio_ms','first_token_ms','capture_ms','peak'}<=s.keys():continue
                if s['io'].get('request')!=s['request'] or s['diagnostics'].get('request')!=s['request']:continue
                if cache_metrics and s['cache'].get('request')!=s['request']:continue
                return s
            raise RuntimeError('Incomplete observation; no request replayed')
        def idle(seconds=30):
            until=time.monotonic()+seconds
            while time.monotonic()<until:
                s=status()
                if not s['busy'] and not s['pending']:return s
                c.collect(.3)
            raise TimeoutError('Worker did not retire')
        try:
            expected=json.loads((a.candidate/'EXPECTED-XIP-HASHES.json').read_text());xip=c.command('desktop xip')
            assert all(k+'_hash='+expected[k] in xip for k in ('text','rodata'))
            before=idle();report['startup']=before;save()
            assert not any(before[k] for k in ('request','ready','asr','enabled','error','peak','tokens','first_audio_ms','capture_ms'))
            assert before['lazy']['default_text']==1 and before['io']['sha_bytes']==0 and before['q4']['bits']==0
            if cache_metrics:assert before['cache']['peak_bytes']==0 and before['cache']['hits']==0
            c.collect(3);still=idle();report['startup_after_idle']=still;save()
            assert not still['request'] and not still['io']['sha_bytes'] and not still['busy'] and not still['ready'] and not still['asr']
            assert 'active=0 ' in c.command('desktop ui recorder-status')
            raw=c.command('desktop local-voice chat "你好"')+c.collect(.15);ack=re.search(r'accepted=1 request=(\d+) error=0',raw)
            assert ack,'Missing acknowledgement; no replay';owned=int(ack[1]);report['request']=owned;save();until=time.monotonic()+480
            last_tokens=-1
            while time.monotonic()<until:
                s=status();report['observations'].append(s);save()
                assert s['request']==owned and not s['error'] and not s['first_audio_ms'] and not s['capture_ms'] and not s['asr'] and not s['enabled']
                assert s['phase'] in (0,1,2,3,14,15),'Unexpected speech/ASR stage in text request'
                assert s['tokens']<=int(golden[1])+1,'Unexpected generation; stopping owned request'
                if s['tokens']!=last_tokens:
                    if s['tokens']:print('NATIVE_TEXT_TOKENS',s['tokens'],'elapsed_ms',s['elapsed_ms'],flush=True)
                    last_tokens=s['tokens']
                if s['first_token_ms'] and 'first_token' not in report:
                    report['first_token']=s;save()
                    assert s['io']['sha_bytes']==llm_bytes and s['io']['verified_reuses']==1
                if not s['busy']:
                    report['completed']=s;save()
                    assert s['ready'] and s['tokens']==int(golden[1]) and s['reply_bytes']==int(golden[2]) and s['reply_fnv']==golden[3]
                    if cache_metrics:assert s['cache']['peak_bytes']>0 and s['cache']['hits']>0 and s['cache']['hit_bytes']>0
                    break
                c.collect(.5)
            else:raise TimeoutError('Text request deadline; no replay')
            owned=0;c.collect(3);settled=idle();report['settled']=settled
            assert settled['request']==s['request'] and settled['reply_fnv']==s['reply_fnv'] and not settled['error'] and not settled['first_audio_ms'] and not settled['asr'] and settled['io']['sha_bytes']==llm_bytes
            report['backlight_logical']=c.command('desktop ui backlight-status');report['passed']=True;save();print('NATIVE_LAZY_TEXT_COMPLETE_OK',flush=True)
        except BaseException as error:
            report['error']=type(error).__name__+': '+str(error)[:180]
            try:report['last_observation']=status()
            except Exception as observation_error:report['observation_error']=type(observation_error).__name__
            save();raise
        finally:
            if owned:
                try:
                    s=status()
                    if s['request']==owned and s['busy']:c.command('desktop local-voice stop');report['cleanup']=idle()
                except Exception as cleanup_error:report['cleanup_error']=type(cleanup_error).__name__
            c.close();save()
if __name__=='__main__':main()
