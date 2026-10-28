#!/usr/bin/env python3
"""Test the real one-shot wake control loop with inert audio/model adapters."""
import argparse,subprocess
from pathlib import Path
from render_all_ui import function
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
repo=Path(__file__).resolve().parents[1];desktop=repo/'app/espdl-quickapp/overlay/apps/system/desktop'
a.output.mkdir(parents=True,exist_ok=True)
text=(desktop/'glass_speech.c').read_text()
(a.output/'wake_handoff_production.inc').write_text('#include "glass_ai_config.h"\n'+'\n'.join(function(text,name) for name in ['speech_publish','glass_speech_capture_woke','speech_stop','glass_speech_yield_background','speech_wait_wake','speech_complete_locked','speech_complete','speech_next_job','speech_start_mode_ungated','speech_start_mode']))
subprocess.run(['gcc','-O2','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread',
 '-I'+str(a.output),'-I'+str(desktop),str(repo/'app/espdl-quickapp/tests/wake_handoff_host.c'),'-o',str(a.output/'wake-handoff-host')],check=True)
subprocess.run([str(a.output/'wake-handoff-host')],check=True,timeout=30)
text=(desktop/'glass_voice_capture.inc').read_text()
struct=text[text.index('struct voice_capture {'):text.index('static int capture_open')]
(a.output/'voice_capture_production.inc').write_text(struct+'\n'+function(text,'capture_open')+'\n'+function(text,'capture_close')+'\n'+function((desktop/'glass_voice.c').read_text(),'ws_wait_start'))
subprocess.run(['gcc','-O2','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread',
 '-I'+str(a.output),str(repo/'app/espdl-quickapp/tests/voice_capture_cancel_host.c'),'-o',str(a.output/'voice-capture-host')],check=True)
subprocess.run([str(a.output/'voice-capture-host')],check=True,timeout=30)
