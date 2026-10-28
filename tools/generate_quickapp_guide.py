#!/usr/bin/env python3
"""Synchronize the app designer's embedded guide without rewriting examples."""
from generate_app_ui import DESKTOP, ROOT, replace_symbol

source = (ROOT / 'app/espdl-quickapp/overlay/apps/system/espclaw/guide/QUICKAPP_GUIDE.md').read_text(encoding='utf-8')
replace_symbol(DESKTOP / 'glass_qpk_assets.inc', 'qpk_authoring_guide', source)
print('QUICKAPP_GUIDE_OK')
