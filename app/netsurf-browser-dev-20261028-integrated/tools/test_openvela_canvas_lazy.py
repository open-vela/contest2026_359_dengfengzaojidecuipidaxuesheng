#!/usr/bin/env python3
"""Test lazy canvas storage in an explicitly prepared candidate core/source."""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--core', type=Path, required=True)
p.add_argument('--source', type=Path, help='explicit edited canvas.c; defaults to prepared core')
p.add_argument('--output', type=Path, required=True)
p.add_argument('--cc', default='gcc')
p.add_argument('--sanitize', action='store_true')
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
ns = a.core.resolve() / 'deps/netsurf-modern'
source = a.source.resolve() if a.source else ns / 'netsurf/content/handlers/html/canvas.c'
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
binary = out / 'canvas_lazy'
flags = ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-ffunction-sections', '-fdata-sections', '-Dnsmonkey', '-DWITH_NS_SVG',
         '-DOV_CANVAS_SOURCE="' + str(source) + '"']
if a.sanitize:
    flags += ['-fsanitize=address,undefined', '--param=asan-globals=0',
              '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
includes = [root / 'platform/openvela', a.core.resolve() / 'port-compat',
            a.core.resolve() / 'build/gen', ns / 'netsurf', ns / 'netsurf/include',
            ns / 'netsurf/content/handlers', ns / 'libwapcaplet/include',
            ns / 'libdom/include', ns / 'libcss/include', ns / 'libparserutils/include',
            ns / 'libhubbub/include', ns / 'libnsutils/include']
for include in includes:
    flags += ['-I', str(include)]
subprocess.run([a.cc, *flags, str(root / 'tests/openvela/canvas_lazy.c'),
                str(root / 'platform/openvela/openvela_canvas_png.c'),
                '-Wl,--gc-sections', '-lm', '-lz', '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True, timeout=30)
