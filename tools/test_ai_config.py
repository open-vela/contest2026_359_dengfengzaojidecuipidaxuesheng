#!/usr/bin/env python3
"""Exercise the enabled production AI configuration lifecycle under sanitizers."""
from pathlib import Path
import argparse,os,subprocess
p=argparse.ArgumentParser();p.add_argument('--apps',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--local-voice',action='store_true');a=p.parse_args()
repo=Path(__file__).resolve().parents[1];d=repo/'app/espdl-quickapp/overlay/apps/system/desktop'
a.output.mkdir(parents=True,exist_ok=True);(a.output/'nuttx').mkdir(exist_ok=True);(a.output/'nuttx/config.h').write_text('#pragma once\n')
c=a.apps/'netutils/cjson/cJSON';exe=a.output/'ai-config'
subprocess.run(['gcc',*(['-DGLASS_LOCAL_VOICE'] if a.local_voice else []),'-std=gnu11','-D_GNU_SOURCE','-DGLASS_AI_CONFIG_ENABLED','-DGLASS_LOCAL_SPEECH','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
 '-O1','-g','-pthread','-fsanitize=address,undefined','-fno-pie','-no-pie','-ffunction-sections','-fdata-sections',
 '-I'+str(a.output),'-I'+str(d),'-I'+str(c),str(repo/'app/espdl-quickapp/tests/ai_config_host.c'),
 str(d/'glass_ai_config.c'),str(d/'glass_portal_config.c'),str(d/'glass_agent_config.c'),str(c/'cJSON.c'),'-Wl,--gc-sections','-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True,timeout=30,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
