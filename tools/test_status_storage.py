#!/usr/bin/env python3
"""Exercise the production TF status provider without touching media."""
import argparse
import os
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();repo=Path(__file__).resolve().parents[1];out=a.output.resolve()
(out/'nuttx').mkdir(parents=True,exist_ok=True)
(out/'nuttx/config.h').write_text('#pragma once\n#define CONFIG_ESP32P4_SDMMC 1\n')
desktop=repo/'app/espdl-quickapp/overlay/apps/system/desktop'
subprocess.run(['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
  '-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread',
  '-I'+str(out),'-I'+str(desktop),str(repo/'app/espdl-quickapp/tests/status_storage_host.c'),
  '-o',str(out/'status-storage-host')],check=True)
result=subprocess.run([str(out/'status-storage-host')],capture_output=True,text=True,
  env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'})
(out/'test.log').write_text(result.stdout+result.stderr)
print(result.stdout,end='')
if result.returncode:print(result.stderr,end='')
result.check_returncode()
