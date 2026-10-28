#!/usr/bin/env python3
"""Create the independent directory-based Vela Quick App distribution."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import zipfile

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
mode=p.add_mutually_exclusive_group(required=True)
mode.add_argument('--native-module',type=Path,help='build_openvela_module.py output directory')
mode.add_argument('--interface-only',action='store_true',help='legacy UI package requiring an embedded engine')
a=p.parse_args()
root=Path(__file__).resolve().parents[1]
app=root/'quickapp/netsurf_browser'
manifest=json.loads((app/'manifest.json').read_text(encoding='utf-8'))
assert manifest['entry']=='app.js'
out=a.output.resolve(); out.mkdir(parents=True,exist_ok=True)
package=out/manifest['package']
package.mkdir(exist_ok=True)
for name in ['manifest.json','app.js']:
    shutil.copy2(app/name,package/name)
module=None
if a.native_module:
    native=a.native_module.resolve()
    module=json.loads((native/'module.json').read_text())
    binary=native/'browser.elf'
    if hashlib.sha256(binary.read_bytes()).hexdigest()!=module['sha256']:
        p.error('native module digest mismatch')
    if (native/'host.abi').read_text().strip()!=module['host_abi']:
        p.error('native module ABI mismatch')
    dest=package/'native'; dest.mkdir(exist_ok=True)
    for name in ['browser.elf','host.abi','module.json']:
        shutil.copy2(native/name,dest/name)
    manifest['native']={'file':'native/browser.elf','abi':1,'hostAbi':module['host_abi']}
    (package/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
elif (package/'native').exists():
    p.error('output contains a native package; use a separate empty output for an interface-only package')
archive=out/('NetSurfBrowser-'+manifest['versionName']+'.zip')
with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as z:
    for file in sorted(package.rglob('*')):
        if file.is_file(): z.write(file,manifest['package']+'/'+str(file.relative_to(package)).replace('\\','/'))
    z.writestr('安装说明.txt',
        '将 com.netsurf.browser 文件夹放到 /sdcard/qpk/ 或 /data/qpk/，刷新桌面应用列表后打开 NetSurf 浏览器。\n'+
        ('已包含 native/browser.elf 浏览器引擎，需配套原生模块加载器固件；不兼容的系统会拒绝加载。\n'
         '首次成功加载后引擎驻留内存至重启，更新引擎后必须重启设备。\n' if module else
         '此包只有界面，需要配套固件提供 system.netsurf 原生引擎，不能作为完整独立浏览器安装包。\n')+
        '字体：/sdcard/fonts/VelaGlobal-Regular.ttf。HTTPS 信任根：/sdcard/netsurf/ca-bundle.pem（PEM）。\n'
        '设备需已有网络连接和正确时间。支持基础网页脚本，JPEG 仅支持 baseline。\n'
        '开发版尚未完成实板验收，不能保证现代网站兼容性。\n')
(out/'package.json').write_text(json.dumps({
    'package':manifest['package'],'version':manifest['versionName'],
    'archive':archive.name,'sha256':hashlib.sha256(archive.read_bytes()).hexdigest(),
    'requires':'matching Vela native-module loader' if module else 'Vela firmware exposing system.netsurf',
    'includes_browser_engine':module is not None,
    'host_abi':module['host_abi'] if module else None,
    'archive_bytes':archive.stat().st_size,
},indent=2)+'\n')
print(archive)
