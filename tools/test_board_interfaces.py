#!/usr/bin/env python3
"""Validate the production board catalog for enabled, legacy-pin and disabled builds."""
import argparse
import os
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--apps',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[1]
desktop=root/'app/espdl-quickapp/overlay/apps/system/desktop'
cjson=a.apps/'netutils/cjson/cJSON'
features=['ESPRESSIF_MIPI_DSI','INPUT_GT9XX','ESPRESSIF_MIPI_CSI','AUDIO_ES8311',
          'ESP32P4_SDMMC','FS_SMARTFS','ESPRESSIF_EMAC','ESPRESSIF_I2C1',
          'ESPRESSIF_SPI2','ESPRESSIF_UART1','ESPRESSIF_LEDC','ESP32P4_CAMERA_USB',
          'ESPRESSIF_USBSERIAL_CONSOLE','SYSTEM_C6_DESKTOP']
for name in ['enabled','legacy','disabled','backlight']:
    out=a.output/name;(out/'nuttx').mkdir(parents=True,exist_ok=True)
    config='#pragma once\n'
    if name!='disabled':config+=''.join('#define CONFIG_'+f+' 1\n' for f in features)
    if name!='disabled':config+='#define CONFIG_NIMBLE_BLE_MAX_CONN '+('2' if name=='legacy' else '3')+'\n'
    (out/'nuttx/config.h').write_text(config)
    flags=['-std=gnu11','-D_GNU_SOURCE','-O1','-g','-Wall','-Wextra','-Werror',
           '-Wno-misleading-indentation','-fsanitize=address,undefined',
           '-fno-omit-frame-pointer','-fno-pie','-no-pie']
    if name=='legacy':flags+=['-DLEGACY_PINS','-DCONFIG_ESPRESSIF_I2C1_SDAPIN=1',
        '-DCONFIG_ESPRESSIF_I2C1_SCLPIN=2','-DCONFIG_ESPRESSIF_SPI2_CSPIN=0',
        '-DCONFIG_ESPRESSIF_SPI2_CLKPIN=3','-DCONFIG_ESPRESSIF_SPI2_MOSIPIN=4',
        '-DCONFIG_ESPRESSIF_SPI2_MISOPIN=5','-DCONFIG_ESPRESSIF_LEDC_CHANNEL0_PIN=0',
        '-DCONFIG_ESPRESSIF_LEDC_CHANNEL1_PIN=3','-DCONFIG_ESPRESSIF_LEDC_CHANNEL2_PIN=4',
        '-DCONFIG_ESPRESSIF_LEDC_CHANNEL3_PIN=6']
    if name=='disabled':flags+=['-DFEATURES_OFF']
    if name=='backlight':flags+=['-DBACKLIGHT_PINS','-DCONFIG_ESPRESSIF_LEDC_CHANNEL0_PIN=26','-DCONFIG_ESPRESSIF_LEDC_CHANNEL1_PIN=3','-DCONFIG_ESPRESSIF_LEDC_CHANNEL2_PIN=4','-DCONFIG_ESPRESSIF_LEDC_CHANNEL3_PIN=6']
    command=['gcc',*flags,'-I'+str(out),'-I'+str(desktop),'-I'+str(cjson),
        str(root/'app/espdl-quickapp/tests/board_interfaces_host.c'),
        str(desktop/'qpk_board_interfaces.c'),str(cjson/'cJSON.c'),'-o',str(out/'test')]
    subprocess.run(command,check=True)
    subprocess.run([str(out/'test')],check=True,env={**os.environ,
        'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
