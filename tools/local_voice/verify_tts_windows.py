#!/usr/bin/env python3
"""Exact waveform/dependency/low-memory tests for bounded vocoder windows."""
import argparse
import concurrent.futures
import hashlib
import json
import math
import os
import re
import struct
import subprocess
import time
from pathlib import Path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    for name in ('reference', 'release', 'baseline', 'sentences', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    desktop = repo / 'app/espdl-quickapp/overlay/apps/system/desktop'
    tests = repo / 'app/espdl-quickapp/tests'
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    sources = [desktop / ('glass_local_' + name + '.c')
               for name in ('model', 'sha', 'tokenizer', 'llm', 'tts', 'frontend')]
    deps = sources + [desktop / 'glass_local_model.h', desktop / 'glass_local_tts.h',
                      tests / 'local_voice_host.c', tests / 'local_tts_window_fault_host.c', Path(__file__)]
    hashes = {path.relative_to(repo).as_posix(): sha(path) for path in deps}
    flags = ['gcc', '-std=gnu11', '-O2', '-g', '-ffp-contract=off',
             '-fsanitize=address,undefined', '-fno-pie', '-no-pie']
    after = out / 'after'; before = out / 'before'; fault = out / 'fault'
    subprocess.run([*flags, '-I' + str(desktop), *map(str, sources),
                    str(tests / 'local_voice_host.c'), '-lm', '-o', str(after)], check=True)
    old = args.reference / 'app/espdl-quickapp/overlay/apps/system/desktop'
    subprocess.run([*flags, '-I' + str(old),
                    *[str(old / path.name) for path in sources],
                    str(args.reference / 'app/espdl-quickapp/tests/local_voice_host.c'),
                    '-lm', '-o', str(before)], check=True)
    subprocess.run([*flags, '-I' + str(desktop), *map(str, sources),
                    str(tests / 'local_tts_window_fault_host.c'),
                    '-Wl,--wrap=calloc,--wrap=fread', '-lm', '-o', str(fault)], check=True)
    shapes = {row['name']: row['shape'] for row in
              json.loads((args.release / 'tts-v2/model.json').read_text())['tensors']}
    assert shapes['vocoder.input_conv.weight'] == [208, 80, 7]
    assert shapes['vocoder.output_conv.1.weight'] == [1, 13, 7]
    for stage, stride in enumerate((5, 5, 4, 3)):
        assert shapes[f'vocoder.upsamples.{stage}.1.weight'][2] == 2 * stride
        for branch, kernel in enumerate((3, 7, 11)):
            for layer in range(3):
                for conv in (1, 2):
                    assert shapes[f'vocoder.blocks.{stage * 3 + branch}.convs{conv}.{layer}.1.weight'][2] == kernel
    maximum = 0
    for start in range(0, 513):
        for center in range(1, 65):
            left, right = start * 300 - 3, (start + center) * 300 - 1 + 3
            for stride in (3, 4, 5, 5):
                left -= 60; right += 60
                pad = (stride + 1) // 2
                left = math.ceil((left + pad - (2 * stride - 1)) / stride)
                right = (right + pad) // stride
            left -= 3; right += 3
            maximum = max(maximum, start - left, right - (start + center - 1))
            assert left >= start - 20 and right <= start + center - 1 + 20
    assert maximum == 19
    report = dict(passed=False, source_sha256=hashes, exact_dependency_halo=maximum,
                  cases=[], boundaries=[], faults=[], scope='Host ASAN/UBSAN actual C/Q8; no microphone/playback or native speed claim')
    def save():
        (out / 'VERIFICATION.json').write_text(json.dumps(report, indent=2) + '\n')
    def ten_case(row):
        target = out / (row['id'] + '.f32'); began = time.monotonic()
        run = subprocess.run([str(after), 'synthesize', str(args.release), row['text'], str(target)],
                             capture_output=True, text=True, timeout=300)
        (out / (row['id'] + '.log')).write_text(run.stdout + run.stderr); run.check_returncode()
        assert target.read_bytes() == (args.baseline / target.name).read_bytes(), row['id']
        print('TTS_TEN_RAW_EXACT', row['id'], flush=True)
        return dict(id=row['id'], bit_exact_raw_float=True, host_seconds=time.monotonic() - began,
                    sha256=sha(target), diagnostics=run.stderr.strip())
    rows = json.loads(args.sentences.read_text())['cases']
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        for result in pool.map(ten_case, rows):
            report['cases'].append(result); save()
    for count in (1, 2, 17, 40, 65, 129):
        mel = out / f'mel-{count}.bin'
        mel.write_bytes(struct.pack('<I', count) + struct.pack('<' + 'f' * (80 * count),
                        *[-2 + .2 * math.sin(i * .37) for i in range(80 * count)]))
        target = out / f'before-{count}.f32'
        run = subprocess.run([str(before), 'vocode', str(args.release / 'tts-v2'), str(mel), str(target)],
                             capture_output=True, text=True, timeout=300)
        (out / f'before-{count}.log').write_text(run.stdout + run.stderr); run.check_returncode()
        for budget in (12, 5):
            dest = out / f'after-{count}-{budget}.f32'; env = dict(os.environ, GLV_TEST_MEMORY_LIMIT=str(budget * 1024 * 1024))
            run = subprocess.run([str(after), 'vocode', str(args.release / 'tts-v2'), str(mel), str(dest)],
                                 capture_output=True, text=True, timeout=300, env=env)
            (out / f'after-{count}-{budget}.log').write_text(run.stdout + run.stderr); run.check_returncode()
            assert dest.read_bytes() == target.read_bytes(), (count, budget)
            report['boundaries'].append(dict(frames=count, budget_mib=budget, bit_exact=True, diagnostics=run.stderr.strip()))
            save()
        print('TTS_BOUNDARY_EXACT', count, flush=True)
    for mode in ('allocation', 'io', 'cancel', 'sink'):
        dest = out / (mode + '.f32')
        run = subprocess.run([str(fault), mode, str(args.release / 'tts-v2'), str(out / 'mel-129.bin'), str(dest)],
                             capture_output=True, text=True, timeout=300)
        (out / (mode + '.log')).write_text(run.stdout + run.stderr); run.check_returncode()
        if mode == 'allocation':
            assert dest.read_bytes() == (out / 'before-129.f32').read_bytes()
        else:
            assert not dest.stat().st_size
        report['faults'].append(dict(mode=mode, passed=True)); save()
        print('TTS_WINDOW_FAULT_OK', mode, flush=True)
    assert hashes == {path.relative_to(repo).as_posix(): sha(path) for path in deps}
    report['passed'] = True; save(); print('TTS_WINDOWS_ALL_EXACT_OK', flush=True)


if __name__ == '__main__':
    main()
