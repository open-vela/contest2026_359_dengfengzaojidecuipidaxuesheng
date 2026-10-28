#!/usr/bin/env python3
"""Compile the actual build-generator trace snippet with the real talloc."""
import argparse
import ast
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--core', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--generator', type=Path, help='optional baseline build generator for negative control')
p.add_argument('--backend', type=Path, help='test an already generated backend.c instead')
p.add_argument('--sanitize', action='store_true')
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
generator = a.generator or root / 'tools/build_openvela_webjs.py'
if a.backend:
    backend = a.backend.read_text()
    start = backend.index('\t\tstatic uint64_t ov_memory_report')
    end = backend.index('\n\t\tif (active && qjs_execution_cancelled', start)
    snippets = [backend[start:end]]
else:
    tree = ast.parse(generator.read_text())
    snippets = [node.value for node in ast.walk(tree)
                if isinstance(node, ast.Constant) and isinstance(node.value, str)
                and 'static uint64_t ov_memory_report' in node.value]
if len(snippets) != 1:
    p.error('expected one production memory trace snippet')
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
snippet = out / 'trace.inc'
snippet.write_text(snippets[0])
ns = a.core.resolve() / 'deps/netsurf-modern/netsurf'
flags = ['-std=gnu11', '-O1', '-g', '-I', str(ns),
         '-DOV_TRACE_SNIPPET="' + str(snippet) + '"']
if a.sanitize:
    flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
binary = out / 'webjs_trace'
subprocess.run(['gcc', *flags, str(root / 'tests/openvela/webjs_trace.c'),
                str(ns / 'utils/talloc.c'), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True, timeout=15)
