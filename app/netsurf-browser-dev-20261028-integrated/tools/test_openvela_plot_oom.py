#!/usr/bin/env python3
"""Exercise production bitmap resampling with allocation-failure injection."""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--core', type=Path, help='prepared core root; defaults to this workspace')
p.add_argument('--output', type=Path, required=True)
p.add_argument('--cc', default='gcc')
p.add_argument('--sanitize', action='store_true', help='enable address and undefined behavior sanitizers')
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
core = a.core.resolve() if a.core else root
ns = core / 'deps/netsurf-modern'
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
binary = out / 'plot_oom'
flags = ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-ffunction-sections', '-fdata-sections', '-Dnsmonkey', '-DWITH_NS_SVG']
if a.sanitize:
    # Keep unrelated plotter-table entries discardable while instrumenting all
    # heap accesses in the production resampler and the allocation tracker.
    flags += ['-fsanitize=address,undefined', '--param=asan-globals=0',
              '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
includes = [root / 'src/compat', root / 'platform/openvela',
            ns / 'netsurf', ns / 'netsurf/include', ns / 'netsurf/frontends',
            ns / 'libwapcaplet/include', ns / 'libdom/include']
for include in includes:
    flags += ['-I', str(include)]
subprocess.run([a.cc, *flags, str(root / 'tests/openvela/plot_oom.c'),
                '-Wl,--gc-sections', '-lm', '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True, timeout=30)
