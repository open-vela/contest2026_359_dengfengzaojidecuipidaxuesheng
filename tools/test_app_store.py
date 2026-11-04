#!/usr/bin/env python3
"""Compile and exercise the real offline store on disposable host volumes."""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--apps', required=True, type=Path)
p.add_argument('--quickjs-cache', required=True, type=Path)
p.add_argument('--cloud-fixture', type=Path, help='Catalog and app.js exported from the actual cloud API')
a = p.parse_args()
repo = Path(__file__).resolve().parents[1]
desktop = repo / 'app/espdl-quickapp/overlay/apps/system/desktop'
qjs = a.apps / 'interpreters/quickjs/quickjs'
subprocess.run(['python3', str(repo/'tools/generate_app_store.py'), '--check'], check=True)
with tempfile.TemporaryDirectory(prefix='app-store-') as tmp:
    root = Path(tmp)
    flags = ['-std=gnu11', '-D_GNU_SOURCE', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-parameter', '-Wno-cast-function-type', '-ffunction-sections', '-fdata-sections',
             '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-DCONFIG_BIGNUM',
             f'-DTEST_ROOT="{tmp}"', f'-DQPK_VOLUME_INTERNAL_ROOT="{tmp}/data/qpk"',
             f'-DQPK_VOLUME_SD_ROOT="{tmp}/sd/qpk"', f'-DGLASS_STORE_REPOSITORY="{tmp}/sd/app-store"',
             '-I'+str(desktop), '-I'+str(qjs), '-I'+str(a.apps/'netutils/cjson/cJSON')]
    # Keep strict diagnostics for new service/tests, isolate a pre-existing SHA
    # streaming-helper formatting warning without relaxing store diagnostics.
    sha = root/'sha.o'
    subprocess.run(['cc', *flags, '-Wno-error=misleading-indentation', '-c', str(desktop/'qpk_espdl_sha256.c'), '-o', str(sha)], check=True)
    sources = [repo/'app/espdl-quickapp/tests/app_store_host.c', desktop/'glass_store_http.c', a.apps/'netutils/cjson/cJSON/cJSON.c', sha]
    objects = [a.quickjs_cache/(n+'.o') for n in ['quickjs.c', 'libregexp.c', 'libunicode.c', 'cutils.c', 'libbf.c']]
    subprocess.run(['cc', *flags, *map(str, sources), *map(str, objects), '-Wl,--gc-sections', '-lm', '-ldl', '-pthread', '-o', str(root/'test')], check=True)
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1', UBSAN_OPTIONS='halt_on_error=1')
    if a.cloud_fixture:env['STORE_CLOUD_FIXTURE']=str(a.cloud_fixture.resolve())
    subprocess.run([str(root/'test')], check=True, timeout=90, env=env)
