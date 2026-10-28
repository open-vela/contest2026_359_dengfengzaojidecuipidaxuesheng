#!/usr/bin/env python3
"""Prepare/build a NEW native-loader candidate without an embedded browser core."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--baseline', type=Path, required=True, help='existing isolated Quick App candidate')
p.add_argument('--target', type=Path, required=True)
p.add_argument('--frontend', type=Path, required=True)
p.add_argument('--module-output', type=Path, required=True)
p.add_argument('--toolchain', type=Path, required=True)
p.add_argument('--build', action='store_true')
p.add_argument('--core-library',type=Path,help='browser archive when baseline does not embed NetSurf')
p.add_argument('--link-command',type=Path,help='saved make command(s) for the prepared baseline')
p.add_argument('--text-pool',action='store_true',help='reserve a native text pool in the existing prepared XIP profile')
p.add_argument('--core',type=Path,help='recompile frontend against this candidate using the prepared cross core')
p.add_argument('--incremental',action='store_true',help='reuse this candidate\'s existing objects; make still tracks dependencies')
p.add_argument('--resume', action='store_true', help='resume this tool\'s partially prepared candidate')
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
base, target = a.baseline.resolve(), a.target.resolve()
if (target.exists() and not a.resume) or base == target or base in target.parents or target in base.parents:
    p.error('target must be a NEW independent directory')
if a.resume and (not (target/'apps/system/desktop/qpk_runtime.c').is_file() or
    'ov_browser_native_install' not in (target/'apps/system/desktop/qpk_runtime.c').read_text()):
    p.error('resume target is not a native-loader candidate')
runtime = (base/'apps/system/desktop/qpk_runtime.c').read_text()
embedded='ov_desktop_netsurf_install' in runtime
anchors = ['#include "netsurf/openvela_desktop.h"' if embedded else '#include "qpk_runtime.h"',
    '(void)ov_desktop_netsurf_install(context, system, g_qpk.root, g_qpk.package);',
    'ov_desktop_netsurf_stop();',
    '  strlcpy(g_qpk.package, package ? package : "", sizeof(g_qpk.package));']
if not embedded:
    anchors[1]='    qpk_pet_bind(context, system);'
    anchors[2]='void qpk_runtime_stop(void)\n{'
if any(runtime.count(x) != 1 for x in anchors): p.error('unsupported baseline hooks')
command = json.loads((a.link_command or base/'build-command.json').read_text())
if command and isinstance(command[0],list):
    command=next(c for c in command if any(str(x).startswith('EXTRA_LIBS=') for x in c))
original_library = a.core_library.resolve() if a.core_library else base/'libnetsurf-quickapp.a'
if embedded and str(original_library) not in ' '.join(command): p.error('unexpected baseline library')
if not a.resume:
    target.mkdir(parents=True)
    for name in ['apps', 'nuttx']:
        subprocess.run(['cp', '-a', '--reflink=auto', str(base/name), str(target/name)], check=True)
    for parent, dirs, files in os.walk(target, followlinks=False):
        for name in dirs+files:
            path = Path(parent)/name
            if path.is_symlink():
                before = os.readlink(path)
                if before.startswith(str(base)+'/'):
                    path.unlink(); path.symlink_to(str(target)+before[len(str(base)):])
    for path in (target/'apps').rglob('Kconfig'):
        if not path.is_symlink():
            before = path.read_text(); after = before.replace(str(base)+'/', str(target)+'/')
            if before != after: path.write_text(after)
desktop = target/'apps/system/desktop'
runtime = runtime.replace(anchors[0], ('' if embedded else anchors[0]+'\n')+'#include "browser/browser_native_host.h"\nstatic const char *ov_browser_entry;')
runtime = runtime.replace(anchors[1], '(void)ov_browser_native_install(context, system, g_qpk.root, g_qpk.package, ov_browser_entry);'+('' if embedded else '\n'+anchors[1]))
runtime = runtime.replace(anchors[2], 'ov_browser_native_stop();' if embedded else anchors[2]+'\n  ov_browser_native_stop();')
runtime = runtime.replace(anchors[3], anchors[3]+'\n  ov_browser_entry = filename; /* Consumed synchronously by register_globals. */')
(desktop/'qpk_runtime.c').write_text(runtime)
shutil.copytree(root/'platform/openvela_module', desktop/'browser', dirs_exist_ok=True)
shutil.copytree(root/'quickapp/netsurf_browser', target/'quickapp/netsurf_browser', dirs_exist_ok=True)
env = dict(os.environ)
env['PATH'] = str(a.toolchain.resolve())+':'+str(base.parent/'.venv/bin')+':'+str(Path.home()/'.local/bin')+':'+env['PATH']
if a.text_pool:
    from prepare_openvela_exec_pool import apply
    apply(target/'nuttx')
subprocess.run(['kconfig-tweak', '--file', str(target/'nuttx/.config'),
    '--enable', 'MODULE', '--enable', 'MODULES', '--enable', 'LIBC_ELF',
    '--enable','LIBC_LOCALE','--enable','LIBC_LOCALE_CHINESE',
    *(['--enable','OV_BROWSER_TEXT_POOL','--enable','ESPRESSIF_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY'] if a.text_pool else [])], env=env, check=True)
new_command = [arg.replace(str(base), str(target)) for arg in command]
new_command = [arg.replace(str(target/'libnetsurf-quickapp.a'), str(target/'libbrowser-loader.a')) if embedded else
    arg.replace('EXTRA_LIBS=', 'EXTRA_LIBS='+str(target/'libbrowser-loader.a')+' ',1) if arg.startswith('EXTRA_LIBS=') else arg for arg in new_command]
subprocess.run(new_command+['olddefconfig'], cwd=target/'nuttx', env=env, check=True)
if a.text_pool:
    resolved=(target/'nuttx/.config').read_text()
    for name in ['OV_BROWSER_TEXT_POOL','ARCH_USE_TEXT_HEAP','ESPRESSIF_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY']:
        if 'CONFIG_'+name+'=y' not in resolved:
            raise RuntimeError('required native execution configuration was dropped: '+name)
subprocess.run(new_command+['context'], cwd=target/'nuttx', env=env, check=True)
if a.core:
    subprocess.run(['python3',str(root/'tools/check_openvela.py'),'--core',str(a.core.resolve()),
        '--target',str(target),'--cc',str(a.toolchain.resolve()/'riscv32-esp-elf-gcc'),
        '--webjs','--output',str(a.frontend.resolve())],env=env,check=True)
subprocess.run(['python3', str(root/'tools/build_openvela_module.py'), '--target', str(target),
    '--library', str(original_library), '--frontend', str(a.frontend.resolve()),
    '--toolchain', str(a.toolchain.resolve()), '--output', str(a.module_output.resolve()),
    '--defer-import-check'], env=env, check=True)
module_out = a.module_output.resolve()
compile_command = json.loads((module_out/'host-compile.json').read_text())
sdk=target/'nuttx/arch/risc-v/src/esp32p4/esp-hal-3rdparty/components'
for directory in ['esp_mm/include','esp_common/include','esp_hw_support/include']:
    compile_command += ['-I',str(sdk/directory)]
compile_command += ['-I',str(sdk.parent/'nuttx/esp32p4/include')]
objects = []
sources=[desktop/'browser/browser_native_host.c', desktop/'browser/browser_native_nuttx.c',
               desktop/'browser/browser_async.c',
               desktop/'browser/browser_cache.c', module_out/'browser_exports.c']
if a.text_pool:sources.append(desktop/'browser/browser_text_heap.c')
for source in sources:
    obj = target/(source.stem+'.o')
    subprocess.run([*compile_command, '-c', str(source), '-o', str(obj)], env=env, check=True)
    objects.append(str(obj))
subprocess.run([str(a.toolchain.resolve()/'riscv32-esp-elf-ar'), 'rcs',
    str(target/'libbrowser-loader.a'), *objects], check=True)
shutil.copy2(module_out/'browser_host_abi.h', desktop/'browser/browser_host_abi.h')
(target/'build-command.json').write_text(json.dumps(new_command, indent=2))
(target/'native-candidate.json').write_text(json.dumps({'baseline':str(base),
    'module_output':str(module_out), 'embedded_browser':False, 'board_tested':False}, indent=2))
print('NATIVE_CANDIDATE_READY', target, flush=True)
if a.build:
    if not a.incremental:subprocess.run(new_command+['clean'], cwd=target/'nuttx', env=env, check=True)
    subprocess.run(new_command, cwd=target/'nuttx', env=env, check=True)
    print('NATIVE_LOADER_FIRMWARE_LINK_OK', target/'nuttx/nuttx.bin', flush=True)
