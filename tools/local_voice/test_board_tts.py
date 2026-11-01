#!/usr/bin/env python3
"""One explicitly dispatched fixed-text TTS request, never microphone capture."""
import argparse
import hashlib
import json
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from music_device_console import Console


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--deadline-seconds', type=int, default=300)
    parser.add_argument('--no-play', action='store_true', help='Synthesize with the same buffers, never open audio')
    parser.add_argument('--kernel', choices=('single','batch'))
    parser.add_argument('--parallel', choices=('on','off'))
    parser.add_argument('--fma', choices=('on','off'))
    parser.add_argument('--rows', choices=('bulk','legacy'))
    parser.add_argument('--window', choices=('planned','legacy'))
    parser.add_argument('--input-parallel',choices=('on','off'))
    parser.add_argument('--bench-limit',type=int,choices=(0,4096),default=0)
    args = parser.parse_args()
    assert 10 <= args.deadline_seconds <= 900
    candidate = args.candidate.resolve()
    meta = json.loads((candidate / 'firmware/BUILD-METADATA.json').read_text())
    image_sha = hashlib.sha256((candidate / 'firmware/nuttx.bin').read_bytes()).hexdigest()
    assert image_sha == meta['sha256']['nuttx.bin']
    pie = bool(meta.get('local_tts_pie', {}).get('included'))
    batches = bool(meta.get('local_tts_pie',{}).get('batch_default'))
    sha_hw = bool(meta.get('local_sha_hw',{}).get('enabled'))
    parallel = bool(meta.get('local_tts_parallel',{}).get('enabled'))
    if parallel: assert args.parallel is not None
    if args.parallel: assert parallel
    fma=bool(meta.get('local_tts_fma',{}).get('enabled'))
    if fma: assert args.fma is not None
    if args.fma: assert fma
    if fma:
        fma_gate=json.loads((candidate/'FMA-TTS-VERIFICATION.json').read_text())
        assert fma_gate['passed'] and fma_gate['image_sha256']==image_sha
    bulk=bool(meta.get('local_tts_io',{}).get('q8_bulk_rows'))
    windows=bool(meta.get('local_tts_windows',{}).get('enabled'))
    input_parallel=bool(meta.get('local_tts_input',{}).get('enabled'))
    if input_parallel:assert args.input_parallel is not None
    if windows:assert args.window is not None
    if args.bench_limit:assert windows and args.no_play
    if bulk:assert args.rows is not None
    if args.kernel: assert batches
    diagnostic = candidate.name == 'candidate25' or pie
    if candidate.name == 'candidate25':
        gate = json.loads((candidate / 'TTS-VERIFICATION.json').read_text())
        assert gate['passed'] and gate['image_sha256'] == image_sha
    if pie:
        gate=json.loads((candidate/'TTS-PIE-VERIFICATION.json').read_text())
        assert gate['passed'] and gate['image_sha256']==image_sha
    if args.no_play: assert diagnostic
    out = args.output.resolve(); out.mkdir(exist_ok=False)
    private = out / '.private'; private.mkdir(); (private / '.gitignore').write_text('*\n')
    report = dict(passed=False, firmware_sha256=image_sha, text='你好。', observations=[],
                  scope='Native fixed-text V2 synthesis and PCM submission; no microphone or wake test',
                  speaker_listening_accepted=False, physical_screen_accepted=False, sd_root_cause_cured=False)
    owned = 0
    report['no_play']=args.no_play
    report['kernel']=args.kernel or ('batch' if batches else 'single')
    if args.no_play:report['scope']='Native fixed-text V2 synthesis and buffered waveform only; no audio device open, PCM submission, microphone or wake test'
    def save():
        (out / 'BOARD-TTS.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    save()
    with (private / 'console.log').open('w', encoding='utf-8') as log:
        console = Console(log); console.serial.dtr = True
        def status():
            for _ in range(6):
                raw = console.command('desktop local-voice status') + console.collect(.1)
                rows = {}
                required = {
                    'state': ('LOCAL_VOICE', {'request','phase','busy','pending','ready','asr','enabled','error','peak','elapsed_ms','first_token_ms','first_audio_ms','tokens'}),
                    'session': ('LOCAL_VOICE_SESSION', {'request','resident','pending','bytes'}),
                    'io': ('LOCAL_VOICE_IO', {'request','sha_bytes','read_ms','decode_ms','matmul_ms'}),
                    'asr_diagnostics': ('LOCAL_VOICE_ASR', {'capture_ms','tokens','error'}),
                    'diag': ('LOCAL_VOICE_DIAG', {'request','cancel_source','cancel_phase','cancel_ms'}),
                }
                if diagnostic:
                    required['tts'] = ('LOCAL_VOICE_TTS', {'request','mel_ms','vocoder_ms','chunks','context_frames','output_frames','memory_fallbacks'})
                if batches:
                    required['work']=('LOCAL_VOICE_TTS_WORK',{'request','single','batches','input_ms','weights_ms','pack_ms','compute_ms','output_ms','upsample_ms','wave_fnv'})
                if sha_hw: required['io'][1].update({'sha_hw','sha_hw_files','sha_hw_updates','sha_hw_selftests'})
                if input_parallel:required['input_parallel']=('LOCAL_VOICE_TTS_INPUT',{'request','enabled','max_jobs','quant_jobs','rounding_fallbacks'})
                if windows:required['windows']=('LOCAL_VOICE_TTS_WINDOWS',{'request','planned','cap_kib','checks','skips','attempts','failed_frames','failed_ms'})
                if bulk: required['rows']=('LOCAL_VOICE_TTS_ROWS',{'request','bulk','reads','bulk_rows','legacy_rows'})
                if fma: required['fma']=('LOCAL_VOICE_TTS_FMA',{'request','enabled','upsample_calls','rejected'})
                if parallel: required['parallel']=('LOCAL_VOICE_TTS_PAR',{'request','enabled','jobs','cpu0','cpu1','fallbacks','stack_charge','elapsed_ms','overlap_ms'})
                if meta.get('local_tts_parallel',{}).get('mlp_time_slices'):required['parallel'][1].add('mlp_jobs')
                if meta.get('local_tts_parallel',{}).get('dynamic_owner_helper_opposite'):required['parallel'][1].update({'owner0','owner1','pin_retries'})
                if meta.get('local_tts_parallel',{}).get('fallback_diagnostics'):required['parallel'][1].update({'cache_fb','budget_fb','reserve_fb','fragment_fb','mutex_fb','setup_fb','create_fb','last_reason','last_error','setup_us','join_us'})
                for key, (prefix, keys) in required.items():
                    match = re.search(r'(?m)^' + prefix + r' (\w+=[^\n]+)', raw)
                    if match:
                        row = {k:int(v) for k,v in re.findall(r'(\w+)=(-?\d+)', match[1])}
                        if keys <= row.keys(): rows[key] = row
                if len(rows) != len(required): continue
                text = re.search(r'(?m)^LOCAL_VOICE_TEXT bytes=(\d+) fnv=([0-9a-f]{8})\b', raw)
                if not text: continue
                state = rows.pop('state')
                if any(row['request'] != state['request'] for row in rows.values() if 'request' in row): continue
                state.update(rows); state.update(reply_bytes=int(text[1]),reply_fnv=text[2]); return state
            raise RuntimeError('Incomplete read-only observation; request not replayed')
        def idle(seconds=30):
            until = time.monotonic() + seconds
            while time.monotonic() < until:
                state = status()
                if not state['busy'] and not state['pending'] and not state['session']['pending']: return state
                console.collect(.5)
            raise TimeoutError('Existing work still active; no request dispatched')
        def heap(label):
            raw = console.command('free')
            match = re.search(r'(?m)^\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+\d+\s+\d+\s+Umem', raw)
            assert match, 'Incomplete heap observation'
            row = dict(label=label,total=int(match[1]),used=int(match[2]),free=int(match[3]),maxused=int(match[4]),largest=int(match[5]))
            report.setdefault('heap', []).append(row); save()
            assert row['free'] >= 4*1024*1024
            return row
        try:
            expected = json.loads((candidate / 'EXPECTED-XIP-HASHES.json').read_text())
            raw = console.command('desktop xip')
            assert all(key + '_hash=' + expected[key] in raw for key in ('text','rodata'))
            before = idle(); report['before'] = before
            # Readiness/capture diagnostics may describe a previous user voice
            # request. The new text-read request resets its own capture fields.
            # Preserve the user's wake preference; submit() yields its existing
            # background listener and busy prevents it from restarting.
            report['wake_enabled_preserved'] = before['enabled']
            assert not before['session']['resident'] and not before['session']['bytes']
            assert 'active=0 ' in console.command('desktop ui recorder-status')
            heap('before')
            if args.kernel:
                raw=console.command('desktop local-voice tts-kernel '+args.kernel)
                assert 'accepted=1 request=0 error=0' in raw,'Kernel choice not acknowledged; no request dispatched'
            if parallel:
                raw=console.command('desktop local-voice tts-parallel '+args.parallel)
                assert 'accepted=1 request=0 error=0' in raw,'Parallel choice not acknowledged; no request dispatched'
                report['parallel_choice']=args.parallel
            if fma:
                ack=console.command('desktop local-voice tts-fma '+args.fma)
                assert 'accepted=1 request=0 error=0' in ack,'FMA choice not acknowledged; no request dispatched'
                report['fma_choice']=args.fma
            if bulk:
                ack=console.command('desktop local-voice tts-rows '+args.rows)
                assert 'accepted=1 request=0 error=0' in ack
                report['rows_choice']=args.rows
            if windows:
                for command in ('tts-window '+args.window,'tts-bench-limit '+str(args.bench_limit)):
                    ack=console.command('desktop local-voice '+command)
                    assert 'accepted=1 request=0 error=0' in ack
                report.update(window_choice=args.window,bench_limit_kib=args.bench_limit)
            if input_parallel:
                ack=console.command('desktop local-voice tts-input '+args.input_parallel)
                assert 'accepted=1 request=0 error=0' in ack
                report['input_parallel_choice']=args.input_parallel
            command='desktop local-voice '+('tts-bench' if args.no_play else 'read')+' "你好。"'
            raw = console.command(command) + console.collect(.15)
            ack = re.search(r'accepted=1 request=(\d+) error=0', raw)
            assert ack, 'No receipt; do not replay'; owned = int(ack[1])
            report['request'] = owned; report['deadline_seconds'] = args.deadline_seconds
            expected_text = '你好。'.encode('utf-8'); expected_hash = 2166136261
            for byte in expected_text: expected_hash = ((expected_hash ^ byte) * 16777619) & 0xffffffff
            save(); until = time.monotonic()+args.deadline_seconds
            last_phase = None; last_heap = time.monotonic()
            while time.monotonic() < until:
                state = status(); report['observations'].append(state); save()
                if (out / 'STOP').exists():
                    if not state['first_audio_ms']: report['synthesis_lower_bound_ms' if args.no_play else 'first_audio_lower_bound_ms'] = state['elapsed_ms']
                    raise InterruptedError('Operator ended owned measurement; no replay')
                assert state['request'] == owned and state['error'] == 0
                assert state['reply_bytes'] == len(expected_text) and state['reply_fnv'] == f'{expected_hash:08x}'
                assert not state['tokens'] and not state['first_token_ms'] and not state['asr_diagnostics']['capture_ms']
                assert state['enabled'] == before['enabled']
                assert state['phase'] in (0,1,4,5,6,7)
                if state['phase'] != last_phase:
                    print('NATIVE_TTS_PHASE',state['phase'],'elapsed_ms',state['elapsed_ms'],flush=True)
                    last_phase = state['phase']
                if state['first_audio_ms'] and 'first_audio' not in report:
                    report['first_audio'] = state; save()
                if not state['busy']:
                    assert state['peak'] <= 12*1024*1024
                    if args.no_play:
                        assert state['first_audio_ms']==0 and state['tts']['ready_ms']>0 and state['tts']['samples']==21000
                    else: assert state['first_audio_ms']>0
                    if pie: assert state['tts']['pie_calls']>=74 and state['tts']['pie_dots']>0 and state['tts']['pie_selftests']==1
                    if batches:
                        assert bool(state['work']['single'])==(report['kernel']=='single')
                        assert (state['work']['batches']>0)==(report['kernel']=='batch')
                        assert state['work']['wave_fnv']>0
                    if sha_hw:
                        assert state['io']['sha_hw']==1 and state['io']['sha_hw_selftests']==1
                        assert state['io']['sha_hw_files']>0 and state['io']['sha_hw_updates']>0
                    if parallel:
                        par=state['parallel'];assert par['enabled']==(args.parallel=='on')
                        if args.parallel=='on':
                            assert par['jobs']>0 and par['cpu0']==par['cpu1']==par['jobs']
                            assert par['overlap_ms']>0 and par['stack_charge']==131072
                            if meta.get('local_tts_parallel',{}).get('mlp_time_slices'):assert par['mlp_jobs']>0
                            if meta.get('local_tts_parallel',{}).get('dynamic_owner_helper_opposite'):assert par['owner0']+par['owner1']==par['jobs']
                        else: assert not par['jobs'] and not par['cpu0'] and not par['cpu1'] and not par['stack_charge']
                        if meta.get('local_tts_parallel',{}).get('fallback_diagnostics'):
                            assert par['fallbacks']==sum(par[k] for k in ('cache_fb','budget_fb','reserve_fb','fragment_fb','mutex_fb','setup_fb','create_fb'))
                    if fma:
                        q=state['fma'];assert q['enabled']==(args.fma=='on') and not q['rejected']
                        assert q['upsample_calls']==(4*state['tts']['chunks'] if args.fma=='on' else 0)
                        if args.fma=='off' and not args.bench_limit:assert state['work']['wave_fnv']==fma_gate['expected_wave_fnv']['off']
                        else:report['native_fma_waveform_quality_pending']=True
                    if input_parallel:
                        q=state['input_parallel'];assert q['enabled']==(args.input_parallel=='on') and not q['rounding_fallbacks']
                        if args.input_parallel=='on' and args.parallel=='on':assert q['max_jobs']>0 and q['quant_jobs']>0
                        else:assert not q['max_jobs'] and not q['quant_jobs']
                    if windows:
                        q=state['windows'];assert q['planned']==(args.window=='planned') and q['cap_kib']==args.bench_limit
                        assert q['attempts']>=state['tts']['chunks']
                        if args.bench_limit:report['native_window_waveform_reference_pending']=True
                    if bulk:
                        q=state['rows'];assert q['bulk']==(args.rows=='bulk')
                        assert q['reads']>0
                        if args.rows=='bulk':assert q['bulk_rows']>0 and not q['legacy_rows']
                        else:assert q['legacy_rows']>0 and not q['bulk_rows']
                    assert not state['session']['resident'] and not state['session']['bytes']
                    if diagnostic:
                        assert state['tts']['chunks'] > 0 and state['tts']['output_frames'] > 0
                        assert state['tts']['context_frames'] >= state['tts']['output_frames']
                    report['completed'] = state; owned=0; heap('after')
                    report['backlight_logical'] = console.command('desktop ui backlight-status')
                    report['passed'] = True; save()
                    print('NATIVE_TTS_COMPLETE',state['first_audio_ms'],state['elapsed_ms'],state.get('tts'),flush=True)
                    return
                if time.monotonic()-last_heap >= 15:
                    heap('active'); last_heap=time.monotonic()
                console.collect(.5)
            if not state['first_audio_ms']: report['synthesis_lower_bound_ms' if args.no_play else 'first_audio_lower_bound_ms'] = state['elapsed_ms']
            raise TimeoutError('TTS deadline; owned request will be canceled, not replayed')
        except BaseException as error:
            report['error'] = type(error).__name__+': '+str(error)[:180]
            try: report['last_observation'] = status()
            except Exception as exc: report['observation_error'] = type(exc).__name__
            save(); raise
        finally:
            if owned:
                try:
                    state = status()
                    if state['request'] == owned and state['busy']:
                        console.command('desktop local-voice stop'); report['cleanup'] = idle()
                except Exception as exc: report['cleanup_error'] = type(exc).__name__
            console.close(); save()


if __name__ == '__main__': main()
