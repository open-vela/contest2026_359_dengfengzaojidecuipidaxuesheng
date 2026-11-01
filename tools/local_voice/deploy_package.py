#!/usr/bin/env python3
"""Install only a new, host-verified SD release over the existing paired portal.

Never overwrite existing files, change board configuration, or flash firmware.
The binary manifest is published last; the app verifies all SHA-256 on device.
"""
import argparse,concurrent.futures,hashlib,http.client,io,ipaddress,json,re,sys,time,urllib.parse,secrets
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from music_device_console import Console
def main():
 p=argparse.ArgumentParser();p.add_argument('--release',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--port',default='COM4');p.add_argument('--verify-published',action='store_true');a=p.parse_args()
 root=a.release.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
 manifest=json.loads((root/'manifest.json').read_text());files=manifest['files']
 files += [dict(path=n,bytes=(root/n).stat().st_size,sha256=hashlib.sha256((root/n).read_bytes()).hexdigest()) for n in ('manifest.json','manifest.bin')]
 for row in files:
  f=root/row['path'];assert f.resolve().is_relative_to(root) and not f.is_symlink()
  assert f.stat().st_size==row['bytes'] and hashlib.sha256(f.read_bytes()).hexdigest()==row['sha256']
 release=root.name;assert re.fullmatch('[a-z0-9-]{1,63}',release)
 target='sdcard/local-voice/models/'+release
 if a.verify_published:
  report=json.loads((out/'DEPLOYMENT.json').read_text());assert report['target']=='/'+target and len(report['files'])==len(files)
  assert report['source_manifest_sha256']==hashlib.sha256((root/'manifest.bin').read_bytes()).hexdigest()
  with (out/'verify-published.log').open('w',encoding='utf-8') as log:
   c=Console(log,a.port)
   try:
    c.command('cd /sdcard/local-voice/models');listing=c.command('ls -l '+release)
    assert 'manifest.bin' in listing.lower();c.command('cd /')
   finally:c.close()
  report['published']=True;report['device_sha256_validation_pending']=True
  (out/'DEPLOYMENT.json').write_text(json.dumps(report,indent=2)+'\n');print('SD release publication confirmed; runtime SHA check pending');return
 stage='sdcard/local-voice/models/.lv-'+secrets.token_hex(4)
 report=dict(target='/'+target,staging='/'+stage,published=False,files=[],source_manifest_sha256=hashlib.sha256((root/'manifest.bin').read_bytes()).hexdigest(),overwrite=False)
 def save():(out/'DEPLOYMENT.json').write_text(json.dumps(report,indent=2)+'\n')
 save();secret=io.StringIO();console=Console(secret,a.port)
 try:
  text=console.command('ifconfig');match=re.search(r'wlan0[\s\S]*?inet addr:([0-9.]+)',text);assert match
  address=match[1];assert ipaddress.ip_address(address).is_private and address!='0.0.0.0'
  text=console.command('desktop portal-pair');match=re.search(r'PORTAL_PAIR=(\d{6})',text);assert match;code=match[1];del text,match
  with (out/'device.log').open('w',encoding='utf-8') as log:
   console.log=log;secret.close()
   assert 'active=0 ' in console.command('desktop ui recorder-status'),'Recording active; stop deployment'
   mounts=console.command('mount');assert re.search(r'/sdcard type (?:vfat|fatfs)',mounts),'Real FAT SD required'
   def request(method,endpoint,path=None,body=None,token=None,allow_error=False):
    if path is not None:endpoint+='?path='+urllib.parse.quote(path,safe='')
    headers={}
    if token:headers['Authorization']='Bearer '+token
    if isinstance(body,dict):body=json.dumps(body);headers['Content-Type']='application/json'
    if hasattr(body,'read'):headers['Content-Length']=str(Path(body.name).stat().st_size)
    client=http.client.HTTPConnection(address,8080,timeout=20)
    try:
     client.request(method,endpoint,body,headers);response=client.getresponse();data=response.read()
     if not allow_error:assert response.status==200,(endpoint.split('?')[0],response.status)
     return response.status,data
    finally:client.close()
   token=json.loads(request('POST','/api/pair',body={'code':code})[1])['token'];del code
   # This portal deliberately limits public components to 31 bytes for
   # SmartFS. FAT supports 64; publish the longer release via relative NSH mv.
   directories={'sdcard/local-voice','sdcard/local-voice/models',stage}
   for row in files:
    parent=Path(row['path']).parent
    while str(parent)!='.':directories.add(stage+'/'+parent.as_posix());parent=parent.parent
   for directory in sorted(directories,key=lambda v:(v.count('/'),v)):
    status,_=request('GET','/api/files',directory,token=token,allow_error=True)
    if status!=200:request('POST','/api/mkdir',directory,{},token)
   response=console.command('cd /sdcard/local-voice/models');assert not re.search(r'nsh:.*failed',response)
   response=console.command('ls '+release)
   assert re.search('No such file|not found',response,re.I),'Existing release preserved without overwriting'
   for row in files:
    began=time.monotonic();print('INSTALL',row['path'],row['bytes'],flush=True)
    with (root/row['path']).open('rb') as stream,concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
     future=pool.submit(request,'PUT','/api/file',stage+'/'+row['path'],stream,token)
     while not future.done():console.collect(.15)
     future.result()
    report['files'].append(dict(**row,seconds=round(time.monotonic()-began,3)));save()
   response=console.command('mv '+Path(stage).name+' '+release);assert not re.search(r'nsh:.*failed',response)
   response=console.command('ls '+release);assert 'manifest.bin' in response.lower()
   console.command('cd /')
   report['published']=True;report['device_sha256_validation_pending']=True;save()
   report['after']={cmd:console.command(cmd) for cmd in ('free','desktop ui backlight-status')};save()
 finally:console.close()
 print('LOCAL_VOICE_SD_INSTALLED: device SHA check still required',flush=True)
if __name__=='__main__':main()
