#!/usr/bin/env python3
"""Collect reproducible host acceptance logs for the independent Vela app."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--core',type=Path,required=True,help='prepared host NetSurf tree')
p.add_argument('--target',type=Path,required=True,help='reference NuttX/apps tree')
p.add_argument('--output',type=Path,required=True)
p.add_argument('--webjs',type=Path,help='namespaced webpage VM build directory')
a=p.parse_args()
root=Path(__file__).resolve().parents[1]
out=a.output.resolve(); out.mkdir(parents=True,exist_ok=True)
apps=a.target/'apps'
jobs={
    'core_and_native_app': [sys.executable,str(root/'tools/test_openvela.py'),
        '--core',str(a.core),'--lvgl',str(apps/'graphics/lvgl/lvgl'),
        '--quickjs',str(apps/'interpreters/quickjs/quickjs'),
        '--output',str(root/'build/openvela-host')],
    'http_transport': [sys.executable,str(root/'tools/test_openvela_transport.py'),
        '--target',str(a.target),'--core',str(a.core),
        '--output',str(root/'build/openvela-transport')],
    'native_module_loader': [sys.executable,str(root/'tools/test_openvela_module.py'),
        '--target',str(a.target),'--output',str(root/'build/module-host')],
}
if a.webjs: jobs['core_and_native_app']+=['--webjs',str(a.webjs)]
results={}
for name,command in jobs.items():
    started=time.monotonic()
    with (out/(name+'.log')).open('w') as log:
        run=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT)
    results[name]={'passed':run.returncode==0,'exit_code':run.returncode,
        'elapsed_seconds':round(time.monotonic()-started,2),'log':name+'.log','command':command}
    print(name,results[name]['passed'],flush=True)
sources={}
for directory in ['platform/openvela','platform/openvela_module']:
    for path in sorted((root/directory).glob('*')):
        if path.suffix in ['.c','.h']: sources[str(path.relative_to(root))]=hashlib.sha256(path.read_bytes()).hexdigest()
for name in ['app.js','manifest.json']:
    path=root/'quickapp/netsurf_browser'/name
    sources[str(path.relative_to(root))]=hashlib.sha256(path.read_bytes()).hexdigest()
report={'tests':results,'source_sha256':sources,'board_tested':False,
        'tls_host_engine':'OpenSSL test shim; target mbedTLS not exercised on device'}
if a.webjs: report['webpage_vm']=json.loads((a.webjs/'manifest.json').read_text())
(out/'validation.json').write_text(json.dumps(report,indent=2)+'\n')
raise SystemExit(0 if all(r['passed'] for r in results.values()) else 1)
