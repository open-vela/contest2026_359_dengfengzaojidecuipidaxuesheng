#!/usr/bin/env python3
"""Check exact wake runtime arithmetic on the host with ASan and UBSan."""
import argparse
import hashlib
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
    source = repo / 'app/espdl-quickapp/tests/wake_runtime_host.c'
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    executable = output / 'wake-runtime-unit-host'
    command = [args.cc, '-std=gnu11', '-O1', '-g',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               '-I' + str(desktop), str(source), '-lm', '-o', str(executable)]
    env = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1',
           'UBSAN_OPTIONS': 'halt_on_error=1'}
    compile_result = subprocess.run(command, capture_output=True, text=True)
    (output / 'compile.log').write_text(compile_result.stdout + compile_result.stderr)
    compile_result.check_returncode()
    result = subprocess.run([str(executable)], env=env, capture_output=True,
                            text=True, timeout=120)
    (output / 'run.log').write_text(result.stdout + result.stderr)
    print(result.stdout, end='')
    if result.stderr:
        print(result.stderr, end='')
    result.check_returncode()
    assert 'WAKE_RUNTIME_HOST_OK ' in result.stdout
    paths = [source, desktop / 'glass_wake_features.h',
             desktop / 'glass_wake_features.c']
    report = {
        'status': 'passed',
        'environment': 'host only; no board, microphone or model service',
        'sanitizers': ['address', 'undefined'],
        'compiler_command': command,
        'checks': result.stdout.strip(),
        'source_sha256': {str(path.relative_to(repo)):
                          hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in paths},
    }
    (output / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
