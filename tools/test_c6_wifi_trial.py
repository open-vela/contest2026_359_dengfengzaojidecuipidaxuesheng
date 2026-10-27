#!/usr/bin/env python3
"""Exercise RAM-first STA trials with real Hosted protobuf and lost replies."""

import argparse
import json
from pathlib import Path
import shutil
import subprocess

from prepare_c6_ota import apply as prepare_ota
from prepare_c6_softap import apply as prepare_softap
from prepare_c6_wifi_trial import apply


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apps', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--toolchain', type=Path)
    parser.add_argument('--nuttx', type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    src = repo / 'app/espdl-quickapp/overlay/apps/system/c6probe'
    sdk = args.apps / 'system/c6probe'
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    prepared = out / 'prepared/system/c6probe'
    prepared.mkdir(parents=True, exist_ok=True)
    for name in ['esp_hosted_rpc.c', 'Makefile']:
        shutil.copyfile(sdk / name, prepared / name)
    prepare_ota(prepared.parents[1])
    first = apply(prepared.parents[1])
    assert apply(prepared.parents[1]) == first
    prepare_ota(prepared.parents[1])
    assert apply(prepared.parents[1]) == first
    rpc = (prepared / 'esp_hosted_rpc.c').read_text()
    assert rpc.count('#include "c6_wifi_trial_rpc.inc"') == 1
    assert 'if (req == NULL || g_c6_ota_attempted ||\n      c6_wifi_trial_pending())' in rpc
    (prepared / 'esp_hosted_rpc.c').write_text('unknown owner\n')
    try:
        apply(prepared.parents[1])
    except ValueError:
        pass
    else:
        raise AssertionError('Unknown prepared owner must be rejected')
    subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-I' + str(src),
                    '-I' + str(sdk),
                    str(repo / 'app/espdl-quickapp/tests/c6_wifi_trial_host.c'),
                    str(sdk / 'esp_hosted_rpc.pb-c.c'),
                    str(sdk / 'protobuf-c.c'), '-pthread',
                    '-o', str(out / 'trial')], check=True)
    subprocess.run([str(out / 'trial')], check=True, timeout=30)
    # Run all three preparation layers twice in the actual builder order.
    integrated = out / 'integrated/system/c6probe'
    integrated.mkdir(parents=True, exist_ok=True)
    for name in ['esp_hosted_rpc.c', 'esp_hosted_rpc.pb-c.h', 'Makefile',
                 'esp_hosted.c', 'esp_hosted.h']:
        shutil.copyfile(sdk / name, integrated / name)
    for prepare in [prepare_ota, prepare_softap, apply]:
        prepare(integrated.parents[1])
    once = (integrated / 'esp_hosted_rpc.c').read_bytes()
    for prepare in [prepare_ota, prepare_softap, apply]:
        prepare(integrated.parents[1])
    assert (integrated / 'esp_hosted_rpc.c').read_bytes() == once
    target_compiled = False
    if args.toolchain or args.nuttx:
        assert args.toolchain and args.nuttx
        subprocess.run([str(args.toolchain / 'riscv32-esp-elf-gcc'),
                        '-c', '-Os', '-g', '-std=gnu11',
                        '-march=rv32imafc_zicsr_zifencei', '-mabi=ilp32f',
                        '-fstack-usage', '-Werror=implicit-function-declaration',
                        '-I' + str(src), '-I' + str(sdk),
                        '-I' + str(args.nuttx / 'include'),
                        '-I' + str(args.apps / 'include'),
                        str(integrated / 'esp_hosted_rpc.c'),
                        '-o', str(out / 'rpc-target.o')], check=True)
        target_compiled = True
    (out / 'RESULT.json').write_text(json.dumps({
        'host_only': True, 'device_updated': False,
        'real_protobuf_trial_and_full_old_config_restore': True,
        'every_begin_commit_abort_rpc_timeout': True,
        'malformed_and_remote_error_replies': True,
        'uncertain_commit_retains_snapshot_until_recovery': True,
        'ota_softap_trial_preparation_composed_twice': True,
        'actual_rpc_owner_target_unit_compiled': target_compiled,
        'preparation_idempotent_and_unknown_owner_rejected': True},
        indent=2) + '\n')


if __name__ == '__main__':
    main()
