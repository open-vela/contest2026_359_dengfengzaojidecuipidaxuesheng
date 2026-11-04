"""Publish the three reference scripts under distinct cloud identities."""
import argparse
import json
import os
import ipaddress
import re
from pathlib import Path
import urllib.request
import urllib.parse

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base',default='http://127.0.0.1:8080')
    p.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[2]/'quickapp/store')
    a=p.parse_args()
    address=urllib.parse.urlsplit(a.base)
    if address.scheme!='https':
        try:local=address.scheme=='http' and (address.hostname=='localhost' or ipaddress.ip_address(address.hostname).is_private or ipaddress.ip_address(address.hostname).is_loopback)
        except ValueError:local=False
        if not local:p.error('use HTTPS for a public administrator endpoint')
    if address.username or address.password or address.query or address.fragment:p.error('use a plain service root URL without credentials/query/fragment')
    token=os.environ.get('STORE_ADMIN_TOKEN','')
    if len(token.encode())<32:p.error('set STORE_ADMIN_TOKEN (at least 32 bytes) in the environment')
    for item in json.loads((a.source/'catalog.json').read_text(encoding='utf-8'))['apps']:
        if not re.fullmatch(r'[a-z][a-z0-9_]*',item['id']):p.error('invalid reference source identity')
        record={key:value for key,value in item.items() if key!='id'}
        record['package']='store.cloud_'+item['id']
        record['name']=item['name']+' · 云端'
        record['source']=(a.source/item['id']/'app.js').read_text(encoding='utf-8')
        body=json.dumps(record,ensure_ascii=False).encode()
        request=urllib.request.Request(a.base.rstrip('/')+'/v1/admin/apps',data=body,method='POST',headers={'Authorization':'Bearer '+token,'Content-Type':'application/json'})
        with urllib.request.urlopen(request,timeout=15) as response:
            result=json.load(response)
        print('Published',result['application']['package'],result['application']['version'])

if __name__=='__main__':main()
