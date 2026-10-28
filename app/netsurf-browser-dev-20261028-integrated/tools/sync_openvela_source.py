#!/usr/bin/env python3
"""Refresh only this browser's sources in its explicitly created dev project."""
import argparse
from pathlib import Path
import shutil
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--destination',type=Path,required=True)
a=p.parse_args();root=Path(__file__).resolve().parents[1];dest=a.destination.resolve()
if not dest.name.startswith('netsurf-browser-dev-') or not (dest/'platform/openvela_module/README.md').is_file():
    p.error('destination is not a browser development export')
for name in ['platform/openvela','platform/openvela_module','quickapp/netsurf_browser','tests/openvela','patches']:
    shutil.copytree(root/name,dest/name,dirs_exist_ok=True,ignore=shutil.ignore_patterns('__pycache__','*.pyc'))
for pattern in ['*openvela*.py','package_quickapp.py','setup_deps.py']:
    for source in (root/'tools').glob(pattern):shutil.copy2(source,dest/'tools'/source.name)
for name in ['ESP32P4-PORT-PLAN.md','WORK_AREAS.md','AGENTS.md']:
    shutil.copy2(root/name,dest/name)
print('SOURCE_SYNCED',dest)
