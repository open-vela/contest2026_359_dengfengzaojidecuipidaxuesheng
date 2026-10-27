#!/usr/bin/env python3
"""Exercise actual JPEG application adapter without hardware or display writes."""
from pathlib import Path
import argparse, hashlib, json, os, subprocess
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
repo = Path(__file__).resolve().parents[1]
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
desktop = repo/'app/espdl-quickapp/overlay/apps/system/desktop'
stubs = {'nuttx/config.h': '#pragma once\n#define CONFIG_ESP32P4_JPEG 1\n',
         'nuttx/compiler.h': '#pragma once\n#define FAR\n',
         'nuttx/fs/ioctl.h': '#pragma once\n#define _IOC(a,b) ((a)|(b))\n',
         'sys/ioctl.h': '#pragma once\nint ioctl(int, unsigned long, ...);\n'}
for name, text in stubs.items():
    path = out/name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
sources = [desktop/'qpk_jpeg_hw.c', repo/'app/espdl-quickapp/tests/p4_jpeg_adapter_host.c']
cmd = ['gcc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
       '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
       '-Dopen=jpeg_test_open', '-Dclose=jpeg_test_close', '-Dioctl=jpeg_test_ioctl',
       '-I'+str(out), '-I'+str(desktop),
       '-I'+str(repo/'app/espdl-quickapp/overlay/nuttx/include'),
       *map(str, sources), '-o', str(out/'adapter-host')]
for name, command in [('compile', cmd), ('run', [str(out/'adapter-host')])]:
    result = subprocess.run(command, capture_output=True, text=True, timeout=60,
        env={**os.environ, 'ASAN_OPTIONS':'detect_leaks=1:halt_on_error=1',
             'UBSAN_OPTIONS':'halt_on_error=1'})
    (out/(name+'.log')).write_text(result.stdout+result.stderr)
    if result.returncode: raise RuntimeError(result.stdout+result.stderr)
    print(result.stdout, end='')
sources += [desktop/'qpk_jpeg_hw.h', desktop/'qpk_jpeg_testdata.h', Path(__file__).resolve()]
(out/'RESULT.json').write_text(json.dumps({'passed':True,'hardware_tested':False,
    'source_sha256':{str(s):hashlib.sha256(s.read_bytes()).hexdigest() for s in sources}},indent=2)+'\n')
