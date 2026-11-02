#!/usr/bin/env python3
"""Bounded, read-only model checks on an audited SD diagnostic candidate.

Never capture audio, run inference, replay a failed check, or reset the board.
Raw console output stays private. A clean short run is not a reliability cure.
"""
import argparse
import json
from pathlib import Path
import re
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from music_device_console import Console


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--checks', type=int, choices=(1, 2, 3), default=1)
    args = parser.parse_args()
    candidate = args.candidate.resolve()
    meta = json.loads((candidate / 'firmware/BUILD-METADATA.json').read_text())
    gate = json.loads((candidate / 'SD-DIAGNOSTIC-VERIFICATION.json').read_text())
    assert gate['passed'] and gate['image_sha256'] == meta['sha256']['nuttx.bin']
    out = args.output.resolve()
    out.mkdir(exist_ok=False)
    private = out / '.private'
    private.mkdir()
    (private / '.gitignore').write_text('*\n')
    report = dict(passed=False, firmware_sha256=meta['sha256']['nuttx.bin'],
                  scope='Native SD model SHA checks only; no inference/audio',
                  requested_checks=args.checks, checks=[], root_cause_cured=False,
                  microphone_captures=0, physical_screen_accepted=False)
    observed = []
    owned = 0

    def save():
        (out / 'BOARD-SD-READ.json').write_text(
            json.dumps(report, indent=2) + '\n', encoding='utf-8')

    save()
    with (private / 'console.log').open('w', encoding='utf-8') as log:
        console = Console(log)
        console.serial.dtr = True

        def status():
            # Retry observation only; never replay check/inference commands.
            for _ in range(5):
                raw = console.command('desktop local-voice status')
                raw += console.collect(.2)
                observed.append(raw)
                match = re.search(r'(?m)^LOCAL_VOICE (request=[^\n]+)', raw)
                if not match:
                    continue
                result = {key: int(value) for key, value in re.findall(
                    r'(\w+)=(-?\d+)', match[1])}
                if {'request', 'busy', 'pending', 'phase', 'ready', 'asr',
                    'enabled', 'error', 'elapsed_ms', 'first_audio_ms',
                    'tokens'} <= result.keys():
                    return result
            raise RuntimeError('Incomplete read-only status; no check replayed')

        def idle():
            deadline = time.monotonic() + 240
            while time.monotonic() < deadline:
                current = status()
                if not current['busy'] and not current['pending']:
                    return current
                observed.append(console.collect(.5))
            raise TimeoutError('Model check did not retire')

        try:
            expected = json.loads((candidate / 'EXPECTED-XIP-HASHES.json').read_text())
            raw = console.command('desktop xip')
            assert all(key + '_hash=' + expected[key] in raw
                       for key in ('text', 'rodata'))
            report['before'] = before = idle()
            save()
            assert before['ready'] and before['asr'] and not before['error']
            assert not before['enabled'], 'Global wake must stay OFF'
            assert not before['first_audio_ms'] and not before['tokens']
            for _ in range(args.checks):
                raw = console.command('desktop local-voice check')
                observed.append(raw)
                match = re.search(r'accepted=1 request=(\d+) error=0', raw)
                assert match, 'Missing acknowledgement; no check replayed'
                owned = int(match[1])
                current = idle()
                report['checks'].append(current)
                save()
                assert current['request'] == owned
                owned = 0
                assert current['ready'] and current['asr'] and not current['error']
                assert not current['enabled'] and not current['tokens']
                assert not current['first_audio_ms']
            report['backlight_logical'] = console.command('desktop ui backlight-status')
            report['passed'] = True
            print('P4_SD_MODEL_READ_CHECKS_OK', flush=True)
        except BaseException as error:
            report['error'] = type(error).__name__ + ': ' + str(error)[:160]
            try:
                report['last_observation'] = status()
            except Exception as observation_error:
                report['observation_error'] = type(observation_error).__name__
            raise
        finally:
            if owned:
                try:
                    current = status()
                    if current['request'] == owned and current['busy']:
                        console.command('desktop local-voice stop')
                        report['cleanup'] = idle()
                except Exception as cleanup_error:
                    report['cleanup_error'] = type(cleanup_error).__name__
            console.close()
            text = '\n'.join(observed)
            report['sd_failures_observed'] = re.findall(
                r'Did not receive data token \([0-9a-f]+\)'
                r' elapsed_ticks=\d+ limit_ticks=\d+ polls=\d+', text)
            save()


if __name__ == '__main__':
    main()
