#!/usr/bin/env python3
"""Actual C cache faults/lifetime and bit-exact deployed-model comparison."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--reference', type=Path, required=True)
    p.add_argument('--release', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    repo = Path(__file__).resolve().parents[2]
    out = a.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    rel = Path('app/espdl-quickapp/overlay/apps/system/desktop')
    d = repo / rel
    sources = [d / f'glass_local_{n}.c' for n in ('model', 'sha', 'tokenizer', 'llm')]
    harness = repo / 'app/espdl-quickapp/tests/local_llm_prefill_host.c'
    unit = repo / 'app/espdl-quickapp/tests/local_weight_cache_host.c'
    deps = [*sources, d/'glass_local_model.h', d/'glass_local_llm.h', harness, unit, Path(__file__)]
    hashes = {str(f.relative_to(repo)): sha(f) for f in deps}
    flags = ['gcc', '-std=gnu11', '-O2', '-g', '-ffp-contract=off',
             '-fsanitize=address,undefined', '-fno-pie', '-no-pie']
    fixture = out/'fixture'; fixture.mkdir()
    weights = struct.pack('<f', -1) + bytes([0x11]*16) + bytes(12)
    weights += struct.pack('<32f', *range(1, 33))
    weights += b''.join(struct.pack('<f', i)+bytes([0x11]*16) for i in range(1, 5))
    tensors = [
        ('bad.weight', [1, 32, 0, 0], 2, 1, 0, 20, 20),
        ('norm.weight', [32, 0, 0, 0], 1, 0, 32, 128, 0),
        ('sample.weight', [4, 32, 0, 0], 2, 1, 160, 80, 20)]
    index = b'GLM1'+struct.pack('<III', 1, 3, len(weights))+hashlib.sha256(weights).digest()
    for name, shape, ndim, dtype, offset, size, stride in tensors:
        index += name.encode().ljust(80, b'\0')+struct.pack('<9I', *shape, ndim, dtype, offset, size, stride)
    (fixture/'model.bin').write_bytes(index); (fixture/'weights.bin').write_bytes(weights)
    exe = out/'cache-unit'
    subprocess.run([*flags, '-I'+str(d), str(d/'glass_local_model.c'), str(d/'glass_local_sha.c'),
                    str(unit), '-Wl,--wrap=fread', '-lm', '-o', str(exe)], check=True)
    run = subprocess.run([str(exe), str(fixture)], capture_output=True, text=True, timeout=30)
    (out/'cache-unit.log').write_text(run.stdout+run.stderr); run.check_returncode()
    print(run.stdout, flush=True)
    exes = {}
    for name, tree in [('before', a.reference.resolve()), ('after', repo)]:
        base = tree/rel; exe = out/name; exes[name] = exe
        subprocess.run([*flags, '-I'+str(base), *[str(base/f'glass_local_{n}.c')
                        for n in ('model', 'sha', 'tokenizer', 'llm')], str(harness),
                        '-Wl,--wrap=fread,--wrap=fseek', '-lm', '-o', str(exe)], check=True)
    report = dict(passed=False, scope='ASAN/UBSAN host only; no board speed claim',
                  source_sha256=hashes, cases=[], unit_passed=True,
                  model_sha256={n: sha(a.release/'edgeidiot'/n)
                                for n in ('model.bin', 'weights.bin', 'tokenizer.bin')})
    for mode, count, budget in [('prefill', 1, 'normal'), ('prefill', 20, 'normal'),
                               ('prefill', 33, 'normal'), ('prefill', 512, 'normal'),
                               ('prefill', 31, 'tight'), ('prefill', 64, 'tiny'),
                               ('cancel', 64, 'normal'), ('generate', 8, 'normal')]:
        values = {}; outputs = {}
        for version in exes:
            tag = f'{version}-{mode}-{count}-{budget}'; target = out/f'{tag}.bin'
            run = subprocess.run([str(exes[version]), mode, str(a.release/'edgeidiot'),
                                  str(count), str(target), budget], capture_output=True,
                                 text=True, timeout=300)
            (out/f'{tag}.log').write_text(run.stdout+run.stderr); run.check_returncode()
            values[version] = json.loads(run.stdout)
            outputs[version] = target.read_bytes() if target.exists() else None
        assert outputs['before'] == outputs['after'], (mode, count, budget)
        assert values['before']['error'] == values['after']['error']
        assert all(not v['remaining_bytes'] for v in values.values())
        if mode != 'cancel':
            assert values['after']['fread_bytes'] <= values['before']['fread_bytes']
        report['cases'].append(dict(mode=mode, tokens=count, budget=budget, bit_exact=True, stats=values))
        (out/'VERIFICATION.json').write_text(json.dumps(report, indent=2)+'\n')
        print('CACHE_EXACT', mode, count, budget, values['before']['fread_bytes'],
              values['after']['fread_bytes'], flush=True)
    assert hashes == {str(f.relative_to(repo)): sha(f) for f in deps}
    report['passed'] = True
    (out/'VERIFICATION.json').write_text(json.dumps(report, indent=2)+'\n')


if __name__ == '__main__':
    main()
