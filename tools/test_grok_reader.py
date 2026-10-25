#!/usr/bin/env python3
"""Compare real LVGL pixels and SD traffic with the idle cache disabled/enabled."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('apps', 'cache', 'assets', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    desktop = repo / 'app/espdl-quickapp/overlay/apps/system/desktop'
    args.output.mkdir(parents=True, exist_ok=True)
    objects = sorted(args.cache.glob('lvgl-*.o'))
    assert objects, 'Prepared LVGL host cache is required'
    flags = ['-std=gnu11', '-O1', '-g', '-pthread', '-fsanitize=address,undefined',
             '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
             '-DLV_CONF_INCLUDE_SIMPLE', '-DGROK_ASSET_ROOT="' + str(args.assets) + '"']
    flags += ['-I' + str(p) for p in (desktop, args.apps/'graphics/lvgl', args.cache)]
    results = {}
    for name, budget in [('uncached', 0), ('cached', 512*1024)]:
        binary = args.output/name
        command = ['gcc', *flags, '-DGROK_FRAME_CACHE_BUDGET=' + str(budget),
                   str(repo/'app/espdl-quickapp/tests/grok_cache_player_host.c'),
                   str(desktop/'glass_grok_state.c'), *map(str, objects), '-lm', '-o', str(binary)]
        with (args.output/(name+'-compile.log')).open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
        run = subprocess.run([str(binary), str(args.output/(name+'.trace'))],
                             env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1',
                                  'UBSAN_OPTIONS': 'halt_on_error=1'}, capture_output=True, text=True)
        (args.output/(name+'.log')).write_text(run.stdout+run.stderr)
        print(name, run.stdout+run.stderr, flush=True)
        run.check_returncode()
        results[name] = json.loads(re.search(r'^CACHE_PLAYER_METRIC (.*)$', run.stdout, re.M)[1])
    before = (args.output/'uncached.trace').read_bytes()
    after = (args.output/'cached.trace').read_bytes()
    assert before == after, 'Animation order, original pixels or rendered framebuffer changed'
    assert results['uncached']['repeat_bytes'] > 0
    assert results['cached']['repeat_bytes'] == 0
    results['method'] = 'Real LVGL with virtual time, real GBS banks, ASan/UBSan; not board CPU/FPS'
    results['identical_frame_hashes'] = len(after.splitlines())
    results['trace_sha256'] = hashlib.sha256(after).hexdigest()
    results['source_sha256'] = {name: hashlib.sha256((desktop/name).read_bytes()).hexdigest()
                                for name in ('glass_grok_player.inc', 'glass_grok_cache.h')}
    results['controls_and_buffers_reused'] = True
    results['io_comparison'] = ('repeat_bytes compares two full warmed idle loops per palette; '
                                'whole-run cached sd_bytes also includes separate fault-injection frames')
    (args.output/'comparison.json').write_text(json.dumps(results, indent=2)+'\n')
    print(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
