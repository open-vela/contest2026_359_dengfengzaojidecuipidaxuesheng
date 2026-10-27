#!/usr/bin/env python3
"""Verify the actual RGB565 encoder wrapper with a controlled ioctl endpoint."""
from pathlib import Path
import argparse,hashlib,json,os,subprocess
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
a=p.parse_args();repo=Path(__file__).resolve().parents[1];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
d=repo/'app/espdl-quickapp/overlay/apps/system/desktop'
for name,body in {'nuttx/config.h':'#pragma once\n#define CONFIG_ESP32P4_JPEG 1\n',
 'nuttx/compiler.h':'#pragma once\n#define FAR\n','nuttx/fs/ioctl.h':'#pragma once\n#define _IOC(a,b) ((a)|(b))\n',
 'sys/ioctl.h':'#pragma once\nint ioctl(int,unsigned long,...);\n'}.items():
 f=out/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_text(body)
sources=[d/'qpk_jpeg_encode.c',repo/'app/espdl-quickapp/tests/p4_jpeg_encode_adapter_host.c']
cmd=['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
 '-fno-omit-frame-pointer','-fno-pie','-no-pie','-ffunction-sections','-fdata-sections',
 '-Dopen=jpeg_test_open','-Dclose=jpeg_test_close','-Dioctl=jpeg_test_ioctl',
 '-I'+str(out),'-I'+str(d),'-I'+str(repo/'app/espdl-quickapp/overlay/nuttx/include'),
 *map(str,sources),'-Wl,--gc-sections','-o',str(out/'encoder-adapter')]
for name,command in [('compile',cmd),('run',[str(out/'encoder-adapter')])]:
 r=subprocess.run(command,capture_output=True,text=True,timeout=60,
  env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
 (out/(name+'.log')).write_text(r.stdout+r.stderr)
 if r.returncode:raise RuntimeError(r.stdout+r.stderr)
 print(r.stdout,end='')
(out/'RESULT.json').write_text(json.dumps({'passed':True,'hardware_tested':False,
 'source_sha256':{str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in sources}},indent=2)+'\n')
