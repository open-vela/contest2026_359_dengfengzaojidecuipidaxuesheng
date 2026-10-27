#!/usr/bin/env python3
"""Exercise bounded JPEG header generation using real parser and sanitizers."""
from pathlib import Path
import argparse,hashlib,json,os,subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();repo=Path(__file__).resolve().parents[1]
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
for name,body in {'nuttx/config.h':'#pragma once\n#define OK 0\n',
 'nuttx/compiler.h':'#pragma once\n#define FAR\n',
 'nuttx/fs/ioctl.h':'#pragma once\n#define _IOC(a,b) ((a)|(b))\n'}.items():
 f=out/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_text(body)
chip=repo/'app/espdl-quickapp/overlay/nuttx/arch/risc-v/src/esp32p4'
sources=[repo/'app/espdl-quickapp/tests/p4_jpeg_emit_host.c',
 chip/'esp32p4_jpeg_emit.c',chip/'esp32p4_jpeg_parse.c']
cmd=['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
 '-fno-omit-frame-pointer','-fno-pie','-no-pie','-I'+str(out),'-I'+str(chip),
 '-I'+str(repo/'app/espdl-quickapp/overlay/nuttx/include'),*map(str,sources),'-o',str(out/'emit-host')]
for name,command in [('compile',cmd),('run',[str(out/'emit-host'),str(out)])]:
 r=subprocess.run(command,capture_output=True,text=True,timeout=90,
  env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
 (out/(name+'.log')).write_text(r.stdout+r.stderr)
 if r.returncode:raise RuntimeError(r.stdout+r.stderr)
 print(r.stdout,end='')
(out/'RESULT.json').write_text(json.dumps({'passed':True,'hardware_tested':False,
 'headers':24,'source_sha256':{str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in sources}},indent=2)+'\n')
