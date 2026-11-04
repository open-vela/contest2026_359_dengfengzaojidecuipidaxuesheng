#!/usr/bin/env python3
"""Run actual Uvicorn on an ephemeral loopback port, seed, download and stop."""
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import hashlib

repo=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='store-live-') as tmp:
    root=Path(tmp);token=secrets.token_urlsafe(48)
    env={**os.environ,'STORE_DATABASE':str(root/'store.sqlite3'),'STORE_ADMIN_TOKEN':token,'PYTHONUTF8':'1'}
    with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
    base=f'http://127.0.0.1:{port}'
    with (root/'server.log').open('w',encoding='utf-8') as log:
        process=subprocess.Popen([sys.executable,'-m','uvicorn','cloud.app_store.app:app','--host','127.0.0.1','--port',str(port),'--no-access-log'],cwd=repo,env=env,stdout=log,stderr=subprocess.STDOUT)
        try:
            for n in range(100):
                try:
                    with urllib.request.urlopen(base+'/healthz',timeout=.5) as response:assert json.load(response)['ok']
                    break
                except (urllib.error.URLError,ConnectionError):
                    if process.poll() is not None:raise RuntimeError('server exited before readiness')
                    time.sleep(.05)
            else:raise RuntimeError('server startup timeout')
            seeded=subprocess.run([sys.executable,'-m','cloud.app_store.seed','--base',base],cwd=repo,env=env,capture_output=True,text=True,timeout=20)
            assert seeded.returncode==0,seeded.stderr
            with urllib.request.urlopen(base+'/v1/catalog',timeout=5) as response:catalog=json.load(response)
            assert len(catalog['apps'])==3
            for app in catalog['apps']:
                with urllib.request.urlopen(base+'/v1/blobs/'+app['sha256']+'/app.js',timeout=5) as response:
                    source=response.read();assert response.headers['Content-Type'].startswith('application/javascript')
                assert len(source)==app['bytes'] and hashlib.sha256(source).hexdigest()==app['sha256']
            for path in ('/admin','/admin/app.js','/admin/style.css','/openapi.json'):
                with urllib.request.urlopen(base+path,timeout=5) as response:assert response.status==200 and response.read()
            # Wrong/no credentials never expose administration data.
            try:urllib.request.urlopen(base+'/v1/admin/apps',timeout=5)
            except urllib.error.HTTPError as error:assert error.code==401
            else:raise AssertionError('administration unexpectedly public')
        finally:
            if os.name=='nt':
                # A Windows venv launcher may own a separate Python process.
                # Terminate only this test's returned PID and its descendants.
                subprocess.run(['taskkill','/PID',str(process.pid),'/T','/F'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            else:process.terminate()
            try:process.wait(timeout=10)
            except subprocess.TimeoutExpired:process.kill();process.wait(timeout=5)
            for _ in range(100):
                try:
                    connection=socket.create_connection(('127.0.0.1',port),timeout=.1);connection.close();time.sleep(.05)
                except OSError:break
            else:raise RuntimeError('test server did not release its loopback port')
    # Allow the Windows launcher and inherited output handle to be reaped.
    time.sleep(.1)
print('CLOUD_LOOPBACK_OK: actual Uvicorn TCP; three seed publications; catalog; immutable source bytes/SHA; admin HTML/CSS/JS and OpenAPI; denied anonymous administration; test server/port released')
