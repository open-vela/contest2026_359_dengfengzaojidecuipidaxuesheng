#!/usr/bin/env python3
"""Copy a prepared core into an isolated workarea and apply explicit new patches.

Symlinks in the source dependencies are dereferenced: applying a patch can never
write through to the verified preparation tree. Existing outputs are refused.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--patch', type=Path, action='append', default=[])
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    if output.exists() or output.is_relative_to(source) or source.is_relative_to(output):
        parser.error('output must be a new directory outside the prepared input')
    if 'releases' in output.parts:
        parser.error('release snapshots are read-only')
    inputs = ['deps', 'port', 'port-compat', 'build/gen', 'Makefile.port', 'target-flags.mk']
    for name in inputs:
        if not (source / name).exists():
            parser.error('missing prepared input: ' + name)
    output.mkdir(parents=True)
    hashes = {}
    for name in inputs:
        src, dst = source / name, output / name
        if src.is_dir():
            shutil.copytree(src, dst, symlinks=False,
                            ignore=shutil.ignore_patterns('downloads', '__pycache__', '.git'))
            for path in dst.rglob('*'):
                if path.is_file():
                    relative = path.relative_to(output)
                    hashes[str(relative)] = digest(path)
                    if digest(source / relative) != hashes[str(relative)]:
                        raise RuntimeError('input changed while copying: ' + str(relative))
        else:
            shutil.copy2(src, dst)
            hashes[name] = digest(dst)
            if digest(src) != hashes[name]:
                raise RuntimeError('input changed while copying: ' + name)
    patches = []
    for patch in args.patch:
        patch = patch.resolve()
        command = ['git', 'apply', '--whitespace=nowarn', '--directory=deps/netsurf-modern', str(patch)]
        subprocess.run(command[:2] + ['--check'] + command[2:], cwd=output, check=True)
        subprocess.run(command, cwd=output, check=True)
        patches.append({'path': str(patch), 'sha256': digest(patch)})
    (output / 'candidate-core.json').write_text(json.dumps({
        'source': str(source), 'output': str(output), 'patches': patches,
        'baseline_source_sha256': hashes,
        'scope': 'isolated sources; no board acceptance implied',
    }, indent=2) + '\n')
    print('PREPARED_CORE_CANDIDATE', output)


if __name__ == '__main__':
    main()
