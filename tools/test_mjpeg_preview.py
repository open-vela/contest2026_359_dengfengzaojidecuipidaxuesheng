#!/usr/bin/env python3
"""Cross-check reduced RGB preview against full JPEG decoding and photo isolation."""
import argparse,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--jpeg',type=Path,nargs='+',required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
repo=Path(__file__).resolve().parents[1];d=repo/'app/espdl-quickapp/overlay/apps/system/desktop'
binary=a.output/'preview-test'
subprocess.run(['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function',
  '-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie',
  '-I'+str(d),str(repo/'app/espdl-quickapp/tests/test_mjpeg_preview.c'),
  str(d/'qpk_mjpeg.c'),str(d/'qpk_tjpgd.c'),'-o',str(binary)],check=True)
for jpeg in a.jpeg:
  print('Fixture:',jpeg.name,flush=True)
  subprocess.run([str(binary),str(jpeg)],check=True,
    env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
