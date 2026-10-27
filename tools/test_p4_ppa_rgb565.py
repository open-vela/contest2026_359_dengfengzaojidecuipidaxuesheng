#!/usr/bin/env python3
"""Run real RGB565 PPA/JPEG application code with a deterministic I/O peer."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();repo=Path(__file__).resolve().parents[1];out=a.output.resolve()
out.mkdir(parents=True,exist_ok=True)
desktop=repo/'app/espdl-quickapp/overlay/apps/system/desktop'
for name,text in {
 'nuttx/config.h':'#pragma once\n#define CONFIG_ESP32P4_PPA 1\n#define CONFIG_ESP32P4_JPEG 1\n',
 'nuttx/compiler.h':'#pragma once\n#define FAR\n',
 'nuttx/fs/ioctl.h':'#pragma once\n#define _IOC(a,b) ((a)|(b))\n',
 'sys/ioctl.h':'#pragma once\nint ioctl(int,unsigned long,...);\n'}.items():
 f=out/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_text(text)
sources=[desktop/n for n in ['qpk_ppa.c','qpk_jpeg_hw.c','qpk_mjpeg.c','qpk_tjpgd.c']]
sources.append(repo/'app/espdl-quickapp/tests/p4_ppa_rgb565_host.c')
flags=['gcc','-std=gnu11','-D_GNU_SOURCE','-D__NuttX__','-O1','-g','-Wall','-Wextra','-Werror',
 '-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie',
 '-Dopen=ppa_test_open','-Dclose=ppa_test_close','-Dioctl=ppa_test_ioctl',
 '-I'+str(out),'-I'+str(desktop),'-I'+str(repo/'app/espdl-quickapp/overlay/nuttx/include')]
for name,cmd in [('compile',flags+list(map(str,sources))+['-o',str(out/'app-host')]),('run',[str(out/'app-host')])]:
 result=subprocess.run(cmd,capture_output=True,text=True,timeout=60,
  env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
 (out/(name+'.log')).write_text(result.stdout+result.stderr)
 if result.returncode:raise RuntimeError(name+' failed:\n'+result.stdout+result.stderr)
 print(result.stdout,end='')
sources.extend([desktop/'qpk_ppa.h',desktop/'qpk_jpeg_hw.h',Path(__file__).resolve()])
(out/'RESULT.json').write_text(json.dumps({'passed':True,'hardware_tested':False,
 'scope':'real PPA/JPEG/MJPEG adapters with mocked synchronous device transport; no physical pixel claim',
 'source_sha256':{str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in sources}},indent=2)+'\n')
