#!/usr/bin/env python3
"""Embed the shared, reviewable hardware API reference for the local MCP provider."""
from pathlib import Path
import json
import re

root = Path(__file__).resolve().parents[1] / 'app/espdl-quickapp/overlay/apps/system'
sections = re.split(r'(?m)(?=^## \d+\.)', (root / 'espclaw/guide/HARDWARE_API.md').read_text(encoding='utf-8'))[1:9]
assert len(sections) == 8
scope = ('Completed hardware state may persist across AI requests. Poll until done and ok; '
         'close explicitly or hardware_release. Cancellation or unfinished jobs release the controller.')
values = [json.dumps({'reference': section, 'scope': scope}, ensure_ascii=False, separators=(',', ':')) for section in sections]
text = '/* Generated from espclaw/guide/HARDWARE_API.md; SPDX-License-Identifier: Apache-2.0 */\nstatic const char *const board_hardware_reference[]={\n'
text += ',\n'.join(json.dumps(value, ensure_ascii=False) for value in values) + '\n};\n'
(root / 'desktop/glass_board_reference.h').write_bytes(text.encode('utf-8'))
