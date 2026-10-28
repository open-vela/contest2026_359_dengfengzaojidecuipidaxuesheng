#!/usr/bin/env python3
"""Build the browser as a sideloaded P4 ET_REL module; never modify the target."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--target', type=Path, required=True)
p.add_argument('--library', type=Path, required=True)
p.add_argument('--frontend', type=Path, required=True, help='frontend output with commands.json')
p.add_argument('--toolchain', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--defer-import-check',action='store_true',help='new host configuration is not built yet; final host audit is mandatory')
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
target = a.target.resolve()
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
prefix = str(a.toolchain.resolve() / 'riscv32-esp-elf-')
cc = prefix + 'gcc'

# ABI intentionally covers configuration and the public LVGL/QuickJS layouts.
# No address is embedded: host imports resolve by name when loaded.
abi_files = [target/'nuttx/.config', target/'apps/interpreters/quickjs/quickjs/quickjs.h']
abi_files += sorted((target/'apps/graphics/lvgl/lvgl/src').rglob('*.h'))
abi_files += sorted((target/'nuttx/include').rglob('*.h'))
digest = hashlib.sha256()
for file in abi_files:
    digest.update(str(file.relative_to(target)).encode() + b'\0' + file.read_bytes())
digest.update(subprocess.check_output([cc, '--version']))
abi = digest.hexdigest()
(out/'browser_host_abi.h').write_text('#define OV_BROWSER_HOST_ABI "'+abi+'"\n')
(out/'host.abi').write_text(abi+'\n')

commands = json.loads((a.frontend/'commands.json').read_text())
base = next(c for c in commands if any(str(x).endswith('openvela_desktop.c') for x in c))
flags = base[1:base.index('-c')]
include_base = Path(flags[flags.index('-isystem')+1]).parents[1]
flags = [str(x).replace(str(include_base)+'/', str(target)+'/') for x in flags]
module_source = root/'platform/openvela_module'
obj = out/'browser_module.o'
subprocess.run([cc, *flags, '-I', str(module_source), '-I', str(out),
                '-c', str(module_source/'browser_module.c'), '-o', str(obj)], check=True)
# Combining input sections reduces section-header and loader metadata cost. The
# result remains ET_REL; relocation/symbol sections must survive packaging.
script = out/'module.ld'
script.write_text('SECTIONS {\n'
    ' .text : ALIGN(256) { *(.text .text.* .gnu.linkonce.t.*) . = ALIGN(256); }\n'
    ' .rodata : ALIGN(256) { *(.rodata .rodata.* .srodata .srodata.* .gnu.linkonce.r.*) . = ALIGN(256); }\n'
    ' .data : { *(.data .data.* .sdata .sdata.* .gnu.linkonce.d.*) }\n'
    ' .bss : { *(.bss .bss.* .sbss .sbss.* .ext_ram.bss .ext_ram.bss.* COMMON) }\n'
    ' /DISCARD/ : { *(.comment .note.GNU-stack) }\n}\n')
raw = out/'browser-unstripped.elf'
library = out/'browser-input.a'
shutil.copy2(a.library.resolve(), library)
updates = sorted(a.frontend.resolve().glob('openvela_*.o'))
subprocess.run([prefix+'ar', 'rcs', str(library), *map(str, updates)], check=True)
subprocess.run([cc, '-nostdlib', '-r', '-Wl,--gc-sections,-u,ov_browser_module_query',
    '-Wl,-e,ov_browser_module_query', '-Wl,-T,'+str(script),
    str(obj), str(library), '-o', str(raw)], check=True)
module = out/'browser.elf'
from openvela_relax_module import resolve_local
resolved = out/'browser-resolved.elf'
local_report = resolve_local(raw, resolved, prefix, out)
subprocess.run([prefix+'objcopy', '--strip-debug', '--strip-unneeded',
    '--keep-global-symbol=ov_browser_module_query', str(resolved), str(module)], check=True)
headers = subprocess.check_output([prefix+'readelf', '-h', str(module)], text=True)
if not re.search(r'Type:\s+REL\b', headers) or 'RISC-V' not in headers:
    raise RuntimeError('expected a RISC-V relocatable ELF')
sections = subprocess.check_output([prefix+'readelf', '-SW', str(module)], text=True)
if re.search(r'\.(?:preinit_array|init_array|fini_array)\b', sections):
    raise RuntimeError('module must not execute constructors before its ABI is checked')
undefined = subprocess.check_output([prefix+'nm', '-u', str(module)], text=True)
imports = sorted({m[1] for line in undefined.splitlines()
                  if (m := re.search(r'\bU\s+(\S+)$', line))})
host_elf=target/'nuttx/nuttx'
host_symbols = subprocess.check_output([prefix+'nm', '--defined-only', str(host_elf)], text=True) if host_elf.exists() else ''
if not host_elf.exists() and not a.defer_import_check:
    raise RuntimeError('host ELF does not exist; build a host first or explicitly defer import checking')
available = {line.split()[-1] for line in host_symbols.splitlines() if len(line.split()) >= 3}
missing = sorted(set(imports)-available)
if missing:
    # A new import may exist in libc/pthread but have been garbage-collected
    # from the previous image. The generated export table roots it next link.
    archives = sorted((target/'nuttx/staging').glob('*.a'))
    archive_symbols = subprocess.check_output([prefix+'nm', '--defined-only', *map(str, archives)], text=True)
    linkable = {line.split()[-1] for line in archive_symbols.splitlines() if len(line.split()) >= 3}
    unavailable = sorted(set(missing)-linkable)
    if unavailable and not a.defer_import_check:
        raise RuntimeError('host has no implementation of module imports: '+', '.join(unavailable))
exports = ['/* Generated from this module; do not edit. */', '#include <nuttx/symtab.h>']
for i, symbol in enumerate(imports):
    if not re.fullmatch(r'[A-Za-z_$][A-Za-z0-9_.$]*', symbol):
        raise RuntimeError('invalid import name: '+symbol)
    exports.append(f'extern const char ov_import_{i}[] __asm__("{symbol}");')
exports.append('const struct symtab_s ov_browser_exports[] = {')
exports += [f'  {{"{symbol}", ov_import_{i}}},' for i, symbol in enumerate(imports)]
exports += ['};', f'const int ov_browser_nexports = {len(imports)};']
(out/'browser_exports.c').write_text('\n'.join(exports)+'\n')
# Give the host builder the exact include/architecture flags, independent of
# Windows/WSL shell quoting. Both module and loader use this configuration.
(out/'host-compile.json').write_text(json.dumps([cc, *flags, '-I', str(module_source), '-I', str(out)], indent=2))
size = subprocess.check_output([prefix+'size', str(module)], text=True)
(out/'module-size.txt').write_text(size)
(out/'module-sections.txt').write_text(sections)
relocations = subprocess.check_output([prefix+'readelf', '-rW', str(module)], text=True)
(out/'module-relocations.txt').write_text(relocations)
relocation_types = sorted(set(re.findall(r'\bR_RISCV_\w+', relocations)))
relocator = (target/'nuttx/libs/libc/machine/risc-v/arch_elf.c').read_text()
supported = set(re.findall(r'case\s+(R_RISCV_\w+)\s*:', relocator)) | {'R_RISCV_RELAX', 'R_RISCV_NONE'}
if set(relocation_types)-supported:
    raise RuntimeError('unsupported target relocation types: '+str(set(relocation_types)-supported))
report = {'format':'ELF32 RISC-V ET_REL', 'host_abi':abi,
    'local_relocations':local_report,
    'module_bytes':module.stat().st_size, 'sha256':hashlib.sha256(module.read_bytes()).hexdigest(),
    'imports':imports, 'imports_to_retain_from_archives':missing,
    'relocations':relocation_types, 'entry':'ov_browser_module_query',
    'board_tested':False, 'load_tested_on_p4':False,
    'status':'cross-linked module only; requires matching native-loader firmware'}
(out/'module.json').write_text(json.dumps(report, indent=2)+'\n')
print(size, end='')
print('MODULE_LINK_OK', module, 'bytes=', report['module_bytes'], 'imports=', len(imports))
