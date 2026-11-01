#!/usr/bin/env python3
"""Check actual Q4 outputs/reads with progress enabled against frozen candidate15."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('reference', 'release', 'output'):
        p.add_argument('--' + name, type=Path, required=True)
    a = p.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    d = Path('app/espdl-quickapp/overlay/apps/system/desktop')
    harness = root / 'app/espdl-quickapp/tests/local_llm_prefill_host.c'
    deps = [root/d/('glass_local_' + n + ext) for n in ('model', 'sha', 'tokenizer', 'llm')
            for ext in ('.c', '.h') if (root/d/('glass_local_' + n + ext)).exists()] + [harness, Path(__file__)]
    hashes = {str(f.relative_to(root)): sha(f) for f in deps}
    for name, tree in [('reference', a.reference), ('observed', root)]:
        command = ['gcc', '-std=gnu11', '-O2', '-g', '-ffp-contract=off',
                   '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-I'+str(tree/d)]
        if name == 'observed':
            command += ['-DGLV_TEST_PROGRESS']
        command += [str(tree/d/('glass_local_'+n+'.c')) for n in ('model','sha','tokenizer','llm')]
        command += [str(harness), '-Wl,--wrap=fread,--wrap=fseek', '-lm', '-o', str(out/name)]
        subprocess.run(command, check=True)
    cases = []
    for mode, count in [('prefill', 1), ('prefill', 33), ('prefill', 512), ('generate', 8)]:
        values = {}
        for name in ('reference', 'observed'):
            tag = f'{name}-{mode}-{count}'
            result = subprocess.run([str(out/name), mode, str(a.release/'edgeidiot'), str(count),
                                     str(out/(tag+'.bin')), 'normal'], capture_output=True, text=True, timeout=300)
            (out/(tag+'.log')).write_text(result.stdout+result.stderr)
            result.check_returncode()
            values[name] = json.loads(result.stdout)
        assert (out/f'reference-{mode}-{count}.bin').read_bytes() == (out/f'observed-{mode}-{count}.bin').read_bytes()
        for field in ('error', 'fread_calls', 'fread_bytes', 'fseek_calls', 'remaining_bytes'):
            assert values['reference'][field] == values['observed'][field]
        cases.append(dict(mode=mode, count=count, bit_exact=True, **values))
        print('PROGRESS_EXACT', mode, count, flush=True)
    assert hashes == {str(f.relative_to(root)): sha(f) for f in deps}
    (out/'VERIFICATION.json').write_text(json.dumps(dict(passed=True,
        scope='Actual C Q4 engines, progress callbacks enabled; same logits/reply and stdio workload, no hardware claim',
        source_sha256=hashes, cases=cases), indent=2)+'\n')


if __name__ == '__main__':
    main()
