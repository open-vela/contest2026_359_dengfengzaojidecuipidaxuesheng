#!/usr/bin/env python3
"""Check the production LCD PWM service using an inert single-channel device."""
import argparse,os,subprocess,json
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
repo=Path(__file__).resolve().parents[1];desktop=repo/'app/espdl-quickapp/overlay/apps/system/desktop';a.output.mkdir(parents=True,exist_ok=True)
for name,pin in [('lcd',26),('generic',2)]:
 out=a.output/name;(out/'nuttx/timers').mkdir(parents=True,exist_ok=True)
 (out/'nuttx/config.h').write_text('#pragma once\n#define CONFIG_PWM 1\n#define CONFIG_ESPRESSIF_LEDC_TIMER0 1\n#define CONFIG_ESPRESSIF_LEDC_TIMER0_CHANNELS 1\n#define CONFIG_ESPRESSIF_LEDC_CHANNEL0_PIN '+str(pin)+'\n')
 (out/'nuttx/timers/pwm.h').write_text('#pragma once\n#include <stdint.h>\n#define PWMIOC_SETCHARACTERISTICS 101\n#define PWMIOC_START 102\nstruct pwm_info_s {uint32_t frequency;uint32_t duty;};\n')
 command=['gcc','-std=gnu11','-D_GNU_SOURCE','-O1','-g','-Wall','-Wextra','-Werror','-U_FORTIFY_SOURCE','-D_FORTIFY_SOURCE=0','-fsanitize=address,undefined','-fno-pie','-no-pie','-pthread','-Dopen=backlight_test_open','-Dioctl=backlight_test_ioctl','-I'+str(out),'-I'+str(desktop),str(desktop/'glass_backlight.c'),str(repo/'app/espdl-quickapp/tests/backlight_host.c'),'-o',str(out/'test')]
 subprocess.run(command,check=True)
 subprocess.run([str(out/'test')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
(a.output/'RESULT.json').write_text(json.dumps({'passed':True,'variants':['lcd GPIO26','generic GPIO2 unavailable'],'hardware_tested':False},indent=2))
