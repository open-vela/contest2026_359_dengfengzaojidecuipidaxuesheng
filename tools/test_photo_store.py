#!/usr/bin/env python3
"""Exercise the real asynchronous photo writer under slow I/O and cancellation."""
import argparse,os,subprocess,tempfile
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
a=p.parse_args();repo=Path(__file__).resolve().parents[1];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(prefix='photo-writer-') as fixture:
    for mode in [0,1]:
      exe=out/('photo-writer-'+str(mode))
      subprocess.run(['gcc','-std=gnu11','-D_GNU_SOURCE',*(['-DCONFIG_ESP32P4_JPEG=1'] if mode else []),'-O1','-g','-pthread','-Wall','-Wextra','-Werror',
       '-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie',
       '-DTEST_ROOT="'+fixture+'"','-I'+str(repo/'app/espdl-quickapp/overlay/apps/system/desktop'),
       str(repo/'app/espdl-quickapp/tests/photo_store_host.c'),'-o',str(exe)],check=True)
      subprocess.run([str(exe)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
