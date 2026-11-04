#!/usr/bin/env python3
"""Run the production material loader against the published UI SD package."""
from pathlib import Path
import argparse,os,subprocess
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
r=Path(__file__).resolve().parents[1];d=r/'app/espdl-quickapp/overlay/apps/system/desktop';o=a.output.resolve();o.mkdir(parents=True,exist_ok=True)
package=r/'materials/board109-ui/desktop-assets/v2';exe=o/'material-cache'
subprocess.run(['gcc','-std=gnu11','-O1','-g','-ffp-contract=off','-fno-pie','-no-pie','-fsanitize=address,undefined','-pthread','-DGLASS_MATERIAL_ROOT="'+str(package)+'"','-I'+str(d),str(d/'glass_material.c'),str(r/'app/espdl-quickapp/tests/material_cache_host.c'),'-o',str(exe)],check=True)
run=subprocess.run([str(exe)],capture_output=True,text=True,timeout=120,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
(o/'test.log').write_text(run.stdout+run.stderr,encoding='utf-8');print(run.stdout+run.stderr,end='');run.check_returncode()
