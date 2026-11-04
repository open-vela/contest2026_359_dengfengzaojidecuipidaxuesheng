#!/usr/bin/env python3
"""Exercise real JBOX tool schemas, Agent routing and async scheduler offline."""
import argparse
import json
import os
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--apps', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
desktop = root / 'app/espdl-quickapp/overlay/apps/system/desktop'
a.output.mkdir(parents=True, exist_ok=True)
(a.output / 'nuttx').mkdir(exist_ok=True)
(a.output / 'nuttx/config.h').write_text('#pragma once\n')
cjson = a.apps / 'netutils/cjson/cJSON'
flags = ['-std=gnu11', '-D_GNU_SOURCE', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-Wno-misleading-indentation', '-Wno-format-truncation',
         '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
         '-ffunction-sections', '-fdata-sections', '-pthread',
         '-I' + str(a.output), '-I' + str(desktop), '-I' + str(cjson)]
sources = [root / 'app/espdl-quickapp/tests/jbox_tools_host.c', desktop / 'glass_agent.c',
           desktop / 'glass_agent_config.c', desktop / 'glass_portal_config.c',
           desktop / 'glass_chat_store.c', desktop / 'qpk_hardware.c', cjson / 'cJSON.c']
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1', UBSAN_OPTIONS='halt_on_error=1')
logs = []
for command in [['gcc', *flags, *map(str, sources), '-Wl,--gc-sections', '-o', str(a.output / 'test')],
                [str(a.output / 'test')]]:
    run = subprocess.run(command, capture_output=True, text=True, env=env)
    logs.append(run.stdout + run.stderr)
    (a.output / 'test.log').write_text(''.join(logs), encoding='utf-8')
    if run.returncode:
        print(logs[-1])
        raise SystemExit(run.returncode)
print(''.join(logs))
(a.output / 'verification.json').write_text(json.dumps(dict(
    passed=True, scope='Real Agent schemas, direct/MCP dispatch and hardware scheduler; '
    'platform is a deterministic fixture without LAN, binding, device or firmware claims.'), indent=2) + '\n')
