#!/usr/bin/env python3
"""Bind adaptive prefill math/memory evidence to the final P4 image."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--toolchain', type=Path, required=True)
    a = parser.parse_args()
    c = a.candidate.resolve()
    root = c.parent
    s = c / 'source'
    previous = root / 'candidate14'
    meta = read(c / 'firmware/BUILD-METADATA.json')
    old = read(previous / 'firmware/BUILD-METADATA.json')
    d = 'app/espdl-quickapp/overlay/apps/system/desktop/'
    assert sha(c / 'firmware/nuttx.bin') == meta['sha256']['nuttx.bin']
    changed = {p for p in set(old['source_sha256']) | set(meta['source_sha256'])
               if old['source_sha256'].get(p) != meta['source_sha256'].get(p)}
    assert changed == {d + 'glass_local_' + n for n in
                       ('llm.c', 'model.c', 'model.h')}, changed
    for relative, digest in meta['source_sha256'].items():
        assert sha(s / relative) == digest, relative
    assert (c / 'firmware/resolved.config').read_bytes() == (
        previous / 'firmware/resolved.config').read_bytes()
    assert meta['prepared_kernel_sources'] == old['prepared_kernel_sources']
    assert meta['sd_read_diagnostics'] == old['sd_read_diagnostics']
    for key in ('name', 'address'):
        assert meta['ble_identity'][key] != old['ble_identity'][key]
    before_model = (previous / 'source' / d / 'glass_local_model.c').read_text()
    after_model = (s / d / 'glass_local_model.c').read_text()
    assert before_model.replace('!batch||batch>8||',
                                '!batch||batch>GLV_MATBATCH_MAX||') == after_model
    before_header = (previous / 'source' / d / 'glass_local_model.h').read_text()
    after_header = (s / d / 'glass_local_model.h').read_text()
    assert before_header.replace('#define GLV_MAX_REPLY 4096\n',
                                 '#define GLV_MAX_REPLY 4096\n'
                                 '#define GLV_MATBATCH_MAX 32u\n') == after_header
    before_llm = (previous / 'source' / d / 'glass_local_llm.c').read_text()
    after_llm = (s / d / 'glass_local_llm.c').read_text()
    # Only workspace capacity/layout/selection and outer batch width may change.
    old_start = before_llm.index('  /* Eight positions need')
    new_start = after_llm.index('  /* Up to 32 positions reuse')
    end = '  char name[128];unsigned last=0;'
    old_end = before_llm.index(end, old_start)
    new_end = after_llm.index(end, new_start)
    normalized = before_llm[:old_start] + after_llm[new_start:new_end] + before_llm[old_end:]
    normalized = normalized.replace('if(batch>8)batch=8;',
                                    'if(batch>width)batch=width;')
    assert normalized == after_llm
    assert 'glv_alloc(s->memory,FFN*sizeof(float))' in after_llm[new_start:new_end]
    evidence = ('prefill15-final/VERIFICATION.json',
                'microphone-worker15-final/VERIFICATION.json')
    for relative in evidence:
        report = read(root / relative)
        assert report['passed']
        for name, digest in report['source_sha256'].items():
            assert sha(s / name) == digest, (relative, name)
    proof = read(root / evidence[0])
    assert proof['verifier_sha256'] == sha(s / 'tools/local_voice/verify_prefill_batch.py')
    assert {case['tokens'] for case in proof['cases']} == {1,7,8,9,16,17,31,32,33,64,512}
    assert {case['tokens'] for case in proof['sequential']} == {33,512}
    assert all(case['bit_exact'] for case in proof['cases'])
    assert proof['cancellation']['error'] == -125
    assert len(proof['budgets']) == 3
    inherited = ('candidate14/SD-DIAGNOSTIC-VERIFICATION.json',
                 'device-sd-read14/BOARD-SD-READ.json',
                 'candidate13/ISOLATION-VERIFICATION.json')
    for relative in inherited:
        assert read(root / relative)['passed']
    for name in ('ELF-AUDIT.json', 'XIP-REVIEW.json', 'RESET-ELF-AUDIT.json', 'HOTLOOP-AUDIT.json'):
        report = read(c / name)
        assert report['passed']
        if 'image_sha256' in report:
            assert report['image_sha256'] == meta['sha256']['nuttx.bin']
    symbols = subprocess.check_output([
        str(a.toolchain / 'riscv32-esp-elf-nm'), '-n', str(c / 'firmware/nuttx.elf')], text=True)
    addresses = {n: int(v, 16) for v, _, n in re.findall(
        r'^([0-9a-f]+)\s+(\S)\s+(\S+)$', symbols, re.M)}
    # GCC splits the validated entry and its nontrivial body into .part.N;
    # --gc-sections can then discard the unused standalone public wrapper.
    prefill = [name for name in addresses if re.fullmatch(
        r'glv_llm_prefill(?:\.part\.\d+)?', name)]
    assert len(prefill) == 1, prefill
    for name in (prefill[0], 'glv_matbatch', 'glv_llm_generate', 'glass_local_voice_new'):
        assert 0x48000000 <= addresses[name] < 0x4c000000
    disassembly = (c / 'linked-disassembly.txt').read_text()
    generate = re.search(r'^[0-9a-f]+ <glv_llm_generate>:\n(.*?)(?=^[0-9a-f]+ <)',
                         disassembly, re.M | re.S)
    assert generate and '<' + prefill[0] + '>' in generate[1]
    scope = read(c / 'scope.json')
    for row in scope['files']:
        assert sha(s / row['path']) == row['sha256']
    report = dict(
        passed=True, image_sha256=meta['sha256']['nuttx.bin'],
        elf_sha256=sha(c / 'firmware/nuttx.elf'),
        scope='Adaptive prefill only: actual Q4 bit-exact, low-memory, cancel, full host chain and native final link; human/physical pending',
        compiled_change_paths=sorted(changed),
        per_dot_product_float_order_unchanged=True,
        allocator_four_mib_reserve_and_twelve_mib_cap_unchanged=True,
        kernel_and_config_identical_to_candidate14=True,
        model_resources_tokenizer_chat_decode_audio_service_unchanged=True,
        linked_prefill_symbol=prefill[0],
        linked_prefill_address=hex(addresses[prefill[0]]),
        generate_calls_audited_prefill=True,
        board_tested=False, sd_root_cause_cured=False,
        candidate_source_sha256={row['path']: row['sha256'] for row in scope['files']},
        evidence_sha256={p: sha(root / p) for p in (*evidence, *inherited)})
    (c / 'PREFILL-VERIFICATION.json').write_text(json.dumps(report, indent=2) + '\n')
    print('ADAPTIVE_PREFILL_P4_LINK_VERIFIED')


if __name__ == '__main__':
    main()
