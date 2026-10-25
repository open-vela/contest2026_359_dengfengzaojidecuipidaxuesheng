#!/usr/bin/env python3
"""Check the production SD voice loader against pinned official voice data."""
import argparse
from pathlib import Path
import shutil
import subprocess
from esp_tts_inputs import verify_prepared,voice_config,voice_variants

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--prepared',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
lock=verify_prepared(args.prepared)
out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
for name,target,row in voice_variants(lock):
    shutil.copyfile(args.prepared/'upstream'/row['path'],out/target)
(out/'esp_tts_voice_config.h').write_text(voice_config(out))
repo=Path(__file__).resolve().parents[1];desktop=repo/'app/espdl-quickapp/overlay/apps/system/desktop'
executable=out/'voice-host'
subprocess.run(['gcc','-std=c11','-D_DEFAULT_SOURCE','-O1','-g','-pthread','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined',
    '-fno-omit-frame-pointer','-DGLASS_ESP_TTS_VOICE_SD','-I'+str(out),'-I'+str(desktop),
    str(desktop/'glass_esp_tts_voice.c'),str(desktop/'qpk_espdl_sha256.c'),
    str(repo/'app/espdl-quickapp/tests/esp_tts_voice_host.c'),'-Wl,--wrap=clock_gettime','-o',str(executable)],check=True)
subprocess.run([str(executable)],check=True,timeout=30)
