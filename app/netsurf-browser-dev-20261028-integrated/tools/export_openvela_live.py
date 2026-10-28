#!/usr/bin/env python3
"""Export current browser artifacts/evidence, never private board backups."""
import argparse,shutil,re
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--destination',type=Path,required=True)
p.add_argument('--feature',help='also export a separately named feature candidate')
a=p.parse_args();root=Path(__file__).resolve().parents[1];dest=a.destination.resolve()
if not dest.name.startswith('netsurf-browser-dev-') or not (dest/'platform/openvela_module/README.md').is_file():
    p.error('destination is not the existing browser development export')
if a.feature and not re.fullmatch(r'[a-z][a-z0-9]*(?:-[a-z0-9]+)*',a.feature):p.error('invalid feature name')
for name in ['live-native-app','live-native-firmware']:
    shutil.copytree(root/'dist'/name,dest/'dist'/name,dirs_exist_ok=True)
out=dest/'validation-live';out.mkdir(exist_ok=True)
for name in ['board-final-https.log','board-final-onclick.log','board-release-final-state.log',
             'board-async-open-close.log','boot-final-trust-filter.log','patch-reuse-audit.json',
             'module-async-test.log','exec-pool-test.log','trust-filter-test.log',
             'transport-tls-test.log','board-first-page.png','board-normalized-load.log',
             'board-touch-link-current.log','flash-trust-filter.log']:
    shutil.copy2(root/'build'/name,out/name)
if a.feature:
    source=root/'dist/candidates'/a.feature
    if not (source/'candidate.json').is_file() or (source/'build-in-progress.json').exists():
        p.error('feature candidate is not complete')
    shutil.copytree(source,dest/'dist/candidates'/a.feature,dirs_exist_ok=True)
print('LIVE_BROWSER_EXPORTED',dest)
