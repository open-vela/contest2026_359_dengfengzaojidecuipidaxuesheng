#!/usr/bin/env python3
"""Exercise production native motion with real LVGL and ASan/UBSan."""
import argparse
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('apps', 'cache', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    args.output.mkdir(parents=True, exist_ok=True)
    objects = sorted(args.cache.glob('lvgl-*.o'))
    assert objects, 'Prepared sanitizer LVGL host cache required'
    flags = ['-std=gnu11', '-O1', '-g', '-pthread', '-fsanitize=address,undefined',
             '-fno-sanitize-recover=all', '-DLV_CONF_INCLUDE_SIMPLE',
             '-I' + str(repo / 'app/espdl-quickapp/overlay/apps/system/desktop'),
             '-I' + str(args.apps / 'graphics/lvgl'), '-I' + str(args.cache)]
    command = ['gcc', *flags, str(repo / 'app/espdl-quickapp/tests/interaction_motion_host.c'),
               str(repo / 'app/espdl-quickapp/overlay/apps/system/desktop/glass_icons.c'),
               *map(str, objects), '-lm', '-o', str(args.output / 'test')]
    with (args.output / 'compile.log').open('w') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    result = subprocess.run([str(args.output / 'test')], capture_output=True, text=True,
                            env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1',
                                 'UBSAN_OPTIONS': 'halt_on_error=1'})
    (args.output / 'native.log').write_text(result.stdout + result.stderr)
    print(result.stdout + result.stderr)
    result.check_returncode()


if __name__ == '__main__':
    main()
