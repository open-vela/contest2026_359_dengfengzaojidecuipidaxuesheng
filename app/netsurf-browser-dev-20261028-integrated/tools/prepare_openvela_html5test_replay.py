#!/usr/bin/env python3
"""Cache unmodified public HTML5test inputs for deterministic host debugging.

This is not the live-board acceptance test and never substitutes feature
results. Browser-identification JS is the real public server response.
"""
import argparse,concurrent.futures,gzip,hashlib,json,urllib.request
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
items=[('https://html5test.co/','index.html','text/html; charset=utf-8'),
 ('https://html5test.co/css/main.css','main.css','text/css'),
 ('https://html5test.co/scripts/base.js','base.js','text/javascript'),
 ('https://html5test.co/scripts/9/engine.js','engine.js','text/javascript'),
 ('https://html5test.co/scripts/9/data.js','data.js','text/javascript'),
 ('https://api.whichbrowser.net/rel/detect.js?ua=NetSurf%2F3.12&e=0&f=16&w=1024&h=600','detect.js','text/javascript')]
def get(item):
    url,name,mime=item
    req=urllib.request.Request(url,headers={'User-Agent':'NetSurf/3.12','Accept-Encoding':'identity'})
    with urllib.request.urlopen(req,timeout=30) as response:
        body=response.read(4*1024*1024+1)
        if len(body)>4*1024*1024:raise ValueError('fixture too large')
        if response.headers.get('Content-Encoding')=='gzip':body=gzip.decompress(body)
    (a.output/name).write_bytes(body)
    return {'url':url,'file':name,'mime':mime,'sha256':hashlib.sha256(body).hexdigest(),'bytes':len(body)}
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:rows=list(pool.map(get,items))
(a.output/'manifest.json').write_text(json.dumps(rows,indent=2)+'\n')
print('UNMODIFIED_REPLAY_INPUTS_READY',len(rows))
