#!/usr/bin/env python3
"""Create bounded feature outputs and preserve the verified 0.5.0 release once."""
import argparse,hashlib,json,re,shutil
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--name',required=True)
p.add_argument('--freeze-baseline',action='store_true')
a=p.parse_args();root=Path(__file__).resolve().parents[1]
if not re.fullmatch(r'[a-z][a-z0-9]*(?:-[a-z0-9]+)*',a.name) or len(a.name)>64:
    p.error('feature name must be a short lowercase hyphenated name')
release=root/'dist/releases/esp32p4-0.5.0'
if a.freeze_baseline:
    if release.exists():
        p.error('verified release already exists; refusing to overwrite')
    audit=json.loads((root/'dist/live-native-firmware/verification.json').read_text())
    board=json.loads((root/'dist/live-native-firmware/board-verification.json').read_text())
    module=json.loads((root/'build/browser-module-live/module.json').read_text())
    if not (audit['layout_passed'] and board['native_module_executed'] and board['flash_hash_verified']):
        p.error('baseline lacks verified layout, flash or module execution evidence')
    if board['firmware_sha256']!=audit['sha256'] or board['module_sha256']!=module['sha256']:
        p.error('board evidence and baseline artifacts disagree')
    binary=root/'dist/live-native-firmware/Vela-NativeLoader-P4-UNTESTED.bin'
    if hashlib.sha256(binary.read_bytes()).hexdigest()!=audit['sha256']:
        p.error('firmware digest mismatch')
    native=root/'dist/live-native-app/com.netsurf.browser/native/browser.elf'
    if hashlib.sha256(native.read_bytes()).hexdigest()!=module['sha256']:
        p.error('packaged module digest mismatch')
    release.mkdir(parents=True)
    for name in ['live-native-app','live-native-firmware']:
        shutil.copytree(root/'dist'/name,release/name)
    for name in ['platform','quickapp','patches','tests/openvela','tools']:
        shutil.copytree(root/name,release/'source'/name,
                        ignore=shutil.ignore_patterns('__pycache__','*.pyc'))
    for name in ['WORK_AREAS.md','ESP32P4-PORT-PLAN.md','Makefile']:
        shutil.copy2(root/name,release/'source'/name)
    evidence=release/'evidence';evidence.mkdir()
    for name in ['patch-reuse-audit.json','board-final-https.log','board-final-onclick.log',
                 'board-release-final-state.log','boot-final-trust-filter.log',
                 'module-async-test.log','trust-filter-test.log','transport-tls-test.log',
                 'exec-pool-test.log']:
        shutil.copy2(root/'build'/name,evidence/name)
    files={str(f.relative_to(release)).replace('\\','/'):hashlib.sha256(f.read_bytes()).hexdigest()
           for f in sorted(release.rglob('*')) if f.is_file()}
    (release/'release-manifest.json').write_text(json.dumps({'version':'0.5.0',
        'firmware_sha256':audit['sha256'],'module_sha256':module['sha256'],
        'scope':'basic hardware acceptance; see board-verification.json for limits',
        'files':files},indent=2)+'\n')
if not (release/'release-manifest.json').is_file():p.error('freeze the verified baseline first')
build=root/'build/openvela'/a.name;dist=root/'dist/candidates'/a.name
for directory in [build,dist]:directory.mkdir(parents=True,exist_ok=True)
profile={'name':a.name,'baseline':str(release),'build':str(build),'dist':str(dist),
         'source_areas':['platform/openvela','tests/openvela'],
         'status':'development; no hardware acceptance implied'}
path=build/'workarea.json'
if path.exists() and json.loads(path.read_text())!=profile:p.error('workarea profile conflict')
path.write_text(json.dumps(profile,indent=2)+'\n')
print(json.dumps(profile,indent=2))
