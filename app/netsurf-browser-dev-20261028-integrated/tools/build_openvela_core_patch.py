#!/usr/bin/env python3
"""Recompile explicit C-only patches from an isolated prepared core.

Unchanged objects retain their verified baseline. Header changes deliberately
require a complete core rebuild instead of this incremental route.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--core', type=Path, required=True)
    p.add_argument('--patch', type=Path, action='append', required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--target', type=Path)
    p.add_argument('--toolchain', type=Path)
    p.add_argument('--library', type=Path, help='copy this base archive and replace compiled members')
    a = p.parse_args()
    core, out = a.core.resolve(), a.output.resolve()
    if not (core / 'candidate-core.json').is_file():
        p.error('use an isolated core created by prepare_openvela_core_candidate.py')
    if bool(a.target) != bool(a.toolchain):
        p.error('cross compilation requires both target and toolchain')
    paths = set()
    for patch in a.patch:
        paths.update(re.findall(r'^\+\+\+ b/([^\r\n]+)', patch.read_text(), re.M))
    components = {'netsurf': 'ns', 'libdom': 'dom'}
    if not paths or any(Path(f).parts[0] not in components or
                        '..' in Path(f).parts or not f.endswith('.c') for f in paths):
        p.error('only NetSurf/libdom C changes support incremental object replacement')
    sources = [core / 'deps/netsurf-modern' / name for name in sorted(paths)]
    digest = lambda f: hashlib.sha256(f.read_bytes()).hexdigest()
    before = {str(f): digest(f) for f in sources + a.patch}
    out.mkdir(parents=True, exist_ok=True)
    makefile = out / 'Makefile.port'
    makefile.write_text((core / 'Makefile.port').read_text().replace('BASE := -O2', 'BASE := -Os'))
    objects = [out / 'obj' / components[Path(name).parts[0]] /
               ('netsurf-modern_' + name.replace('/', '_') + '.o')
               for name in sorted(paths)]
    prefix = str(a.toolchain.resolve() / 'riscv32-esp-elf-') if a.toolchain else ''
    command = ['make', '-C', str(core), '-f', 'target-flags.mk', '-f', str(makefile),
               *map(str, objects), 'BUILD=' + str(out), 'CC=' + prefix + 'gcc', 'AR=' + prefix + 'ar', '-j4']
    if a.target:
        command += ['TARGET_FLAGS=-march=rv32imafc_zicsr_zifencei -mabi=ilp32f -isystem ' +
                    str(a.target.resolve() / 'nuttx/include') + ' -include nuttx/config.h -D__NuttX__']
    subprocess.run(command, check=True)
    if before != {str(f): digest(f) for f in sources + a.patch}:
        raise RuntimeError('source changed during compilation')
    report = {'command': command, 'source_sha256': before,
              'objects': {str(f): digest(f) for f in objects}}
    if a.library:
        library_hash = digest(a.library)
        archive = out / 'libnetsurf-patched.a'
        shutil.copy2(a.library, archive)
        members = subprocess.check_output([prefix + 'ar', 't', str(archive)], text=True).splitlines()
        if any(f.name not in members for f in objects):
            raise RuntimeError('base archive lacks a patched object')
        subprocess.run([prefix + 'ar', 'rcs', str(archive), *map(str, objects)], check=True)
        if digest(a.library) != library_hash:
            raise RuntimeError('base archive changed during compilation')
        report.update(base_archive=str(a.library), base_archive_sha256=library_hash,
                      archive=str(archive), archive_sha256=digest(archive))
    (out / 'patched-core.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PATCHED_CORE_BUILT', out)


if __name__ == '__main__':
    main()
