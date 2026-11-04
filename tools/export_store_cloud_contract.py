#!/usr/bin/env python3
"""Export actual FastAPI responses for the native C installer's contract test."""
import argparse
import json
import tempfile
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from fastapi.testclient import TestClient
from cloud.app_store.app import create_app

p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
repo=Path(__file__).resolve().parents[1];a.output.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(prefix='store-api-contract-') as tmp:
    token='isolated-cloud-contract-administrator-token'
    source=(repo/'quickapp/store/calculator/app.js').read_text(encoding='utf-8')
    metadata={'package':'store.cloud_demo','name':'云端计算器','version':'1.0.0','summary':'从真实服务端获取的应用目录和脚本','category':0,'icon':'calculator','source':source}
    with TestClient(create_app(Path(tmp)/'store.sqlite3',token)) as client:
        response=client.post('/v1/admin/apps',json=metadata,headers={'Authorization':'Bearer '+token});assert response.status_code==200,response.text
        home={'headline':'Cloud contract','tagline':'Actual cloud catalog data','featured':[{'package':'store.cloud_demo','title':'Cloud edition','art':'none','tone':'lavender'}]}
        assert client.put('/v1/admin/home',json=home,headers={'Authorization':'Bearer '+token}).status_code==200
        catalog=client.get('/v1/catalog');assert catalog.status_code==200
        entry=catalog.json()['apps'][0]
        blob=client.get('/v1/blobs/'+entry['sha256']+'/app.js');assert blob.status_code==200
        (a.output/'catalog.json').write_bytes(catalog.content);(a.output/'app.js').write_bytes(blob.content)
        (a.output/'openapi.json').write_text(json.dumps(client.get('/openapi.json').json(),ensure_ascii=False,indent=2),encoding='utf-8')
        (a.output/'PROVENANCE.json').write_text(json.dumps({'public_catalog_status':catalog.status_code,'blob_status':blob.status_code,'sha256':entry['sha256'],'bytes':entry['bytes'],'source':'actual FastAPI ASGI responses; isolated SQLite; no production service'},indent=2))
print('CLOUD_CONTRACT_EXPORTED',a.output)
