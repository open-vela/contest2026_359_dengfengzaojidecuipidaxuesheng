import hashlib
import json
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from cloud.app_store.app import create_app

TOKEN='fixture-administrator-token-32-bytes-minimum'
AUTH={'Authorization':'Bearer '+TOKEN}


def payload(**extra):
    return {'package':'store.cloud_demo','name':'云端测试','version':'1.0.0','category':0,
            'icon':'grid','summary':'A real reference script',
            'source':"ui.sidebar([]); ui.text('Cloud app',24,24,600,28,ui.primary);",**extra}


@pytest.fixture
def client(tmp_path):
    with TestClient(create_app(tmp_path/'data/store.sqlite3',TOKEN)) as session:
        yield session


def test_public_catalog_and_etag(client):
    assert client.get('/healthz').json()['ok']
    r=client.get('/v1/catalog');assert r.status_code==200
    assert r.json()['schema']==1 and r.json()['apps']==[]
    assert client.get('/v1/catalog',headers={'If-None-Match':r.headers['etag']}).status_code==304


def test_admin_authentication(client):
    assert client.post('/v1/admin/apps',json=payload()).status_code==401
    assert client.post('/v1/admin/apps',json=payload(),headers={'Authorization':'Bearer wrong'}).status_code==401
    assert client.get('/v1/admin/apps').status_code==401
    assert client.put('/v1/admin/home',json={},headers=AUTH).status_code==422


def test_publish_download_and_immutable_versions(client):
    p=payload();r=client.post('/v1/admin/apps',json=p,headers=AUTH);assert r.status_code==200,r.text
    app=r.json()['application'];data=p['source'].encode();sha=hashlib.sha256(data).hexdigest()
    assert app['sha256']==sha and app['bytes']==len(data) and 'source' not in app
    assert client.get('/v1/catalog').json()['apps']==[app]
    blob=client.get('/v1/blobs/'+sha+'/app.js');assert blob.content==data
    assert blob.headers['x-content-type-options']=='nosniff'
    assert client.get('/v1/blobs/'+sha+'/app.js',headers={'If-None-Match':blob.headers['etag']}).status_code==304
    assert client.post('/v1/admin/apps',json=p,headers=AUTH).status_code==200
    assert client.post('/v1/admin/apps',json=payload(source='changed;'),headers=AUTH).status_code==409
    assert client.post('/v1/admin/apps',json=payload(version='2.0.0',source='2;'),headers=AUTH).status_code==200
    assert client.get('/v1/catalog').json()['apps'][0]['version']=='2.0.0'
    assert client.post('/v1/admin/apps',json=p,headers=AUTH).status_code==200
    assert client.get('/v1/catalog').json()['apps'][0]['version']=='2.0.0'
    assert client.get('/v1/blobs/'+sha+'/app.js').content==data


def test_home_publication_and_unpublish(client):
    app=client.post('/v1/admin/apps',json=payload(),headers=AUTH).json()['application']
    home={'headline':'New edition','tagline':'Catalog driven','featured':[{'package':app['package'],'title':'A new\nedition','art':'none','tone':'lavender'}]}
    assert client.put('/v1/admin/home',json=home,headers=AUTH).status_code==200
    assert client.get('/v1/catalog').json()['featured'][0]['package']==app['package']
    assert client.delete('/v1/admin/apps/'+app['package'],headers=AUTH).status_code==200
    catalog=client.get('/v1/catalog').json();assert catalog['apps']==[] and catalog['featured']==[]
    assert client.get('/v1/blobs/'+app['sha256']+'/app.js').status_code==200
    assert client.post('/v1/admin/apps',json=payload(),headers=AUTH).status_code==200
    assert client.get('/v1/catalog').json()['apps']==[]
    assert client.post('/v1/admin/apps/'+app['package']+'/restore',headers=AUTH).status_code==200
    assert len(client.get('/v1/catalog').json()['apps'])==1


@pytest.mark.parametrize('extra',[
    {'package':'../escape'}, {'package':'store.a.b'}, {'package':'ai.custom'},
    {'name':'中'*16}, {'summary':'x'*192}, {'version':'x'*24}, {'category':3},
    {'source':'x'*65537}, {'source':'nul\0;'}, {'source':''}, {'icon':'html'},
    {'name':'bad\nname'}, {'name':'\ud800'}, {'category':True},
])
def test_reject_incompatible_packages(client,extra):
    response=client.post('/v1/admin/apps',content=json.dumps(payload(**extra)).encode(),headers={**AUTH,'Content-Type':'application/json'})
    assert response.status_code==422,response.text
    assert 'source' not in response.json().get('detail',[{}])[0].keys()


def test_builtin_and_catalog_capacity(client):
    assert client.post('/v1/admin/apps',json=payload(package='store.focus'),headers=AUTH).status_code==409
    for n in range(21):assert client.post('/v1/admin/apps',json=payload(package=f'store.tool{n}'),headers=AUTH).status_code==200
    assert client.post('/v1/admin/apps',json=payload(package='store.extra'),headers=AUTH).status_code==409
    assert len(client.get('/v1/catalog').json()['apps'])==21


def test_bad_home_references_and_presets(client):
    base={'headline':'H','tagline':'T','featured':[]}
    for feature in [
        {'package':'store.absent','title':'Bad'}, {'package':'store.focus','title':'Bad','art':'script'},
        {'package':'store.focus','title':'a\nb\nc'},
    ]:
        assert client.put('/v1/admin/home',json={**base,'featured':[feature]},headers=AUTH).status_code==422
    assert client.put('/v1/admin/home',json={**base,'featured':[{'package':'store.focus','title':'A'}]*2},headers=AUTH).status_code==422
    assert client.put('/v1/admin/home',json=base,headers=AUTH).status_code==200


def test_oversize_streaming_body(client):
    assert client.post('/v1/admin/apps',content=(b'x'*8192 for _ in range(70)),headers=AUTH).status_code==413


def test_concurrent_publication_is_idempotent(client):
    with ThreadPoolExecutor(max_workers=6) as workers:
        codes=list(workers.map(lambda _:client.post('/v1/admin/apps',json=payload(),headers=AUTH).status_code,range(12)))
    assert codes==[200]*12
    assert len(client.get('/v1/catalog').json()['apps'])==1


def test_restart_keeps_publication_and_home(tmp_path):
    db=tmp_path/'store.sqlite3'
    with TestClient(create_app(db,TOKEN)) as client:
        assert client.post('/v1/admin/apps',json=payload(),headers=AUTH).status_code==200
        assert client.put('/v1/admin/home',json={'headline':'Saved','tagline':'Kept','featured':[]},headers=AUTH).status_code==200
    with TestClient(create_app(db,TOKEN)) as client:
        catalog=client.get('/v1/catalog').json();assert catalog['headline']=='Saved' and len(catalog['apps'])==1


def test_readonly_and_admin_assets(tmp_path):
    with TestClient(create_app(tmp_path/'store.sqlite3','')) as client:
        assert client.get('/v1/catalog').status_code==200
        assert client.get('/v1/admin/apps',headers=AUTH).status_code==503
        assert client.get('/admin').status_code==200
        assert client.get('/admin/app.js').status_code==200
        assert client.get('/admin/style.css').status_code==200
        assert client.get('/admin/not_allowed').status_code==404
    with pytest.raises(ValueError):create_app(tmp_path/'bad.sqlite3','weak')
