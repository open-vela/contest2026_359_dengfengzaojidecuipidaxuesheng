#!/usr/bin/env python3
"""Bit-exact scalar prefill/generation AB with real SD-format Q4 assets."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--release', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    rel = Path('app/espdl-quickapp/overlay/apps/system/desktop')
    harness = repo / 'app/espdl-quickapp/tests/local_llm_prefill_host.c'
    deps = [repo / rel / ('glass_local_' + name + '.c')
            for name in ('model', 'sha', 'tokenizer', 'llm')]
    deps += [repo / rel / 'glass_local_model.h', repo / rel / 'glass_local_llm.h', harness, Path(__file__)]
    hashes = {path.relative_to(repo).as_posix(): sha(path) for path in deps}
    flags = ['gcc', '-std=gnu11', '-O2', '-g', '-ffp-contract=off',
             '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-DGLV_TEST_ROPE_COUNT']
    executables = {}
    for version, tree in (('before', args.reference.resolve()), ('after', repo)):
        base = tree / rel
        exe = out / version
        subprocess.run([*flags, '-I' + str(base),
                        *[str(base / ('glass_local_' + name + '.c')) for name in ('model', 'sha', 'tokenizer', 'llm')],
                        str(harness), '-Wl,--wrap=fread,--wrap=fseek,--wrap=powf', '-lm', '-o', str(exe)], check=True)
        executables[version] = exe
    cases = [('prefill', count, 'normal') for count in (1, 20, 27, 33, 55, 64, 65, 511, 512)]
    cases += [('prefill', 31, 'tight'), ('prefill', 64, 'tiny'),
              ('cancel', 64, 'normal'), ('generate', 8, 'normal')]
    report = dict(passed=False, source_sha256=hashes, cases=[],
                  model_sha256={name: sha(args.release / 'edgeidiot' / name)
                                for name in ('model.bin', 'weights.bin', 'tokenizer.bin')},
                  scope='Host ASAN/UBSAN real Q4 scalar AB: logits/reply bytes, read reduction, bounded memory, cancellation and libm call counts. Not a native timing claim.')
    for mode, count, budget in cases:
        stats = {}
        outputs = {}
        for version, exe in executables.items():
            tag = f'{version}-{mode}-{count}-{budget}'
            target = out / (tag + '.bin')
            run = subprocess.run([str(exe), mode, str(args.release / 'edgeidiot'), str(count), str(target), budget],
                                 capture_output=True, text=True, timeout=300)
            (out / (tag + '.log')).write_text(run.stdout + run.stderr)
            run.check_returncode()
            stats[version] = json.loads(run.stdout)
            outputs[version] = target.read_bytes() if target.exists() else None
        assert outputs['before'] == outputs['after'], (mode, count, budget)
        assert stats['before']['error'] == stats['after']['error']
        assert all(not value['remaining_bytes'] and value['peak_bytes'] <= value['limit_bytes']
                   for value in stats.values())
        if mode != 'cancel':
            assert stats['after']['fread_bytes'] <= stats['before']['fread_bytes']
            assert stats['after']['rope_pow_calls'] <= stats['before']['rope_pow_calls']
        if mode == 'prefill' and budget == 'normal':
            assert stats['after']['rope_pow_calls'] == count * 32
            assert stats['before']['rope_pow_calls'] == count * 32 * 10 * 12
            if count > 32:
                assert stats['after']['fread_bytes'] < stats['before']['fread_bytes']
        report['cases'].append(dict(mode=mode, tokens=count, budget=budget, bit_exact=True, stats=stats))
        (out / 'VERIFICATION.json').write_text(json.dumps(report, indent=2) + '\n')
        print('PREFILL_TUNING_EXACT', mode, count, budget,
              stats['before']['fread_bytes'], stats['after']['fread_bytes'], flush=True)
    assert hashes == {path.relative_to(repo).as_posix(): sha(path) for path in deps}
    report['passed'] = True
    (out / 'VERIFICATION.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
