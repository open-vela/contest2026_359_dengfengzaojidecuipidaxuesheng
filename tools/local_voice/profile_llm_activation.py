#!/usr/bin/env python3
"""Pilot actual-Q4 activation/masking study; never modifies firmware or weights.

Masks use already computed activations, so this oracle cannot save gate/up
reads. Group skipping assumes a future sparse reader and suitable SD layout.
Host runtime is intentionally not reported as an ESP32 speed estimate.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--release', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--tokens', type=int, default=16)
    a = parser.parse_args()
    assert 1 <= a.tokens <= 96
    repo = Path(__file__).resolve().parents[2]
    d = repo/'app/espdl-quickapp/overlay/apps/system/desktop'
    out = a.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    harness = repo/'app/espdl-quickapp/tests/local_activation_probe_host.c'
    source = (d/'glass_local_llm.c').read_text()
    scalar = 'for(unsigned i=0;i<FFN;i++)gate[i]=gate[i]/(1+expf(-gate[i]))*up[i];'
    batched = 'for(unsigned i=0;i<batch*FFN;i++)gate[i]=gate[i]/(1+expf(-gate[i]))*up[i];'
    assert source.count(scalar) == source.count(batched) == 1
    source = source.replace(scalar, scalar+'\n    glv_activation_probe(layer,gate,FFN,1,0);')
    source = source.replace(batched, batched+'\n      glv_activation_probe(layer,gate,FFN,batch,1);')
    prototype = 'void glv_activation_probe(unsigned,float *,unsigned,unsigned,int);\n'
    instrumented = out/'instrumented_llm.c'; instrumented.write_text(prototype+source)
    sources = [d/f'glass_local_{n}.c' for n in ('model', 'sha', 'tokenizer')]
    deps = [*sources, d/'glass_local_llm.c', d/'glass_local_model.h', d/'glass_local_llm.h', harness, Path(__file__)]
    hashes = {str(f.relative_to(repo)): sha(f) for f in deps}
    exe = out/'activation-probe'
    subprocess.run(['gcc', '-std=gnu11', '-O2', '-g', '-ffp-contract=off',
                    '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-I'+str(d),
                    *map(str, sources), str(instrumented), str(harness), '-lm', '-o', str(exe)], check=True)
    report = dict(passed=False, scope='Four-prompt pilot; actual Q4 C engine with host-only oracle masks',
                  source_sha256=hashes, model_sha256={n: sha(a.release/'edgeidiot'/n)
                    for n in ('model.bin', 'weights.bin', 'tokenizer.bin')},
                  tokens_limit=a.tokens, cases=[], limitations=[
                    'Exact-reply agreement is not a semantic-quality evaluation.',
                    'SiLU*up is computed before selecting the oracle mask; gate/up reads cannot be skipped by this experiment.',
                    'No actual sparse I/O kernel, board acceleration, predictor or production mask is enabled.',
                    'Group skip counts account for prefill batch unions; random small reads may erase storage benefits.',
                    'The small public prompt set cannot establish general Chinese quality or safe sparsity.'])
    variants = [('dense', 0, 0), ('neuron10', 1, 10), ('neuron25', 1, 25),
                ('neuron50', 1, 50), ('block25', 2, 25), ('block50', 2, 50)]
    for i, prompt in enumerate(['你好', '1加1等于几？', '台灯怎么打开？', '为什么要节约用水？']):
        template = '<|im_start|>user\n'+prompt+'<|im_end|>\n<|im_start|>assistant\n'
        dense = None
        for name, mode, percent in variants:
            run = subprocess.run([str(exe), str(a.release/'edgeidiot'), str(mode), str(percent),
                                  template, str(a.tokens)], capture_output=True, text=True, timeout=300)
            (out/f'{i}-{name}.log').write_text(run.stdout+run.stderr); run.check_returncode()
            result = json.loads(run.stdout)
            if dense is None:
                dense = result['reply']
            row = dict(prompt=prompt, variant=name, oracle_drop_percent=percent,
                       exact_reply_agreement=result['reply'] == dense, **result)
            report['cases'].append(row)
            (out/'ACTIVATION-STUDY.json').write_text(json.dumps(report, indent=2, ensure_ascii=False)+'\n')
            print('ACTIVATION_PILOT', i, name, row['exact_reply_agreement'], flush=True)
    assert hashes == {str(f.relative_to(repo)): sha(f) for f in deps}
    report['passed'] = True
    (out/'ACTIVATION-STUDY.json').write_text(json.dumps(report, indent=2, ensure_ascii=False)+'\n')


if __name__ == '__main__':
    main()
