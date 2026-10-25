#!/usr/bin/env python3
"""Check the standalone Grok codec and concurrent cache under ASan/UBSan."""
import argparse
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cc', default='gcc')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    desktop = repo / 'app/espdl-quickapp/overlay/apps/system/desktop'
    args.output.mkdir(parents=True, exist_ok=True)
    binary = args.output / 'grok-cache-host'
    command = [args.cc, '-std=gnu11', '-O1', '-g', '-pthread',
               '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
               '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
               '-I' + str(desktop),
               str(repo / 'app/espdl-quickapp/tests/grok_cache_host.c'),
               '-o', str(binary)]
    with (args.output / 'compile.log').open('w') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    result = subprocess.run([str(binary.resolve())],
                            env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1',
                                 'UBSAN_OPTIONS': 'halt_on_error=1'},
                            capture_output=True, text=True)
    (args.output / 'native.log').write_text(result.stdout + result.stderr)
    print(result.stdout + result.stderr, end='', flush=True)
    result.check_returncode()
    assert 'GROK_CACHE_HOST_OK' in result.stdout
    report = {'method': 'standalone host unit checks; ASan/UBSan with leak detection',
              'result': result.stdout.strip()}
    (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
