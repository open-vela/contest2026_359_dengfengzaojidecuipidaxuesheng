#!/usr/bin/env python3
"""Export the independent app sources and validation evidence to a NEW folder."""
import argparse
from pathlib import Path
import shutil

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--destination',type=Path,required=True)
p.add_argument('--native',action='store_true',help='export only the current native app release evidence')
a=p.parse_args()
root=Path(__file__).resolve().parents[1]
dest=a.destination.resolve()
if dest.exists(): p.error('destination exists; refusing to overwrite project changes')
dest.mkdir(parents=True)
names=['platform/openvela','platform/openvela_module','quickapp/netsurf_browser','tests/openvela']
names+=['dist/native-app','dist/native-loader-firmware','dist/validation-native'] if a.native else ['dist']
for name in names:
    shutil.copytree(root/name,dest/name,ignore=shutil.ignore_patterns('__pycache__','*.pyc'))
(dest/'tools').mkdir()
for pattern in ['*openvela*.py','package_quickapp.py']:
    for source in (root/'tools').glob(pattern): shutil.copy2(source,dest/'tools'/source.name)
shutil.copy2(root/'ESP32P4-PORT-PLAN.md',dest/'ESP32P4-PORT-PLAN.md')
print('EXPORTED',dest)
