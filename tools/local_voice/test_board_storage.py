#!/usr/bin/env python3
"""Reversible missing-mount check; never modify model/user files or format SD.

This does not claim a physical hot-pull test. If normal unmount is busy, abort
without forcing it. Always attempt to restore the original mount and wake mode.
"""
import argparse, json, re, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from music_device_console import Console


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--candidate', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--execute-unmount', action='store_true')
    args = p.parse_args()
    assert args.execute_unmount, 'Explicit test flag required'
    out = args.output.resolve(); out.mkdir(exist_ok=False)
    report = dict(passed=False, scope='Real board, normal unmount/remount; no physical removal or file corruption', steps=[])
    def save():
        (out / 'BOARD-STORAGE.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    save()
    with (out / 'board.log').open('w', encoding='utf-8') as log:
        c = Console(log); c.serial.dtr = True
        original, unmounted = None, False
        def status():
            for _ in range(4):
                text = c.command('desktop local-voice status')
                m = re.search(r'LOCAL_VOICE (request=[^\n]+)', text)
                if m:
                    state = {key: int(value) for key, value in re.findall(r'(\w+)=(-?\d+)', m[1])}
                    if {'busy','pending','ready','error','model_error','local_wake','enabled'} <= state.keys(): return state
            raise RuntimeError('Incomplete status')
        def wait(label, timeout=30):
            end = time.monotonic() + timeout
            while time.monotonic() < end:
                state = status()
                if not state['busy'] and not state['pending']:
                    report['steps'].append(dict(label=label, **state)); save(); return state
                c.collect(.3)
            raise TimeoutError(label)
        try:
            meta = json.loads((args.candidate / 'firmware/BUILD-METADATA.json').read_text(encoding='utf-8'))
            expected = json.loads((args.candidate / 'EXPECTED-XIP-HASHES.json').read_text(encoding='utf-8'))
            xip = c.command('desktop xip', 20)
            assert 'ready=1 protected_cpus=3' in xip and 'text_hash='+expected['text'] in xip and 'rodata_hash='+expected['rodata'] in xip
            report['firmware_sha256'] = meta['sha256']['nuttx.bin']
            original = wait('initial', 180); assert original['ready'] and not original['error']
            assert 'active=0 ' in c.command('desktop ui recorder-status'), 'Recorder active'
            c.command('desktop local-voice wake off'); assert not wait('wake-off')['enabled']
            for _ in range(20):
                if 'active=0 ' in c.command('desktop speech status'): break
                c.collect(.3)
            else: raise RuntimeError('Audio ownership did not retire')
            mounts = c.command('mount')
            assert re.search(r'/sdcard\s+type\s+vfat\b', mounts), 'Unexpected mount topology'
            c.command('desktop ui settings'); c.collect(.4)
            assert '-> 1' in c.command('desktop ui click:本地语音'); c.collect(.4)
            tree = c.command('desktop ui audit'); assert '唤醒与离线资源' in tree and '模型资源' in tree
            report['settings_entry_opened'] = True
            c.command('umount /sdcard'); mounts = c.command('mount')
            assert not re.search(r'/sdcard\s+type\s+vfat\b', mounts), 'Normal unmount refused; not forced'
            unmounted = True; c.collect(2.5)
            c.command('desktop local-voice check'); absent = wait('missing-mount', 30)
            assert absent['error'] in (-2,-5,-19) and not absent['ready']
            failure = absent['model_error']; assert failure
            for target in ('assistant','local'):
                c.command('desktop local-voice wake '+target); state = wait('missing-target-'+target)
                assert state['error'] == 0 and state['model_error'] == failure and not state['ready']
            assert '未找到 SD 卡或模型包' in c.command('desktop ui audit') or failure == -5
            c.command('mount -t vfat /dev/mmcsd0 /sdcard')
            assert re.search(r'/sdcard\s+type\s+vfat\b', c.command('mount')), 'Restore mount failed'
            unmounted = False; c.collect(2.5)
            c.command('desktop local-voice check'); restored = wait('restored-sha', 180)
            assert restored['ready'] and not restored['error'] and not restored['model_error']
            report['checks_passed'] = True; save()
        except BaseException as error:
            report['error'] = type(error).__name__ + ': ' + str(error)[:240]; save(); raise
        finally:
            try:
                if unmounted:
                    c.command('mount -t vfat /dev/mmcsd0 /sdcard')
                    report['restoration_mounts'] = c.command('mount')
                if original:
                    state = status()
                    if not state['busy'] and not state['pending']:
                        c.command('desktop local-voice wake '+('local' if original['local_wake'] else 'assistant')); wait('restore-target')
                        c.command('desktop local-voice wake '+('on' if original['enabled'] else 'off')); report['final'] = wait('restore-enabled')
                        report['restoration_ok'] = report['final']['local_wake']==original['local_wake'] and report['final']['enabled']==original['enabled'] and report['final']['ready']==1 and not report['final']['error']
                report['backlight_after'] = c.command('desktop ui backlight-status'); save()
            finally: c.close()
    report['passed'] = bool(report.get('checks_passed') and report.get('restoration_ok')); save()
    assert report['passed'], 'Checks or restoration incomplete'
    print('LOCAL_VOICE_BOARD_STORAGE_CHECK', report['passed'], flush=True)


if __name__ == '__main__': main()
