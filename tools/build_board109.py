#!/usr/bin/env python3
"""Build the board109 feature profile using explicitly prepared openvela inputs.

This never flashes hardware. The existing builder generates a fresh BLE test
identity per build; the historical deployed image hash belongs to its receipt.
"""
from pathlib import Path
import argparse,subprocess,sys
p=argparse.ArgumentParser(description=__doc__)
for name in ['baseline','workspace','toolchain','archive','output','speech-runtime','esp-tts-prepared','local-voice-sdk']:
    p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--source-revision',help='Explicit 40-hex source revision for Windows-mounted worktrees; forwarded to the builder')
p.add_argument('--jobs',type=int,default=6);a=p.parse_args();r=Path(__file__).resolve().parents[1]
cmd=[sys.executable,str(r/'tools/build_bilibili_v3.py')]
for name in ['baseline','workspace','toolchain','archive','output','speech-runtime','esp-tts-prepared','local-voice-sdk']:
    cmd.extend(['--'+name,str(getattr(a,name.replace('-','_')).resolve())])
flags=['--local-session-guard','--local-q4-pie','--local-tts-pie','--local-tts-int8','--local-sha-hw','--local-tts-parallel','--local-tts-fma',
       '--local-asr-evaluation','--local-asr-accel','--local-asr-official-dsp','--local-asr-streaming','--local-asr-microphone',
       '--sd-read-diagnostics','--sd-spi-dma','--no-native-browser','--offline-inputs','--usb-camera','--factory-header','--psram-xip','--hardware-jpeg',
       '--video-blit','--lcd-backlight','--ethernet-share','--share-net-stats','--ble-eight-test']
cmd+=flags+['--share-forward-slots','16','--jobs',str(a.jobs)]
if a.source_revision:cmd+=['--source-revision',a.source_revision]
subprocess.run(cmd,check=True)
