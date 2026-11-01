#!/usr/bin/env python3
"""Actual Q4 LLM differential tests against frozen eight-position prefill."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--reference', type=Path, required=True)
    p.add_argument('--release', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = a.output.resolve()
    out.mkdir(exist_ok=False, parents=True)
    d = 'app/espdl-quickapp/overlay/apps/system/desktop'
    names = ['model', 'sha', 'tokenizer', 'llm']
    harness = root / 'app/espdl-quickapp/tests/local_llm_prefill_host.c'
    sources = [root / d / ('glass_local_' + n + '.c') for n in names]
    sources += [root / d / ('glass_local_' + n + '.h')
                for n in ('model', 'tokenizer', 'llm')]
    hashes = {str(f.relative_to(root)): sha(f) for f in [*sources, harness]}
    executables = {}
    for name, tree in [('eight', a.reference), ('adaptive', root)]:
        exe = out / ('prefill-' + name)
        flags = ['gcc', '-std=gnu11', '-O2', '-g', '-ffp-contract=off',
                 '-fno-omit-frame-pointer', '-fsanitize=address,undefined',
                 '-fno-pie', '-no-pie', '-I' + str(tree / d)]
        command = flags + [str(tree / d / ('glass_local_' + n + '.c'))
                           for n in names]
        command += [str(harness), '-Wl,--wrap=fread,--wrap=fseek', '-lm',
                    '-o', str(exe)]
        subprocess.run(command, check=True)
        executables[name] = exe
    report = dict(passed=False, scope='Host actual SD Q4 C LLM; no native/audio test',
                  sanitizers=['address', 'undefined'], source_sha256=hashes,
                  verifier_sha256=sha(Path(__file__)),
                  model_sha256={n: sha(a.release / 'edgeidiot' / n)
                                for n in ('model.bin', 'weights.bin', 'tokenizer.bin')},
                  reference_source_sha256={str(f.relative_to(root)): sha(
                      a.reference / f.relative_to(root)) for f in sources},
                  cases=[])

    def save():
        (out / 'VERIFICATION.json').write_text(json.dumps(report, indent=2) + '\n')

    def run(version, mode, count, budget='normal'):
        tag = f'{version}-{mode}-{count}-{budget}'
        path = out / (tag + '.bin')
        result = subprocess.run([str(executables[version]), mode,
                                 str(a.release / 'edgeidiot'), str(count),
                                 str(path), budget], capture_output=True, text=True)
        (out / (tag + '.log')).write_text(result.stdout + result.stderr)
        result.check_returncode()
        value = json.loads(result.stdout)
        assert value['remaining_bytes'] == 0
        return path, value

    save()
    for count in (1, 7, 8, 9, 16, 17, 31, 32, 33, 64, 512):
        before, old = run('eight', 'prefill', count)
        after, new = run('adaptive', 'prefill', count)
        assert before.read_bytes() == after.read_bytes(), count
        assert new['fread_bytes'] <= old['fread_bytes']
        if count > 8:
            assert new['fread_bytes'] < old['fread_bytes']
        row = dict(tokens=count, bit_exact=True, eight=old, adaptive=new)
        report['cases'].append(row)
        save()
        print('PREFILL_EXACT', count, old['fread_bytes'], new['fread_bytes'], flush=True)
    # Independent sequential forward, including the maximum causal context.
    for count in (33, 512):
        sequential, result = run('adaptive', 'sequential', count)
        assert sequential.read_bytes() == (
            out / f'adaptive-prefill-{count}-normal.bin').read_bytes()
        report.setdefault('sequential', []).append(dict(tokens=count, **result))
        save()
        print('SEQUENTIAL_EXACT', count, flush=True)
    for budget, counts in [('tight', (31, 64)), ('tiny', (64,))]:
        for count in counts:
            before, old = run('eight', 'prefill', count, budget)
            after, new = run('adaptive', 'prefill', count, budget)
            assert old['error'] == new['error']
            if budget == 'tight':
                assert before.read_bytes() == after.read_bytes()
                assert old['fread_bytes'] == new['fread_bytes']
            report.setdefault('budgets', []).append(dict(budget=budget, tokens=count,
                                                       eight=old, adaptive=new))
            save()
    _, cancelled = run('adaptive', 'cancel', 64)
    report['cancellation'] = cancelled
    before, old = run('eight', 'generate', 8)
    after, new = run('adaptive', 'generate', 8)
    assert before.read_bytes() == after.read_bytes()
    report['greedy_reply_bit_exact'] = dict(eight=old, adaptive=new,
                                           reply_sha256=sha(after))
    assert hashes == {str(f.relative_to(root)): sha(f) for f in [*sources, harness]}
    report['passed'] = True
    save()
    print('ADAPTIVE_PREFILL_ACTUAL_MODEL_VERIFIED', flush=True)


if __name__ == '__main__':
    main()
