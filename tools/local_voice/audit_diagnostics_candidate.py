#!/usr/bin/env python3
"""Bind diagnostic-only changes and host evidence to the linked P4 image."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--candidate', type=Path, required=True)
    a = p.parse_args()
    c = a.candidate.resolve()
    root, source = c.parent, c/'source'
    previous = root/'candidate15'
    meta, old = read(c/'firmware/BUILD-METADATA.json'), read(previous/'firmware/BUILD-METADATA.json')
    d = 'app/espdl-quickapp/overlay/apps/system/desktop/'
    changed = {name for name in set(meta['source_sha256']) | set(old['source_sha256'])
               if meta['source_sha256'].get(name) != old['source_sha256'].get(name)}
    expected = {d+n for n in ('desktop_main.c','glass_ai_config.c','glass_local_llm.c',
                             'glass_local_model.h','glass_local_voice.c','glass_local_voice.h','glass_local_voice_ui.inc')}
    assert changed == expected, changed
    assert sha(c/'firmware/nuttx.bin') == meta['sha256']['nuttx.bin']
    for name, digest in meta['source_sha256'].items():
        assert sha(source/name) == digest, name
    assert (c/'firmware/resolved.config').read_bytes() == (previous/'firmware/resolved.config').read_bytes()
    assert meta['prepared_kernel_sources'] == old['prepared_kernel_sources']
    assert meta['sd_read_diagnostics'] == old['sd_read_diagnostics']
    assert all(meta['ble_identity'][k] != old['ble_identity'][k] for k in ('name','address'))
    llm = (source/d/'glass_local_llm.c').read_text()
    normalized = re.sub(r'static void progress\([^\n]+\n\{[^\n]+\}\n', '', llm)
    normalized = '\n'.join(line for line in normalized.split('\n') if not line.lstrip().startswith('progress(s,'))
    assert normalized == (previous/'source'/d/'glass_local_llm.c').read_text(), 'Numerical LLM body changed'
    evidence = ('diagnostics16-final/VERIFICATION.json','llm-diagnostics16/VERIFICATION.json',
                'diagnostics-ui16/VERIFICATION.json','ai-config16/VERIFICATION.json')
    for relative in evidence:
        proof = read(root/relative)
        assert proof['passed'], relative
        for name, digest in proof['source_sha256'].items():
            assert sha(source/name) == digest, (relative,name)
    diaglog = (root/'diagnostics16-final/diagnostics.log').read_text()
    assert 'LOCAL_DIAGNOSTICS_OK' in diaglog
    assert all(f'CANCEL_DIAG_OK source={i} ' in diaglog for i in range(2,9))
    modelproof = read(root/'llm-diagnostics16/VERIFICATION.json')
    assert {(row['mode'],row['count']) for row in modelproof['cases']} == {('prefill',1),('prefill',33),('prefill',512),('generate',8)}
    for name in ('ELF-AUDIT.json','XIP-REVIEW.json','RESET-ELF-AUDIT.json','HOTLOOP-AUDIT.json'):
        proof = read(c/name)
        assert proof['passed']
        if 'image_sha256' in proof:
            assert proof['image_sha256'] == meta['sha256']['nuttx.bin']
        if 'elf_sha256' in proof:
            assert proof['elf_sha256'] == sha(c/'firmware/nuttx.elf')
    scope = read(c/'scope.json')
    for row in scope['files']:
        assert sha(source/row['path']) == row['sha256']
    report = dict(passed=True,image_sha256=meta['sha256']['nuttx.bin'],
                  scope='Diagnostic-only service/progress change; frozen math, host lifecycle/UI checks, linked image. Native test pending.',
                  numerical_llm_body_unchanged=True,kernel_and_config_identical_to_candidate15=True,
                  compiled_change_paths=sorted(changed),
                  candidate_source_sha256={row['path']:row['sha256'] for row in scope['files']},
                  evidence_sha256={name:sha(root/name) for name in evidence})
    (c/'DIAGNOSTICS-VERIFICATION.json').write_text(json.dumps(report,indent=2)+'\n')
    print('DIAGNOSTICS_P4_LINK_VERIFIED')


if __name__ == '__main__':
    main()
