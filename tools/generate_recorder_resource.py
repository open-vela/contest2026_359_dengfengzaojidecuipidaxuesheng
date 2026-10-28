#!/usr/bin/env python3
"""Generate the built-in recorder resource from its canonical UTF-8 script."""
from pathlib import Path
import json

root=Path(__file__).resolve().parents[1]
desktop=root/'app/espdl-quickapp/overlay/apps/system/desktop'
source=(desktop/'recorder/app.js').read_text(encoding='utf-8')
assert '\0' not in source
result='/* Generated from recorder/app.js. SPDX-License-Identifier: Apache-2.0 */\n'
result+='static const char g_recorder_app_js[]=\n'
result+='\n'.join(json.dumps(line,ensure_ascii=False) for line in source.splitlines(keepends=True))
result+='\n;\nconst char *recorder_get_app_js(unsigned int *len) { *len=sizeof(g_recorder_app_js)-1; return g_recorder_app_js; }\n'
(desktop/'recorder_resource.c').write_text(result,encoding='utf-8',newline='\n')
print('Generated recorder_resource.c',len(source.encode('utf-8')),'bytes')
