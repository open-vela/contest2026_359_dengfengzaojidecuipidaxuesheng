#!/usr/bin/env python3
"""Test native SD clip ASR, cancel/resume and actual PSRAM reserve.

No user microphone/audio is captured. A lost observation never resubmits the
model task: --resume requires the exact saved request ID still on the board.
"""
import argparse,hashlib,json,re,sys,time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from music_device_console import Console


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--candidate',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--sample',type=int,choices=(0,1),default=0);p.add_argument('--resume',action='store_true');p.add_argument('--skip-cancel',action='store_true');p.add_argument('--wake-test',action='store_true',help='Temporarily enable local global wake for handoff tests, then restore its original settings');p.add_argument('--timeout',type=int,default=3600);a=p.parse_args()
    out=a.output.resolve();out.mkdir(exist_ok=a.resume);private=out/'.private';private.mkdir(exist_ok=a.resume)
    report_path=out/'BOARD-ASR.json'
    report=json.loads(report_path.read_text(encoding='utf-8')) if a.resume else dict(passed=False,scope='P4 native manifest-bound official 16kHz WAV; no user recording or microphone readiness',asr_ready=False,sample=a.sample,observations=[],heap_samples=[])
    assert report['sample']==a.sample
    def save():report_path.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    save()
    with (private/'console.log').open('a' if a.resume else 'w',encoding='utf-8') as log:
        c=Console(log);c.serial.dtr=True
        def status():
            for _ in range(5):
                raw=c.command('desktop local-voice status');m=re.search(r'(?m)^LOCAL_VOICE (request=[^\n]+)',raw)
                if not m:continue
                s={k:int(v) for k,v in re.findall(r'(\w+)=(-?\d+)',m[1])}
                text=re.search(r'(?m)^LOCAL_VOICE_TEXT bytes=(\d+) fnv=([0-9a-fA-F]+)',raw)
                if not text or not {'request','phase','busy','pending','ready','asr','error','peak','elapsed_ms','tokens'}<=s.keys():continue
                s['reply_bytes']=int(text[1]);s['reply_fnv']=text[2].lower();return s
            raise RuntimeError('Incomplete status; task not resubmitted')
        def heap():
            raw=c.command('free');m=re.search(r'(?m)^\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+\d+\s+\d+\s+Umem',raw)
            assert m,'Incomplete PSRAM status';h=dict(free=int(m[3]),largest=int(m[5]));report['heap_samples'].append(h);save();assert h['free']>=4*1024*1024,'PSRAM reserve breached';return h
        def idle(seconds):
            until=time.monotonic()+seconds
            while time.monotonic()<until:
                s=status()
                if not s['busy'] and not s['pending']:return s
                c.collect(.3)
            raise TimeoutError('Worker still busy; request not replayed')
        def start(sample):
            raw=c.command('desktop local-voice asr-test '+str(sample));m=re.search(r'LOCAL_VOICE accepted=1 request=(\d+) error=0',raw);assert m,raw[-300:];return int(m[1])
        def listen():
            until=time.monotonic()+30;last=None
            while time.monotonic()<until:
                raw=c.command('desktop speech status');m=re.search(r'(?m)^speech (active=[^\n]+)',raw)
                if m:
                    s={k:int(v) for k,v in re.findall(r'(\w+)=(-?\d+)',m[1]) if k!='score'}
                    assert not s.get('error',0)
                    if s.get('active') and s.get('evaluations',0)>0 and last is not None and s.get('frames',0)>last.get('frames',0):return s
                    last=s
                c.collect(1)
            raise TimeoutError('Native EI did not resume after ASR')
        try:
            expected=json.loads((a.candidate/'EXPECTED-XIP-HASHES.json').read_text(encoding='utf-8'));meta=json.loads((a.candidate/'firmware/BUILD-METADATA.json').read_text(encoding='utf-8'))
            xip=c.command('desktop xip',20);assert 'ready=1 protected_cpus=3' in xip and 'text_hash='+expected['text'] in xip and 'rodata_hash='+expected['rodata'] in xip
            report['firmware_sha256']=meta['sha256']['nuttx.bin'];report['xip_bound']=True
            assert 'active=0 ' in c.command('desktop ui recorder-status')
            if not a.resume:
                s=idle(240);assert s['ready'] and s['asr']==0 and not s['error'];report['before']=s;heap()
                if a.wake_test:
                    report['original_wake']=dict(enabled=s['enabled'],local_wake=s['local_wake']);report['wake_override_active']=True;save()
                    if not s['local_wake']:c.command('desktop local-voice wake local');s=idle(30);assert s['local_wake'] and not s['error']
                    if not s['enabled']:c.command('desktop local-voice wake on');s=idle(30);assert s['enabled'] and not s['error']
                    report['wake_before_asr']=listen();save()
                c.command('desktop ui apps');c.collect(.3);c.command('desktop ui pointer-tap:524:566');c.collect(.3);assert '-> 1' in c.command('desktop ui click:本地语音')
                if not a.skip_cancel:
                    request=start(0);until=time.monotonic()+300
                    while time.monotonic()<until:
                        s=status();assert s['request']==request
                        if s['phase']==11:break
                        if not s['busy']:raise RuntimeError('ASR could not start '+str(s['error']))
                        heap();c.collect(2)
                    else:raise TimeoutError('ASR compute not reached; inspect current request before retry')
                    began=time.monotonic();assert '-> 1' in c.command('desktop ui pointer-tap:376:492');s=idle(60)
                    assert s['error']==-125 and s['reply_bytes']==0 and s['asr']==0
                    report['touch_cancel']=dict(**s,observed_stop_seconds=time.monotonic()-began);save()
                    if s['enabled'] and s['local_wake']:report['wake_after_cancel']=listen()
                request=start(a.sample);report['request']=request;report['current']=dict(request=request);save()
            else:
                assert status()['request']==report['request'],'Request changed; never replay old work'
            until=time.monotonic()+a.timeout;last_notice=0
            while time.monotonic()<until:
                s=status();assert s['request']==report['request'];report['current']=s;report['observations'].append(s);heap();save()
                if not s['busy'] and not s['pending']:break
                if time.monotonic()-last_notice>45:print('ASR_NATIVE_PROGRESS phase=%u elapsed_ms=%u tokens=%u'%(s['phase'],s['elapsed_ms'],s['tokens']),flush=True);last_notice=time.monotonic()
                c.collect(8)
            else:raise TimeoutError('Native ASR observation limit reached; inspect/resume same request')
            assert not s['error'] and s['asr']==0 and s['tokens']>0 and s['first_token_ms']>0 and s['peak']<=12*1024*1024,s
            expected_fnv=('04bc9ea5','13ff9039')[a.sample];assert s['reply_bytes']==75 and s['reply_fnv']==expected_fnv,s
            report['completed']=s;report['device_sha_validation_proven_by_asr_execution']=True
            # Diagnostic fields contain no transcript or private recording.
            log.flush();raw=(private/'console.log').read_text(encoding='utf-8',errors='replace')
            matches=list(re.finditer(r'LOCAL_ASR_IO sample=(\d+) bytes=(\d+) reads=(\d+) seeks=(\d+) packed_cache=(\d+) result=(-?\d+)',raw))
            for match in reversed(matches):
                if int(match[1])==a.sample and int(match[6])==0:
                    report['native_weight_io']=dict(bytes=int(match[2]),reads=int(match[3]),seeks=int(match[4]),packed_cache=int(match[5]));break
            if s['enabled'] and s['local_wake']:report['wake_after_completed']=listen()
            backlight=c.command('desktop ui backlight-status');assert 'percent=100 ' in backlight and 'error=0' in backlight;report['backlight_stable_logically']=True
            report['native_heap_reserve_sampled']=True;report['microphone_tested']=False;report['human_chinese_accepted']=False
            if 'error' in report:report.setdefault('prior_observation_errors',[]).append(report.pop('error'))
            report['passed']=True;save();print('ASR_NATIVE_P4_SAMPLE_OK',a.sample,s['elapsed_ms'],s['peak'],flush=True)
        except BaseException as error:
            report['error']=type(error).__name__+': '+str(error)[:300];save();raise
        finally:
            try:
                if report.get('wake_override_active'):
                    current=status()
                    if current['busy'] or current['pending']:
                        report['wake_restore_deferred_until_request_finishes']=True;report['passed']=False
                    else:
                        original=report['original_wake']
                        if current['local_wake']!=original['local_wake']:
                            c.command('desktop local-voice wake '+('local' if original['local_wake'] else 'assistant'));current=idle(30)
                        if current['enabled']!=original['enabled']:
                            c.command('desktop local-voice wake '+('on' if original['enabled'] else 'off'));current=idle(30)
                        assert current['enabled']==original['enabled'] and current['local_wake']==original['local_wake']
                        report['wake_override_active']=False;report['original_wake_restored']=True
                    save()
            except BaseException as error:
                report['wake_restore_error']=type(error).__name__+': '+str(error)[:200];report['passed']=False;save();raise
            finally:c.close()


if __name__=='__main__':main()
