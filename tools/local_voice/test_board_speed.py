#!/usr/bin/env python3
"""One owned fixed-text request, two tokens then cancel; no recording/playback."""
import argparse,json,re,sys,time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from music_device_console import Console

def main():
    p=argparse.ArgumentParser();p.add_argument('--candidate',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    out=a.output.resolve();out.mkdir(exist_ok=False);private=out/'.private';private.mkdir();(private/'.gitignore').write_text('*\n')
    meta=json.loads((a.candidate/'firmware/BUILD-METADATA.json').read_text());gate=json.loads((a.candidate/'SPEED-VERIFICATION.json').read_text())
    assert gate['passed'] and gate['image_sha256']==meta['sha256']['nuttx.bin']
    report=dict(passed=False,firmware_sha256=meta['sha256']['nuttx.bin'],observations=[],scope='Actual scalar fixed-text two-token latency, IO profile, cancel and readiness check; no microphone or audio playback',sd_root_cause_cured=False)
    owned=0
    def save():(out/'BOARD-SPEED.json').write_text(json.dumps(report,indent=2)+'\n')
    save()
    with (private/'console.log').open('w',encoding='utf-8') as log:
        c=Console(log);c.serial.dtr=True
        def status():
            for attempt in range(5):
                raw=c.command('desktop local-voice status')+c.collect(.15)
                values={}
                for key,prefix in [('state','LOCAL_VOICE'),('diagnostics','LOCAL_VOICE_DIAG'),('io','LOCAL_VOICE_IO'),('q4','LOCAL_VOICE_Q4')]:
                    m=re.search(r'(?m)^'+prefix+r' (\w+=[^\n]+)',raw)
                    if m:values[key]={k:int(v) for k,v in re.findall(r'(\w+)=(-?\d+)',m[1])}
                text=re.search(r'(?m)^LOCAL_VOICE_TEXT bytes=(\d+) fnv=([0-9a-f]+)',raw)
                if set(values)!=set(('state','diagnostics','io','q4')) or not text:continue
                s=values.pop('state');s.update(values);s['reply_bytes']=int(text[1]);s['reply_fnv']=text[2]
                if not {'request','phase','busy','pending','ready','asr','enabled','error','tokens','first_audio_ms','first_token_ms'}<=s.keys():continue
                if s['io'].get('request')!=s['request'] or s['diagnostics'].get('request')!=s['request']:continue
                if not {'sha_bytes','sha_read_ms','sha_compute_ms','weight_bytes','read_ms','seek_ms','decode_ms','matmul_ms','verified_reuses'}<=s['io'].keys():continue
                return s
            raise RuntimeError('Incomplete observation; no inference replayed')
        def idle(seconds=180):
            until=time.monotonic()+seconds
            while time.monotonic()<until:
                s=status()
                if not s['busy'] and not s['pending']:return s
                c.collect(.5)
            raise TimeoutError('Worker did not retire')
        try:
            expected=json.loads((a.candidate/'EXPECTED-XIP-HASHES.json').read_text());xip=c.command('desktop xip')
            assert all(k+'_hash='+expected[k] in xip for k in ('text','rodata'))
            before=idle();report['before']=before;save()
            assert before['ready'] and before['asr'] and not before['error'] and not before['enabled']
            assert before['q4']['bits']==0
            assert 'active=0 ' in c.command('desktop ui recorder-status')
            raw=c.command('desktop local-voice chat "你好"')+c.collect(.2)
            ack=re.search(r'accepted=1 request=(\d+) error=0',raw);assert ack,'Missing acknowledgement; no replay'
            owned=int(ack[1]);report['request']=owned;save();until=time.monotonic()+300;first=None
            while time.monotonic()<until:
                s=status();report['observations'].append(s);save()
                assert s['request']==owned and s['busy'] and not s['error'] and not s['first_audio_ms'],'Request retired or failed; see retained diagnostics'
                if s['tokens'] and first is None:
                    first=s;report['first_token']=s;save()
                    assert s['io']['verified_reuses']==1,'Verified LLM stream was not reused'
                    assert s['reply_bytes']==6 and s['reply_fnv']=='86938da3','First text differs from frozen native reference'
                    print('NATIVE_FIRST_TOKEN',s['first_token_ms'],json.dumps(s['io']),flush=True)
                if s['tokens']>=2:
                    report['second_token']=s;report['observed_second_token_ms']=s['elapsed_ms']
                    report['observed_decode_step_ms']=s['elapsed_ms']-s['first_token_ms'];save();break
                c.collect(.4)
            else:raise TimeoutError('Two-token deadline exceeded; no replay')
            c.command('desktop local-voice new');retired=idle(30);report['retired']=retired;save()
            assert retired['request']==owned and retired['error']==-125 and not retired['reply_bytes'] and not retired['tokens'] and not retired['first_audio_ms']
            assert retired['diagnostics']['cancel_source']==5
            owned=0
            raw=c.command('desktop local-voice check');ack=re.search(r'accepted=1 request=(\d+) error=0',raw);assert ack,'No check acknowledgement; no replay'
            owned=int(ack[1]);recovered=idle();report['recovered']=recovered;save()
            assert recovered['request']==owned and not recovered['error'] and recovered['ready'] and recovered['asr'] and not recovered['first_audio_ms']
            owned=0;report['backlight_logical']=c.command('desktop ui backlight-status');report['passed']=True;save();print('NATIVE_SPEED_AND_CLEANUP_OK',flush=True)
        except BaseException as error:
            report['error']=type(error).__name__+': '+str(error)[:180]
            try:report['last_observation']=status()
            except Exception as observation_error:report['observation_error']=type(observation_error).__name__
            save();raise
        finally:
            if owned:
                try:
                    s=status()
                    if s['request']==owned and s['busy']:c.command('desktop local-voice stop');report['cleanup']=idle(30)
                except Exception as cleanup_error:report['cleanup_error']=type(cleanup_error).__name__
            c.close();save()
if __name__=='__main__':main()
