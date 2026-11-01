#!/usr/bin/env python3
"""Bounded native cold/warm comparison with identical second-turn context.

No microphone or playback. Never replay a request with an uncertain receipt.
Only stop requests accepted by this process; keep failed observations intact.
"""
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
    parser.add_argument('--resume-failed-harness', type=Path)
    parser.add_argument('--resume-cancelled-check', type=Path,
                        help='Explicitly authorized new test after the user stopped the initial SHA check')
    args = parser.parse_args()
    candidate = args.candidate.resolve()
    gate_name = 'PREFILL-VERIFICATION.json' if candidate.name == 'candidate24' else 'SESSION-VERIFICATION.json'
    gate = json.loads((candidate / gate_name).read_text())
    assert gate['passed']
    image_sha = hashlib.sha256((candidate / 'firmware/nuttx.bin').read_bytes()).hexdigest()
    assert gate['image_sha256'] == image_sha
    reference = candidate.parent / gate['text_reference']
    golden = re.search(r'LAZY_CHAT_REPLY tokens=(\d+) bytes=(\d+) fnv=([0-9a-f]+)',
                       (reference / 'chat.log').read_text())
    assert golden
    llm_bytes = json.loads((reference / 'VERIFICATION.json').read_text())['llm_only_sha_bytes']
    out = args.output.resolve()
    out.mkdir(exist_ok=False)
    private = out / '.private'
    private.mkdir()
    (private / '.gitignore').write_text('*\n')
    report = dict(passed=False, firmware_sha256=image_sha, observations=[],
                  scope='Native cold greeting, then same-context warm/cold first token; no audio or microphone',
                  sd_root_cause_cured=False, physical_screen_accepted=False)
    owned = 0

    def save():
        (out / 'BOARD-SESSION.json').write_text(json.dumps(report, indent=2) + '\n')

    save()
    with (private / 'console.log').open('w', encoding='utf-8') as log:
        console = Console(log)
        console.serial.dtr = True

        def status():
            for _ in range(5):
                raw = console.command('desktop local-voice status') + console.collect(.12)
                rows = {}
                for key, prefix in (
                    ('state', 'LOCAL_VOICE'), ('diagnostics', 'LOCAL_VOICE_DIAG'),
                    ('io', 'LOCAL_VOICE_IO'), ('q4', 'LOCAL_VOICE_Q4'),
                    ('lazy', 'LOCAL_VOICE_LAZY'), ('asr_diagnostics', 'LOCAL_VOICE_ASR'),
                    ('cache', 'LOCAL_VOICE_CACHE'), ('session', 'LOCAL_VOICE_SESSION')):
                    match = re.search(r'(?m)^' + prefix + r' (\w+=[^\n]+)', raw)
                    if match:
                        rows[key] = {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)', match[1])}
                text = re.search(r'(?m)^LOCAL_VOICE_TEXT bytes=(\d+) fnv=([0-9a-f]+)', raw)
                if len(rows) != 8 or not text:
                    continue
                state = rows.pop('state')
                if not {'request', 'phase', 'busy', 'pending', 'ready', 'asr',
                        'enabled', 'error', 'peak', 'elapsed_ms', 'tokens',
                        'first_token_ms', 'first_audio_ms'} <= state.keys():
                    continue
                if not {'resident', 'pending', 'bytes', 'reused', 'kv_reused',
                        'prefill_tokens'} <= rows['session'].keys():
                    continue
                required = {
                    'io': {'sha_bytes', 'verified_reuses', 'sha_read_ms', 'sha_compute_ms',
                           'weight_bytes', 'read_ms', 'seek_ms', 'decode_ms', 'matmul_ms'},
                    'diagnostics': {'cancel_source', 'cancel_phase', 'cancel_ms', 'check_ms',
                                    'load_ms', 'tokenize_ms', 'prefill_ms', 'decode_ms', 'progress', 'total'},
                    'cache': {'peak_bytes', 'hits', 'hit_bytes'},
                    'q4': {'bits', 'selftest', 'matrices', 'dots'},
                    'lazy': {'default_text', 'wake_error'},
                    'asr_diagnostics': {'error', 'level', 'capture_ms', 'tokens', 'first_token_ms', 'elapsed_ms', 'text_bytes'},
                }
                if any(not keys <= rows[key].keys() for key, keys in required.items()):
                    continue
                if not all(rows[k].get('request') == state.get('request')
                           for k in ('diagnostics', 'io', 'cache', 'session')):
                    continue
                state.update(rows)
                state.update(reply_bytes=int(text[1]), reply_fnv=text[2])
                state['capture_ms'] = state['asr_diagnostics'].get('capture_ms', -1)
                return state
            raise RuntimeError('Incomplete observation; no request replayed')

        def idle(seconds=30, released=False):
            until = time.monotonic() + seconds
            while time.monotonic() < until:
                state = status()
                if not state['busy'] and not state['pending']:
                    if not released or not any(state['session'][k] for k in ('resident', 'pending', 'bytes')):
                        return state
                console.collect(.3)
            raise TimeoutError('Worker did not retire')

        def start(text):
            nonlocal owned
            raw = console.command('desktop local-voice chat "' + text + '"') + console.collect(.15)
            ack = re.search(r'accepted=1 request=(\d+) error=0', raw)
            assert ack, 'Missing acknowledgement; no replay'
            owned = int(ack[1])
            return owned

        def observe(label, deadline, complete):
            until = time.monotonic() + deadline
            first = None
            while time.monotonic() < until:
                state = status()
                report['observations'].append(dict(case=label, **state))
                assert state['request'] == owned and state['error'] == 0
                assert not any(state[k] for k in ('first_audio_ms', 'capture_ms', 'asr', 'enabled'))
                assert state['phase'] in (0, 1, 2, 3, 14, 15)
                if state['first_token_ms'] and first is None:
                    first = state
                    report[label + '_first'] = first
                    print('NATIVE_SESSION_FIRST', label, first['first_token_ms'],
                          'sha', first['io']['sha_bytes'],
                          'reused', first['session']['kv_reused'], flush=True)
                save()
                if not complete and first is not None:
                    return first
                if not state['busy']:
                    assert complete, 'Request ended without first token'
                    report[label + '_completed'] = state
                    save()
                    return state
                if complete:
                    assert state['tokens'] <= int(golden[1]) + 1
                console.collect(.5)
            raise TimeoutError(label + ' exceeded deadline; no replay')

        def stop_owned():
            nonlocal owned
            current = status()
            assert current['request'] == owned
            if current['busy']:
                console.command('desktop local-voice stop')
            retired = idle(released=True)
            assert retired['request'] == owned
            assert retired['error'] in (0, -125)
            owned = 0
            return retired

        try:
            expected = json.loads((candidate / 'EXPECTED-XIP-HASHES.json').read_text())
            xip = console.command('desktop xip')
            assert all(k + '_hash=' + expected[k] in xip for k in ('text', 'rodata'))
            before = idle(released=True)
            report['startup'] = before
            assert not (args.resume_failed_harness and args.resume_cancelled_check)
            if args.resume_failed_harness or args.resume_cancelled_check:
                failed_path = (args.resume_failed_harness or args.resume_cancelled_check).resolve()
                failed = json.loads(failed_path.read_text())
                assert not failed['passed']
                if args.resume_failed_harness:
                    assert failed['error'] == "KeyError: 'enabled'"
                    expected_cancel = 4
                else:
                    assert failed['error'].startswith('AssertionError:')
                    assert failed['cleanup']['diagnostics']['cancel_source'] == 2
                    assert all(not row['tokens'] and not row['first_token_ms'] and row['phase'] in (0, 1)
                               for row in failed['observations'])
                    expected_cancel = 2
                    report['new_test_after_user_authorized_resume'] = True
                assert failed['firmware_sha256'] == image_sha
                assert 'greeting_completed' not in failed and 'greeting_first' not in failed
                cleanup = failed['cleanup']
                cleared_later_cancel = False
                if before['request'] != cleanup['request']:
                    # The user may have tried and stopped one request while
                    # granting continuation. No completed history is carried
                    # into the benchmark: explicitly start a new conversation.
                    assert args.resume_cancelled_check
                    assert before['request'] == cleanup['request'] + 1
                    assert before['error'] == -125 and before['diagnostics']['cancel_source'] == 2
                    assert not any(before[k] for k in ('busy', 'pending', 'asr', 'enabled', 'first_audio_ms', 'capture_ms'))
                    report['later_user_cancelled_request'] = before
                    console.command('desktop local-voice new')
                    before = idle(released=True)
                    assert before['request'] == cleanup['request'] + 1 and not before['reply_bytes']
                    cleared_later_cancel = True
                assert before['request'] == cleanup['request'] + int(cleared_later_cancel)
                assert not any(before[k] for k in ('busy', 'pending', 'tokens', 'asr', 'enabled', 'first_audio_ms', 'capture_ms'))
                assert before['error'] in ((0, -125) if cleared_later_cancel else (-125,))
                assert before['diagnostics']['cancel_source'] == expected_cancel
                assert not any(failed['startup'][k] for k in ('request', 'ready', 'asr', 'enabled', 'error', 'peak', 'tokens', 'first_audio_ms', 'capture_ms'))
                report['preserved_failed_harness_sha256'] = hashlib.sha256(failed_path.read_bytes()).hexdigest()
                report['original_startup'] = failed['startup']
            else:
                assert not any(before[k] for k in ('request', 'ready', 'asr', 'enabled', 'error', 'peak', 'tokens', 'first_audio_ms', 'capture_ms'))
                assert before['io']['sha_bytes'] == 0
                assert before['q4']['bits'] == 0 and before['cache']['peak_bytes'] == 0
            assert before['lazy']['default_text'] == 1
            settle_until = time.monotonic() + 90
            while True:
                report['heap_before'] = console.command('free')
                heap = re.search(r'(?m)^\s*\d+\s+\d+\s+(\d+)\s+\d+\s+\d+\s+\d+\s+\d+\s+Umem', report['heap_before'])
                if heap and int(heap[1]) >= (4 * 1024 * 1024 + 4608 * 1024):
                    break
                if time.monotonic() >= settle_until:
                    raise TimeoutError('Startup heap did not settle; no inference dispatched')
                console.collect(1)
            assert 'active=0 ' in console.command('desktop ui recorder-status')
            start('你好')
            completed = observe('greeting', 480, True)
            assert completed['tokens'] == int(golden[1])
            assert completed['reply_bytes'] == int(golden[2]) and completed['reply_fnv'] == golden[3]
            assert completed['io']['sha_bytes'] == llm_bytes
            assert completed['session']['resident'] == 1 and completed['session']['bytes'] > 0
            assert completed['session']['reused'] == 0 and completed['session']['kv_reused'] == 0
            owned = 0
            console.collect(2)
            settled = idle()
            assert settled['session']['resident'] == 1
            report['greeting_settled'] = settled
            start('1加1等于几？')
            warm = observe('warm', 240, False)
            assert warm['io']['sha_bytes'] == 0
            assert warm['session']['reused'] == 1 and warm['session']['kv_reused'] > 0
            report['warm_retired'] = stop_owned()
            # Cancel does not append the incomplete second response to history.
            # The cold request therefore has exactly the same dialogue context.
            start('1加1等于几？')
            cold = observe('cold', 300, False)
            assert cold['io']['sha_bytes'] == llm_bytes
            assert cold['session']['reused'] == 0 and cold['session']['kv_reused'] == 0
            assert warm['reply_bytes'] == cold['reply_bytes'] and warm['reply_fnv'] == cold['reply_fnv']
            assert warm['session']['prefill_tokens'] + warm['session']['kv_reused'] == cold['session']['prefill_tokens']
            report['cold_retired'] = stop_owned()
            report['comparison'] = dict(
                warm_first_token_ms=warm['first_token_ms'], cold_first_token_ms=cold['first_token_ms'],
                saved_ms=cold['first_token_ms'] - warm['first_token_ms'],
                saved_percent=100 * (1 - warm['first_token_ms'] / cold['first_token_ms']),
                same_prompt_context=True, first_token_equal=True)
            report['heap_after'] = console.command('free')
            report['backlight_logical'] = console.command('desktop ui backlight-status')
            report['passed'] = True
            save()
            print('NATIVE_SESSION_COMPARISON_OK', json.dumps(report['comparison']), flush=True)
        except BaseException as error:
            report['error'] = type(error).__name__ + ': ' + str(error)[:180]
            try:
                report['last_observation'] = status()
            except Exception as observation_error:
                report['observation_error'] = type(observation_error).__name__
            save()
            raise
        finally:
            if owned:
                try:
                    current = status()
                    if current['request'] == owned:
                        report['cleanup'] = stop_owned()
                except Exception as cleanup_error:
                    report['cleanup_error'] = type(cleanup_error).__name__
            console.close()
            save()


if __name__ == '__main__':
    main()
