#!/usr/bin/env python3
"""Audit and export a linked candidate; never flash it."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--target',type=Path,required=True)
p.add_argument('--toolchain',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
target=a.target.resolve()
out=a.output.resolve(); out.mkdir(parents=True,exist_ok=True)
prefix=str(a.toolchain.resolve()/'riscv32-esp-elf-')
elf=target/'nuttx/nuttx'
binary=target/'nuttx/nuttx.bin'
config=(target/'nuttx/.config').read_text()
nm=subprocess.check_output([prefix+'nm',str(elf)],text=True)
def symbol(name):
    match=re.search(r'^([0-9a-fA-F]+) \w '+re.escape(name)+r'$',nm,re.M)
    if not match: raise RuntimeError('missing linked symbol: '+name)
    return int(match[1],16)
symbols=['ov_desktop_netsurf_install','ov_desktop_netsurf_stop','ov_qpk_netsurf_bind',
         'ov_http_nuttx_perform','ov_http_register','ov_image_register','png_read_image','qpk_jd_decomp',
         'FT_Init_FreeType','browser_window_create']
linked={name:hex(symbol(name)) for name in symbols}
features=json.loads((target/'features.json').read_text()) if (target/'features.json').exists() else {}
webjs=features.get('webpage_javascript',False)
if webjs:
    for name in ['ovweb_JS_NewRuntime2','JS_NewRuntime','js_exec']:
        linked[name]=hex(symbol(name))
offset=int(re.search(r'^CONFIG_ESPRESSIF_STORAGE_MTD_OFFSET=(\S+)',config,re.M)[1],0)
size=binary.stat().st_size
erase_end=(0x2000+size+4095)&~4095
ebss=symbol('_ebss'); limit=0x4ffaefc0
initialiser=subprocess.check_output([prefix+'objdump','-d','--disassemble=esp_psram_bss_init',str(elf)],text=True)
psram_initialised=bool(re.search(r'<(?:memset|__wrap_memset)>',initialiser))
passed=erase_end<=offset and ebss<=limit and psram_initialised
report={
    'firmware_linked':True,'board_tested':False,'deployable':False,
    'javascript_in_webpages':webjs,'native_quickapp_service':True,
    'linked_symbols':linked,'image_bytes':size,
    'application_offset':hex(0x2000),'data_partition_offset':hex(offset),
    'flash_remaining_bytes':offset-erase_end,'internal_static_margin_bytes':limit-ebss,
    'external_bss_bytes':symbol('_ext_ram_bss_end')-symbol('_ext_ram_bss_start'),
    'external_bss_initialiser_verified':psram_initialised,'layout_passed':passed,
    'sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
    'status':'host tests and link only; on-board acceptance pending' if passed else 'layout rejected',
}
filename='NetSurf-P4-UNTESTED.bin' if passed else 'NetSurf-P4-LAYOUT-REJECTED.bin'
shutil.copy2(binary,out/filename)
shutil.copy2(target/'libnetsurf-quickapp.a',out/'libnetsurf-quickapp.a')
(out/'verification.json').write_text(json.dumps(report,indent=2)+'\n')
(out/'psram-initialiser.txt').write_text(initialiser)
(out/'sections.txt').write_text(subprocess.check_output([prefix+'size','-A',str(elf)],text=True))
(out/'resolved.config').write_text(config)
(out/'features.json').write_text(json.dumps(features,indent=2)+'\n')
print(json.dumps(report,indent=2))
raise SystemExit(0 if passed else 1)
