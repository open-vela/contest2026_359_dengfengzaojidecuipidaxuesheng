#!/usr/bin/env python3
"""Synchronize the reviewed HA frontend and retain exact-source authorization."""
from pathlib import Path
import hashlib,json,re,runpy,shutil
from ha_design_contract import read_design_contract

repo=Path(__file__).resolve().parents[1]
desktop=repo/'app/espdl-quickapp/overlay/apps/system/desktop'
source=desktop/'homeassistant/app.js'
catalog=json.loads((repo/'app/homeassistant/common-card-profiles.json').read_text(encoding='utf-8'))
profiles=catalog['profiles']
types=json.loads((repo/'app/homeassistant/quick-card-catalog.json').read_text(encoding='utf-8'))['types']
assert len({row['id'] for row in profiles})==len(profiles), 'Duplicate profile ID'
covered=[kind for row in profiles for kind in row['types']]
assert len(set(covered))==len(covered), 'Device type belongs to multiple profiles'
assert set(covered)==set(types), 'Every registered device type needs a profile'
for row in profiles:
    assert all(len(reading)==3 for reading in row.get('readings',[])), row['id']
text=source.read_text(encoding='utf-8')
from household_icon_design import ART, SYMBOLS, CODEPOINTS
assert set(ART)==set(types), 'Registered devices and authored artwork must match exactly'
motion={kind:dict(glyph=CODEPOINTS[SYMBOLS[kind]],states=spec['states'],description=spec['detail']) for kind,spec in ART.items()}
motion_block='/* HA_DEVICE_MOTIONS_BEGIN */\n  const deviceIconMotions = '+json.dumps(motion,ensure_ascii=True,separators=(',',':'))+';\n  /* HA_DEVICE_MOTIONS_END */'
if '/* HA_DEVICE_MOTIONS_BEGIN */' in text:
    text=re.sub(r'/\* HA_DEVICE_MOTIONS_BEGIN \*/.*?/\* HA_DEVICE_MOTIONS_END \*/',lambda _:motion_block,text,flags=re.S)
else:text=text.replace('  function deviceGlyph(item) {','  '+motion_block+'\n  function deviceGlyph(item) {',1)
contract=read_design_contract(repo/'app/homeassistant/preview/unified-device-cards.html')
block='/* HA_DETAIL_DESIGN_BEGIN */\n  const detailDesign = '+json.dumps(contract,ensure_ascii=True,separators=(',',':'))+';\n  /* HA_DETAIL_DESIGN_END */\n'
if '/* HA_DETAIL_DESIGN_BEGIN */' in text:
    text=re.sub(r'/\* HA_DETAIL_DESIGN_BEGIN \*/.*?/\* HA_DETAIL_DESIGN_END \*/\n',lambda _:block,text,flags=re.S)
else:text=text.replace('  function renderProfileDetail(', '  '+block+'  function renderProfileDetail(',1)
text,count=re.subn(r'/\* HA_COMMON_PROFILES_BEGIN \*/.*?/\* HA_COMMON_PROFILES_END \*/',
    lambda _: '/* HA_COMMON_PROFILES_BEGIN */\n  const commonCardProfiles = JSON.parse('+json.dumps(json.dumps(profiles,ensure_ascii=False,separators=(',',':')),ensure_ascii=False)+');\n  /* HA_COMMON_PROFILES_END */',text,flags=re.S)
assert count==1
source.write_text(text,encoding='utf-8',newline='\n')
data=source.read_bytes()
(repo/'quickapp/homeassistant/app.js').write_bytes(data)
runpy.run_path(str(repo/'app/homeassistant/generate_homeassistant_resource.py'),run_name='__main__')
generated=repo/'app/homeassistant/homeassistant_resource.c'
for target in [desktop/'homeassistant_resource.c',repo/'app/homeassistant_resource.c']:
    shutil.copy2(generated,target)
auth=desktop/'hass_ui_auth.h'
text=auth.read_text(encoding='utf-8')
digest=hashlib.sha256(data).digest()
text,count=re.subn(r'g_hass_ui_sha256\[32\] = \{[^}]+\}',
    'g_hass_ui_sha256[32] = {'+', '.join(f'0x{value:02x}' for value in digest)+'}',text)
assert count==1
auth.write_text(text,encoding='utf-8')
print('HA_UI_PACKAGE_OK',len(data),'bytes',digest.hex())
