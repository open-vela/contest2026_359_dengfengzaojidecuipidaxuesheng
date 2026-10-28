#!/usr/bin/env python3
"""Embed the committed, unhinted DejaVu ASCII subsets (no build dependency).

Subsets cover U+0020..007E, with GSUB/GPOS/kern removed for the target
TinyTTF reader. Chinese uses the existing desktop font fallback. Font license
and the exact subset binaries live beside the generated source in fonts/.
"""
from pathlib import Path

root = Path(__file__).resolve().parents[1] / 'app/espdl-quickapp/overlay/apps/system/desktop'
chunks = ['/* Generated DejaVu ASCII subsets. See fonts/LICENSE-DejaVu.txt. */', '#include <stddef.h>']
for name in ('glass_code', 'glass_bold'):
    data = (root / 'fonts' / (name + '-ascii.ttf')).read_bytes()
    rows = ['  ' + ','.join(f'0x{value:02x}' for value in data[i:i+16]) + ',' for i in range(0, len(data), 16)]
    chunks.append('const unsigned char ' + name + '_font_data[] = {\n' + '\n'.join(rows) + '\n};')
    chunks.append('const size_t ' + name + '_font_size = sizeof(' + name + '_font_data);')
chunks.append('#include "glass_heading.inc"')
(root / 'glass_code_font.c').write_text('\n'.join(chunks) + '\n')
