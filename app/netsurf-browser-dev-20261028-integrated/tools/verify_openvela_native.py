#!/usr/bin/env python3
"""Audit a native-loader image and its separately installed browser module."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--target', type=Path, required=True)
p.add_argument('--module', type=Path, required=True)
p.add_argument('--toolchain', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
target, module, out = a.target.resolve(), a.module.resolve(), a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
prefix = str(a.toolchain.resolve()/'riscv32-esp-elf-')
elf = target/'nuttx/nuttx'
binary = target/'nuttx/nuttx.bin'
nm = subprocess.check_output([prefix+'nm', '--defined-only', str(elf)], text=True)
symbols = {m[3]:int(m[1],16) for line in nm.splitlines()
           if (m := re.match(r'([0-9a-fA-F]+)\s+(\S)\s+(\S+)$',line))}
metadata = json.loads((module/'module.json').read_text())
if hashlib.sha256((module/'browser.elf').read_bytes()).hexdigest() != metadata['sha256']:
    raise RuntimeError('module digest mismatch')
missing = sorted(set(metadata['imports'])-symbols.keys())
if missing: raise RuntimeError('final host is missing imports: '+str(missing))
for name in ['ov_browser_native_install', 'ov_browser_native_stop', 'ov_browser_exports',
             'ov_browser_cache_prepare', 'esp_cache_msync', 'esp_cache_get_line_size_by_addr', 'insmod', 'modsym']:
    if name not in symbols: raise RuntimeError('missing host loader symbol '+name)
for name in ['ov_desktop_netsurf_install', 'browser_window_create', 'ovweb_JS_NewRuntime2']:
    if name in symbols: raise RuntimeError('browser engine is still embedded: '+name)
abi = (target/'apps/system/desktop/browser/browser_host_abi.h').read_text()
if '"'+metadata['host_abi']+'"' not in abi: raise RuntimeError('host/module ABI mismatch')
config = (target/'nuttx/.config').read_text()
for name in ['MODULE','LIBC_ELF','LIBC_ARCH_ELF']:
    if 'CONFIG_'+name+'=y' not in config: raise RuntimeError('missing loader configuration '+name)
offset = int(re.search(r'^CONFIG_ESPRESSIF_STORAGE_MTD_OFFSET=(\S+)',config,re.M)[1],0)
size = binary.stat().st_size
end = (0x2000+size+4095)&~4095
margin = 0x4ffaefc0-symbols['_ebss']
initialiser = subprocess.check_output([prefix+'objdump', '-d', '--disassemble=esp_psram_bss_init', str(elf)],text=True)
psram = bool(re.search(r'<(?:memset|__wrap_memset)>',initialiser))
passed = end <= offset and margin >= 0 and psram
pool=None
if 'CONFIG_OV_BROWSER_TEXT_POOL=y' in config:
    start,end_pool=symbols['_browser_exec_start'],symbols['_browser_exec_end']
    pool={'start':hex(start),'end':hex(end_pool),'bytes':end_pool-start,
          'inside_reserved_bss':symbols['_ext_ram_bss_start']<=start<end_pool<=symbols['_ext_ram_bss_end']}
    passed &= pool['inside_reserved_bss'] and pool['bytes']==0x300000 and start%65536==0
    if 'up_textheap_memalign' not in symbols:raise RuntimeError('text pool allocator is not linked')
report = {'layout_passed':passed, 'firmware_linked':True, 'browser_engine_embedded':False,
    'explicit_module_cache_sync_linked':True,
    'reserved_text_pool':pool,
    'module_imports_resolved':len(metadata['imports']), 'host_abi':metadata['host_abi'],
    'image_bytes':size, 'flash_remaining_bytes':offset-end, 'application_offset':'0x2000',
    'data_partition_offset':hex(offset), 'internal_static_margin_bytes':margin,
    'external_bss_initialiser_verified':psram, 'sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
    'module_bytes':metadata['module_bytes'], 'module_sha256':metadata['sha256'],
    'board_tested':False, 'module_executed_on_p4':False, 'deployable':False,
    'status':'cross-linked host and module; P4 load/execute/display/network acceptance pending'}
(out/'verification.json').write_text(json.dumps(report,indent=2)+'\n')
(out/'sections.txt').write_text(subprocess.check_output([prefix+'size','-A',str(elf)],text=True))
(out/'resolved.config').write_text(config)
shutil.copy2(binary,out/'Vela-NativeLoader-P4-UNTESTED.bin')
print(json.dumps(report,indent=2))
raise SystemExit(0 if passed else 1)
