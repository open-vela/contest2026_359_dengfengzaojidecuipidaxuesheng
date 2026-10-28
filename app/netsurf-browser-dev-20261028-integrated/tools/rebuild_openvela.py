#!/usr/bin/env python3
"""Refresh frontend objects in an existing isolated Quick App candidate."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--target',type=Path,required=True)
p.add_argument('--objects',type=Path,required=True)
p.add_argument('--toolchain',type=Path,required=True)
p.add_argument('--webjs',type=Path,help='namespaced webpage VM objects')
p.add_argument('--core-objects',type=Path,help='replacement core obj/ root, built with the same ABI')
a=p.parse_args()
root=Path(__file__).resolve().parents[1]
target=a.target.resolve()
command_path=target/'build-command.json'
archive=target/'libnetsurf-quickapp.a'
if not command_path.is_file() or not archive.is_file(): p.error('not an isolated NetSurf Quick App candidate')
command=json.loads(command_path.read_text())
if command[0]!='make' or str(archive) not in ' '.join(command): p.error('unexpected build command')
prefix=str(a.toolchain.resolve()/'riscv32-esp-elf-')
objects=sorted(a.objects.resolve().glob('openvela_*.o'))
expected={s.stem for s in (root/'platform/openvela').glob('*.c') if not s.stem.endswith('_test')}
if {o.stem for o in objects}!=expected: p.error('incomplete frontend object set')
if a.core_objects:
    members=subprocess.check_output([prefix+'ar','t',str(archive)],text=True).splitlines()
    kept=[m for m in members if m.startswith('openvela_') or m.startswith('libpng_') or m.startswith('webjs_') or 'setjmp' in m or 'longjmp' in m]
    remove=[m for m in members if m not in kept]
    if remove: subprocess.run([prefix+'ar','d',str(archive),*remove],check=True)
    objects += [o for o in sorted(a.core_objects.rglob('*.o')) if not any(name in o.name for name in
                ['port_nsp_main.','port_nsp_font.','port_nsw_plot.','port_nsw_bitmap.','port_nsw_schedule.'])]
if a.webjs:
    compile_commands=json.loads((a.objects/'commands.json').read_text())
    if not any('-DOV_ENABLE_WEB_JS=1' in cmd and any(str(arg).endswith('openvela_resources.c') for arg in cmd) for cmd in compile_commands):
        p.error('frontend must be compiled with --webjs')
    members=subprocess.check_output([prefix+'ar','t',str(archive)],text=True).splitlines()
    remove=[m for m in members if 'javascript_none_none.' in m or 'nsp_js_none.' in m]
    if remove: subprocess.run([prefix+'ar','d',str(archive),*remove],check=True)
    objects=[o for o in objects if 'javascript_none_none.' not in o.name and 'nsp_js_none.' not in o.name]
    objects += [Path(name) for name in json.loads((a.webjs/'manifest.json').read_text())['objects']]
updated=[]
for obj in objects + sorted(a.objects.resolve().glob('libpng_*.o')):
    sections=subprocess.check_output([prefix+'objdump','-h',str(obj)],text=True)
    rename=[]
    for line in sections.splitlines():
        match=re.match(r'\s*\d+\s+(\S+)\s+[0-9a-fA-F]+\s',line)
        if match and re.match(r'^\.(?:bss|sbss)(?:\.|$)',match[1]):
            rename+=['--rename-section',match[1]+'=.ext_ram.bss.'+match[1][1:]]
    dest=target/'netsurf-library'/obj.name
    subprocess.run([prefix+'objcopy',*rename,str(obj),str(dest)],check=True)
    updated.append(str(dest))
response=target/'netsurf-library/refresh.rsp'
response.write_text('\n'.join(updated))
subprocess.run([prefix+'ar','rcs',str(archive),'@'+str(response)],check=True)
shutil.copytree(root/'platform/openvela',target/'apps/system/desktop/netsurf',dirs_exist_ok=True)
shutil.copytree(root/'quickapp/netsurf_browser',target/'quickapp/netsurf_browser',dirs_exist_ok=True)
env=dict(os.environ)
env['PATH']=str(a.toolchain.resolve())+':'+str(target.parent/'.venv/bin')+':'+env['PATH']
subprocess.run(command,cwd=target/'nuttx',env=env,check=True)
(target/'features.json').write_text(json.dumps({'webpage_javascript':bool(a.webjs),
    'engine_namespace':'ovweb_' if a.webjs else None,
    'webpage_vm':json.loads((a.webjs/'manifest.json').read_text()) if a.webjs else None},indent=2)+'\n')
print('FIRMWARE_LINK_OK',target/'nuttx/nuttx.bin')
