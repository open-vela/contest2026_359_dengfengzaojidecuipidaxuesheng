#!/usr/bin/env python3
"""Install the browser into a NEW isolated copy of an existing P4 build tree.

Does not flash a device. Keeps the original build tree untouched. Requires a
prepared PSRAM NetSurf archive from the existing offline port and frontend
objects produced by check_openvela.py against the same configuration.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--baseline', type=Path, required=True)
p.add_argument('--target', type=Path, required=True)
p.add_argument('--core-library', type=Path, required=True)
p.add_argument('--objects', type=Path, required=True)
p.add_argument('--toolchain', type=Path, required=True)
p.add_argument('--link-command', type=Path, required=True)
p.add_argument('--build', action='store_true')
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
base, target = a.baseline.resolve(), a.target.resolve()
if target == base or target in base.parents or base in target.parents:
    p.error('target must be independent of baseline')
if target.exists():
    p.error('target already exists; choose a new isolated directory')
for name in ('nuttx/.config', 'apps/system/desktop/qpk_runtime.c'):
    if not (base / name).is_file(): p.error('missing baseline ' + name)
runtime = (base / 'apps/system/desktop/qpk_runtime.c').read_text()
anchors = ['#include "qpk_runtime.h"', '    qpk_pet_bind(context, system);', 'void qpk_runtime_stop(void)\n{']
if any(runtime.count(anchor) != 1 for anchor in anchors):
    p.error('unsupported runtime revision: hook anchors must each occur once')
if 'ov_desktop_netsurf' in runtime:
    p.error('baseline already contains NetSurf hooks')
command = json.loads(a.link_command.read_text())
if not isinstance(command, list) or command[0] != 'make' or not any(x.startswith('EXTRA_LIBS=') for x in command):
    p.error('expected saved make command with EXTRA_LIBS')
objects = sorted(a.objects.resolve().glob('openvela_*.o'))
expected = {src.stem for src in (root / 'platform/openvela').glob('*.c') if not src.stem.endswith('_test')}
if {obj.stem for obj in objects} != expected: p.error('frontend objects are incomplete')
target.mkdir(parents=True)
for name in ('apps', 'nuttx'):
    subprocess.run(['cp', '-a', '--reflink=auto', str(base / name), str(target / name)], check=True)
# Rebase absolute symlinks and generated Kconfig paths in the copied tree.
for parent, dirs, files in os.walk(target, followlinks=False):
    for name in dirs + files:
        path = Path(parent) / name
        if path.is_symlink():
            old = os.readlink(path)
            if old.startswith(str(base) + '/'):
                path.unlink()
                path.symlink_to(str(target) + old[len(str(base)):])
for path in (target / 'apps').rglob('Kconfig'):
    if not path.is_symlink():
        before = path.read_text()
        after = before.replace(str(base) + '/', str(target) + '/')
        if before != after: path.write_text(after)
desktop = target / 'apps/system/desktop'
shutil.copytree(root / 'platform/openvela', desktop / 'netsurf')
shutil.copytree(root / 'quickapp/netsurf_browser', target / 'quickapp/netsurf_browser')
runtime = runtime.replace(anchors[0], anchors[0] + '\n#include "netsurf/openvela_desktop.h"')
runtime = runtime.replace(anchors[1],
    '    (void)ov_desktop_netsurf_install(context, system, g_qpk.root, g_qpk.package);\n' + anchors[1])
runtime = runtime.replace(anchors[2], anchors[2] + '\n  ov_desktop_netsurf_stop();')
(desktop / 'qpk_runtime.c').write_text(runtime)
# The existing NSH renderer is superseded; avoid retaining a second frontend
# and its process-global NetSurf tables in the same image.
main = target / 'apps/graphics/netsurf_port/netsurf_main.c'
if not main.exists(): p.error('expected offline-port NSH entry is missing')
main.write_text('#include <stdio.h>\nint netsurf_main(int argc, char **argv)\n'
                '{ (void)argc; (void)argv; puts("Open the NetSurf Quick App from the desktop"); return 0; }\n')
prefix = str(a.toolchain.resolve() / 'riscv32-esp-elf-')
libdir = target / 'netsurf-library'
libdir.mkdir()
subprocess.run([prefix + 'ar', 'x', str(a.core_library.resolve())], cwd=libdir, check=True)
archive_objects = [obj for obj in libdir.glob('*.o') if not any(name in obj.name for name in
    ('port_nsp_main.', 'port_nsp_font.', 'port_nsw_plot.', 'port_nsw_bitmap.', 'port_nsw_schedule.'))]
for obj in objects + sorted(a.objects.resolve().glob('libpng_*.o')):
    sections = subprocess.check_output([prefix + 'objdump', '-h', str(obj)], text=True)
    rename = []
    for line in sections.splitlines():
        match = re.match(r'\s*\d+\s+(\S+)\s+[0-9a-fA-F]+\s', line)
        if match and re.match(r'^\.(?:bss|sbss)(?:\.|$)', match[1]):
            rename += ['--rename-section', match[1] + '=.ext_ram.bss.' + match[1][1:]]
    dest = libdir / obj.name
    subprocess.run([prefix + 'objcopy', *rename, str(obj), str(dest)], check=True)
    archive_objects.append(dest)
rsp = libdir / 'objects.rsp'
rsp.write_text('\n'.join(str(obj) for obj in archive_objects))
archive = target / 'libnetsurf-quickapp.a'
subprocess.run([prefix + 'ar', 'rcs', str(archive), '@' + str(rsp)], check=True)
command = [arg.replace(str(base), str(target)) for arg in command]
command = [arg.replace('EXTRA_LIBS=', 'EXTRA_LIBS=' + str(archive) + ' ', 1)
           if arg.startswith('EXTRA_LIBS=') else arg for arg in command]
(target / 'build-command.json').write_text(json.dumps(command, indent=2))
print('ISOLATED_TREE_READY', target, flush=True)
if a.build:
    env = dict(os.environ)
    env['PATH'] = str(a.toolchain.resolve()) + ':' + str(base.parent / '.venv/bin') + ':' + env['PATH']
    # Full clean ensures copied archives and HAL objects cannot mask the new
    # frontend or stale external-BSS initialization.
    subprocess.run(command + ['clean'], cwd=target / 'nuttx', env=env, check=True)
    subprocess.run(command, cwd=target / 'nuttx', env=env, check=True)
    print('FIRMWARE_LINK_OK', target / 'nuttx/nuttx.bin', flush=True)
