#!/usr/bin/env python3
"""Build the pinned C6 eight-link companion in isolation. Never flash/reset."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess

from prepare_c6_ble_capacity import apply

IDF_COMMIT = 'cf8dad074683e67181ec1aefee1f3079e0b59c26'
HOSTED_COMMIT = '0c4aa5f40ce0feebb454c1d671c39329c795b037'
PRESERVED = {
    'app_main.c': 'ec7fc5d9ee8f8771eccf99dd0b884e2bd70f11177d39cf303ee2cfb885ff07e4',
    'sdio_slave_api.c': 'c4e1a680693437f4deefd3d8bfb866ac2ca16231a19b957f37e2e1e0d1ae60fd',
}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def git(path, *args):
    return subprocess.check_output(['git', '-C', str(path), *args], text=True).strip()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline', type=Path, required=True, help='Pinned diagnostic C6 tree, containing esp-hosted-mcu and sdkconfig.tx-first-error')
    p.add_argument('--sdk-root', type=Path, required=True, help='Existing pinned esp-idf and idf-tools directory')
    p.add_argument('--workspace', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--jobs', type=int, default=6)
    a = p.parse_args()
    if not 1 <= a.jobs <= 32:
        p.error('jobs must be between 1 and 32')
    base, sdk_root, work, out = [v.resolve() for v in
                               (a.baseline, a.sdk_root, a.workspace, a.output)]
    if work == base or base in work.parents or work in base.parents:
        p.error('Use a separate workspace outside the baseline')
    if work.exists():
        p.error('Workspace already exists; preserve the first build and select a new directory')
    sdk, tools = sdk_root/'esp-idf', sdk_root/'idf-tools'
    source = base/'esp-hosted-mcu'
    config = base/'sdkconfig.tx-first-error'
    if git(sdk, 'rev-parse', 'HEAD') != IDF_COMMIT or git(source, 'rev-parse', 'HEAD') != HOSTED_COMMIT:
        p.error('Pinned SDK/Hosted revision mismatch')
    if git(sdk, 'status', '--porcelain', '--untracked-files=no'):
        p.error('SDK has tracked modifications')
    expected_status = {' M slave/main/app_main.c', ' M slave/main/sdio_slave_api.c'}
    # Preserve leading porcelain spaces when checking tracked modifications.
    status = subprocess.check_output(['git','-C',str(source),'status','--porcelain','--untracked-files=no'],text=True)
    if set(status.splitlines()) != expected_status:
        p.error('Unexpected baseline Hosted modifications')
    for name, expected in PRESERVED.items():
        if sha(source/'slave/main'/name) != expected:
            p.error('Pinned diagnostic/SDIO source mismatch: '+name)
    bt = source/'slave/main/slave_bt.c'
    if bt.read_bytes() != subprocess.check_output(['git','-C',str(source),'show','HEAD:slave/main/slave_bt.c']):
        p.error('Bluetooth source must match the pinned original before preparing eight connections')
    manifest = (source/'idf_component.yml').read_text()
    if 'version: "2.0.13"' not in manifest or 'version: ">=5.3"' not in manifest:
        p.error('Hosted/IDF compatibility evidence changed')
    envs = list((tools/'python_env').glob('idf5.5_py*_env'))
    if len(envs) != 1:
        p.error('One prepared IDF 5.5 Python environment required')
    venv = envs[0]
    compiler = tools/'tools/riscv32-esp-elf/esp-14.2.0_20241119/riscv32-esp-elf/bin'
    env = dict(os.environ, IDF_PATH=str(sdk), IDF_TOOLS_PATH=str(tools),
               IDF_PYTHON_ENV_PATH=str(venv), IDF_TARGET='esp32c6',
               PATH=f'{venv}/bin:{compiler}:'+os.environ['PATH'])
    py = str(venv/'bin/python')
    idf = [py, str(sdk/'tools/idf.py')]
    version = subprocess.check_output([*idf,'--version'],env=env,text=True).strip()
    if '5.5' not in version:
        p.error('Unexpected ESP-IDF version: '+version)
    out.mkdir(parents=True, exist_ok=True)
    baseline_hashes = {'config':sha(config), 'slave_bt.c':sha(bt),
                       **{n:sha(source/'slave/main'/n) for n in PRESERVED}}
    work.mkdir(parents=True)
    shutil.copytree(source, work/'esp-hosted-mcu', symlinks=True)
    prepared_config = work/'sdkconfig.ble-eight'
    shutil.copy2(config, prepared_config)
    project, build = work/'esp-hosted-mcu/slave', work/'build'
    prepared = apply(project, prepared_config)
    overrides = Path(__file__).resolve().parent/'c6/sdkconfig.ble-eight.defaults'
    defaults = ';'.join(map(str,[project/'sdkconfig.defaults',project/'sdkconfig.defaults.esp32c6',overrides]))
    cmd = [*idf,'-C',str(project),'-B',str(build),'-D','SDKCONFIG='+str(prepared_config),
           '-D','SDKCONFIG_DEFAULTS='+defaults,'-D','IDF_TARGET=esp32c6',
           '-D','CMAKE_JOB_POOLS=openvela_compile='+str(a.jobs),
           '-D','CMAKE_JOB_POOL_COMPILE=openvela_compile','build']
    metadata = {'idf_commit':IDF_COMMIT,'hosted_commit':HOSTED_COMMIT,'idf_version':version,
                'compatibility_evidence':'Hosted 2.0.13 manifest: IDF >=5.3; pinned SDK 5.5',
                'baseline':str(base),'workspace':str(work),'baseline_sha256':baseline_hashes,
                'preserved_source_sha256':PRESERVED,'preparation':prepared,'command':cmd,
                'device_actions':False}
    (out/'BUILD-INPUTS.json').write_text(json.dumps(metadata,indent=2)+'\n')
    with (out/'build.log').open('w') as log:
        result = subprocess.run(cmd,env=env,stdout=log,stderr=subprocess.STDOUT)
    (out/'BUILD-RESULT.json').write_text(json.dumps({'exit_code':result.returncode,'device_actions':False},indent=2)+'\n')
    if result.returncode:
        raise SystemExit(result.returncode)
    for name, expected in PRESERVED.items():
        if sha(project/'main'/name) != expected:
            raise RuntimeError('Existing fixes changed during build: '+name)
    if sha(config) != baseline_hashes['config'] or sha(bt) != baseline_hashes['slave_bt.c']:
        raise RuntimeError('Baseline changed during isolated build')
    artifacts = out/'artifacts';artifacts.mkdir(exist_ok=True)
    for name in ['network_adapter.bin','network_adapter.elf','network_adapter.map',
                 'compile_commands.json','project_description.json']:
        shutil.copy2(build/name,artifacts/name)
    for src,name in [(prepared_config,'sdkconfig'),(build/'config/sdkconfig.h','sdkconfig.h'),
                     (build/'partition_table/partition-table.bin','partition-table.bin'),
                     (project/'partitions.esp32c6.csv','partitions.esp32c6.csv'),
                     (project/'main/slave_bt.c','slave_bt.c')]:
        shutil.copy2(src,artifacts/name)
    with (out/'image-info.txt').open('w') as log:
        subprocess.run([py,'-m','esptool','--chip','esp32c6','image_info',str(artifacts/'network_adapter.bin')],
                       env=env,check=True,stdout=log,stderr=subprocess.STDOUT)
    with (out/'size.txt').open('w') as log:
        subprocess.run([*cmd[:-1],'size'],env=env,check=True,stdout=log,stderr=subprocess.STDOUT)
    print('C6_EIGHT_BUILD_OK: '+str(artifacts),flush=True)


if __name__ == '__main__':
    main()
