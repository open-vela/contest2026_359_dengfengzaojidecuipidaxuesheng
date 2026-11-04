#!/usr/bin/env python3
"""Prepare locked public inputs and build the documented P4 base profile.

No serial access, flashing or model installation. --dry-run prints both commands
without downloading inputs or creating a build directory.
"""
import argparse
from pathlib import Path
import shlex
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--toolchain', type=Path, required=True, help='GCC 14.2.0 bin directory')
    parser.add_argument('--archive', type=Path, required=True, help='Pinned OpenH264 2.6.0 source archive')
    parser.add_argument('--cache', type=Path, default=Path.home()/'.cache/espclaw-p4')
    parser.add_argument('--inputs', type=Path, default=Path.home()/'espclaw-p4-inputs')
    parser.add_argument('--workspace', type=Path, default=Path.home()/'espclaw-p4-build')
    parser.add_argument('--output', type=Path, default=Path('out/p4'))
    parser.add_argument('--jobs', type=int, default=6)
    parser.add_argument('--source-revision', help='Optional explicit source commit, forwarded unchanged')
    parser.add_argument('--offline', action='store_true', help='Require all input archives in cache')
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args()
    if sys.platform == 'win32' and not args.dry_run:
        parser.error('Run actual P4 builds in Linux/WSL Bash; Windows supports --dry-run only')
    if not 1 <= args.jobs <= 64:
        parser.error('--jobs must be between 1 and 64')
    inputs, workspace, output = (p.resolve() for p in (args.inputs, args.workspace, args.output))
    if (workspace.is_relative_to(inputs) or inputs.is_relative_to(workspace)
            or output.is_relative_to(inputs) or inputs.is_relative_to(output)
            or output.is_relative_to(workspace) or workspace.is_relative_to(output)):
        parser.error('inputs, workspace and output must be separate directories without nesting')
    repo = Path(__file__).resolve().parents[1]
    prepare = [sys.executable, str(repo/'tools/prepare_p4_repro_inputs.py'),
               '--cache', str(args.cache.resolve()), '--output', str(inputs)]
    if args.offline:
        prepare.append('--offline')
    build = [sys.executable, str(repo/'tools/build_p4_repro.py'),
             '--inputs', str(inputs), '--workspace', str(workspace),
             '--toolchain', str(args.toolchain.resolve()), '--archive', str(args.archive.resolve()),
             '--output', str(output), '--jobs', str(args.jobs)]
    if args.source_revision:
        build += ['--source-revision', args.source_revision]
    for command in (prepare, build):
        print(shlex.join(command), flush=True)
        if not args.dry_run:
            subprocess.run(command, cwd=repo, check=True)


if __name__ == '__main__':
    main()
