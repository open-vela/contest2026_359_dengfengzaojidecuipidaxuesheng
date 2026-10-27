#!/usr/bin/env python3
"""Exercise production portal network HTTP routes over native loopback TCP."""
import argparse
import hashlib
import json
import os
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apps', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    desktop = repo / 'app/espdl-quickapp/overlay/apps/system/desktop'
    fixture = repo / 'app/espdl-quickapp/tests/portal_network_host.c'
    cjson = args.apps.resolve() / 'netutils/cjson/cJSON'
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / 'nuttx').mkdir(exist_ok=True)
    (out / 'nuttx/config.h').write_text('#pragma once\n')
    binary = out / 'portal-network-http'
    command = ['gcc', '-std=gnu11', '-D_GNU_SOURCE', '-O1', '-g', '-pthread',
               '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               '-fno-pie', '-no-pie', '-ffunction-sections', '-fdata-sections',
               '-I' + str(out), '-I' + str(desktop),
               '-I' + str(repo / 'tools/c6'), '-I' + str(cjson),
               str(fixture), str(cjson / 'cJSON.c'),
               '-Wl,--gc-sections', '-o', str(binary)]
    with (out / 'compile.log').open('w') as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        print((out / 'compile.log').read_text())
        raise SystemExit(result.returncode)
    result = subprocess.run([str(binary)], text=True, capture_output=True,
                            timeout=30, env={**os.environ,
                                'ASAN_OPTIONS': 'detect_leaks=1:halt_on_error=1',
                                'UBSAN_OPTIONS': 'halt_on_error=1'})
    (out / 'run.log').write_text(result.stdout + result.stderr)
    print(result.stdout + result.stderr, end='')
    if result.returncode or 'PORTAL_NETWORK_HTTP_OK:' not in result.stdout:
        raise SystemExit(result.returncode or 1)
    files = [fixture, desktop / 'glass_portal_http.c', desktop / 'glass_portal.h',
             desktop / 'glass_network_recovery.h', repo / 'tools/c6/desktop_worker.h',
             cjson / 'cJSON.c', cjson / 'cJSON.h', Path(__file__).resolve()]
    report = {'passed': True, 'scope': 'native HTTP routes and fake services; no board',
              'sanitizers': ['address', 'undefined'],
              'source_sha256': {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                                for path in files}}
    (out / 'RESULT.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')


if __name__ == '__main__':
    main()
