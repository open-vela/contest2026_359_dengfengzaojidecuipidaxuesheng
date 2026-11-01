#!/usr/bin/env python3
"""Bind the minimal SD token diagnostic to differential tests and native link."""
import argparse
import hashlib
import importlib.util
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
    args = parser.parse_args()
    candidate = args.candidate.resolve()
    root = candidate.parent
    source = candidate / 'source'
    previous = root / 'candidate13'
    meta = read(candidate / 'firmware/BUILD-METADATA.json')
    prior = read(previous / 'firmware/BUILD-METADATA.json')
    assert sha(candidate / 'firmware/nuttx.bin') == meta['sha256']['nuttx.bin']
    assert meta['source_sha256'] == prior['source_sha256']
    for relative, digest in meta['source_sha256'].items():
        assert sha(source / relative) == digest, relative
    assert (candidate / 'firmware/resolved.config').read_bytes() == (
        previous / 'firmware/resolved.config').read_bytes()
    config = (candidate / 'firmware/resolved.config').read_text()
    for line in ('CONFIG_DEBUG_FS_ERROR=y', 'CONFIG_LIBC_LONG_LONG=y',
                 'CONFIG_MMCSD_SPIRETRY_COUNT=0',
                 'CONFIG_MMCSD_SPICLOCK=20000000'):
        assert line in config.splitlines()
    for key in ('name', 'address'):
        assert meta['ble_identity'][key] != prior['ble_identity'][key]
    relative = 'drivers/mmcsd/mmcsd_spi.c'
    expected = dict(prior['prepared_kernel_sources'])
    expected[relative] = meta['sd_read_diagnostics']['sha256']
    assert meta['prepared_kernel_sources'] == expected
    diagnostic = meta['sd_read_diagnostics']
    assert diagnostic['enabled']
    for flag in ('read_retry_added', 'write_path_changed',
                 'clock_or_timeout_changed'):
        assert diagnostic[flag] is False
    tool = source / 'tools/prepare_sd_read_diagnostics.py'
    assert diagnostic['tool_sha256'] == sha(tool)
    spec = importlib.util.spec_from_file_location('frozen_sd_diagnostic', tool)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    host = root / 'sd-read-diagnostics14-host-final'
    proof = read(host / 'VERIFICATION.json')
    assert proof['passed'] and proof['differential_cases'] == 10594
    assert proof['prepare_sha256'] == sha(tool)
    assert proof['tool_sha256'] == sha(source / 'tools/test_sd_read_diagnostics.py')
    before = host / 'mmcsd_spi-before.c'
    after = candidate / 'prepared-kernel' / relative
    assert sha(before) == proof['baseline_source_sha256']
    assert sha(after) == proof['diagnostic_source_sha256'] == diagnostic['sha256']
    assert module.transform(after.read_text(), False) == before.read_text()
    assert module.transform(before.read_text(), True) == after.read_text()
    assert read(host / 'STYLE-VERIFICATION.json')['passed']
    reports = ('ELF-AUDIT.json', 'XIP-REVIEW.json', 'RESET-ELF-AUDIT.json',
               'HOTLOOP-AUDIT.json')
    for name in reports:
        report = read(candidate / name)
        assert report['passed'], name
        if 'image_sha256' in report:
            assert report['image_sha256'] == meta['sha256']['nuttx.bin']
    inherited = ('candidate13/ISOLATION-VERIFICATION.json',
                 'device-new-session13-recovered/BOARD-NEW-SESSION.json')
    for name in inherited:
        assert read(root / name)['passed']
    syms = subprocess.check_output([
        str(args.toolchain / 'riscv32-esp-elf-nm'), '-n',
        str(candidate / 'firmware/nuttx.elf')], text=True)
    names = {name: int(address, 16) for address, _, name in re.findall(
        r'^([0-9a-f]+)\s+(\S)\s+(\S+)$', syms, re.M)}
    receive = [name for name in names if name.startswith('mmcsd_recvblock')]
    assert len(receive) == 1
    assert 0x48000000 <= names[receive[0]] < 0x4c000000
    # RV32 NuttX defines uint32_t as unsigned long (host libc uses unsigned
    # int). PRIu32 therefore emits %lu on this exact target. Preserve the
    # firmware's type-correct format instead of imposing the host spelling.
    assert b' elapsed_ticks=%jd limit_ticks=%jd polls=%lu\n' in (
        candidate / 'firmware/nuttx.bin').read_bytes()
    scope = read(candidate / 'scope.json')
    for row in scope['files']:
        assert sha(source / row['path']) == row['sha256']
    evidence = (*inherited, 'sd-read-diagnostics14-host-final/VERIFICATION.json',
                'sd-read-diagnostics14-host-final/STYLE-VERIFICATION.json')
    report = dict(
        passed=True, image_sha256=meta['sha256']['nuttx.bin'],
        elf_sha256=sha(candidate / 'firmware/nuttx.elf'),
        scope='SD token failure diagnostics only; not an SD reliability fix',
        compiled_overlay_inputs_identical_to_candidate13=True,
        kernel_config_identical_to_candidate13=True,
        receive_function_is_only_kernel_input_change=True,
        default_build_restores_original_receive_function=True,
        retries_clock_timeout_and_write_path_unchanged=True,
        diagnostic_receive_symbol=receive[0],
        diagnostic_receive_address=hex(names[receive[0]]),
        diagnostic_poll_format='%lu (target PRIu32)',
        board_tested=False, root_cause_cured=False,
        inherited_model_tests_remain_candidate13=True,
        candidate_source_sha256={row['path']: row['sha256']
                                 for row in scope['files']},
        evidence_sha256={name: sha(root / name) for name in evidence})
    (candidate / 'SD-DIAGNOSTIC-VERIFICATION.json').write_text(
        json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print('SD_DIAGNOSTIC_FINAL_LINK_OK')


if __name__ == '__main__':
    main()
