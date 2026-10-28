#!/usr/bin/env python3
"""Validate actual asynchronous SD player under ASan/UBSan."""
import argparse,os,subprocess
from pathlib import Path
def main():
    p=argparse.ArgumentParser();
    for k in ['apps','cache','output','assets']:p.add_argument('--'+k,type=Path,required=True)
    a=p.parse_args();repo=Path(__file__).resolve().parents[1];desktop=repo/'app/espdl-quickapp/overlay/apps/system/desktop';a.output.mkdir(exist_ok=True,parents=True)
    objects=sorted(a.cache.glob('lvgl-*.o'))
    flags=['-std=gnu11','-O1','-g','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer','-DLV_CONF_INCLUDE_SIMPLE','-DGROK_HOST_TEST','-DGROK_ASSET_ROOT="'+str(a.assets)+'"']
    flags+=['-I'+str(x) for x in [desktop,a.apps/'graphics/lvgl',a.cache]]
    gaze_cmd=['gcc',*flags,str(repo/'app/espdl-quickapp/tests/grok_gaze_host.c'),'-o',str(a.output/'gaze-test')]
    with (a.output/'gaze-compile.log').open('w') as f:subprocess.run(gaze_cmd,stdout=f,stderr=subprocess.STDOUT,check=True)
    gaze=subprocess.run([str(a.output/'gaze-test'),str(a.assets)],env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'},capture_output=True,text=True)
    (a.output/'gaze.log').write_text(gaze.stdout+gaze.stderr);print(gaze.stdout+gaze.stderr,flush=True);gaze.check_returncode()
    cmd=['gcc',*flags,str(repo/'app/espdl-quickapp/tests/grok_sd_host.c'),str(desktop/'glass_grok_state.c'),*map(str,objects),'-lm','-o',str(a.output/'test')]
    with (a.output/'compile.log').open('w') as f:subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT,check=True)
    r=subprocess.run([str(a.output/'test'),str(a.output)],env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'},capture_output=True,text=True)
    (a.output/'native.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr);raise SystemExit(r.returncode)
if __name__=='__main__':main()
