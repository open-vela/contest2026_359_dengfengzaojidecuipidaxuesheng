#!/usr/bin/env python3
"""Check live libdom title semantics and count large-document comparisons."""
import argparse
import os
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--core', type=Path, required=True)
p.add_argument('--objects', type=Path, required=True)
p.add_argument('--replacement', type=Path)
p.add_argument('--expect-full-scan', action='store_true')
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
ns = a.core/'deps/netsurf-modern'
obj = out/'document_title.o'
subprocess.run(['gcc', '-std=gnu11', '-O1', '-g', '-ffunction-sections', '-fdata-sections',
    '-I'+str(ns/'libdom/include'), '-I'+str(ns/'libwapcaplet/include'),
    '-c', str(root/'tests/openvela/document_title.c'), '-o', str(obj)], check=True)
objects = [obj] + [o for part in ['dom', 'wc', 'pu', 'hb'] for o in (a.objects/part).glob('*.o')]
if a.replacement:
    if sum(o.name == a.replacement.name for o in objects) != 1:
        raise RuntimeError('replacement must match exactly one original libdom object')
    objects = [a.replacement if o.name == a.replacement.name else o for o in objects]
rsp = out/'objects.rsp'
rsp.write_text('\n'.join(str(o.resolve()) for o in objects))
exe = out/'document-title'
subprocess.run(['gcc', '@'+str(rsp), '-o', str(exe), '-Wl,--gc-sections',
    '-Wl,--wrap=dom_string_caseless_isequal', '-lm'], check=True)
env = dict(os.environ)
if a.expect_full_scan: env['TITLE_EXPECT_FULL_SCAN'] = '1'
result = subprocess.run([str(exe)], capture_output=True, text=True, env=env, timeout=30)
(out/'verification.log').write_text(result.stdout + result.stderr)
print(result.stdout + result.stderr, end='')
result.check_returncode()
