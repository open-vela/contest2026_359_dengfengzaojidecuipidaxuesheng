'use strict';
const $=id=>document.getElementById(id);
const sessionKey='device-space-session-v1';
let token='',path='data',pack=null,busy=false,connectionAttempt=0,connecting=false;
let pairBlockedUntil=0,pairTimer=0;
try{token=sessionStorage.getItem(sessionKey)||'';}catch{}
if(!/^[0-9a-f]{32}$/.test(token))token='';
const say=(message,error=false)=>{ $('status').textContent=message; $('status').classList.toggle('error',error); };
function remember(){try{if(token)sessionStorage.setItem(sessionKey,token);else sessionStorage.removeItem(sessionKey);}catch{}}
function connectionView(connected){
  $('workspace').hidden=!connected;$('connect').hidden=connected;$('disconnect').hidden=!connected;
  $('connection-badge').textContent=connected?'已连接设备':'等待连接';
  $('connection-badge').classList.toggle('offline',!connected);
  $('retry-connection').hidden=true;
}
function forget(){token='';remember();connectionView(false);$('pair-code').value='';}
function pairButton(){
  const seconds=Math.max(0,Math.ceil((pairBlockedUntil-Date.now())/1000));
  $('pair-button').disabled=connecting||seconds>0;
  $('pair-button').textContent=connecting?'正在连接…':seconds?seconds+' 秒后重试':'连接设备';
  if(!seconds&&pairTimer){clearInterval(pairTimer);pairTimer=0;}
}
async function request(url,method='GET',body,authenticate=true){
  const requestToken=token,headers=authenticate?{Authorization:'Bearer '+requestToken}:{};
  if(body!==undefined&&!(body instanceof Blob)&&!(body instanceof Uint8Array)){headers['Content-Type']='application/json';body=JSON.stringify(body);}
  const controller=new AbortController();
  const timer=setTimeout(()=>controller.abort(),body instanceof Blob||body instanceof Uint8Array||url.startsWith('file?')?120000:15000);
  try{
    const response=await fetch('/api/'+url,{method,headers,body,cache:'no-store',signal:controller.signal});
    // Keep the deadline until the entire reply has arrived, including downloads.
    const r=new Response(await response.arrayBuffer(),{status:response.status,headers:response.headers});
    if(!r.ok){
      let data;try{data=await r.json();}catch{}
      if(authenticate&&r.status===401&&token===requestToken)forget();
      const error=Error(data?.error||'连接失败，请检查设备网络');
      error.status=r.status;error.retryAfter=Number(data?.retry_after)||0;throw error;
    }
    return r;
  }catch(e){
    if(e.name==='AbortError'||e instanceof TypeError){
      $('retry-connection').hidden=!token;$('connection-badge').textContent='连接中断';
      throw Error('暂时无法连接设备，请检查 Wi-Fi 后重试。');
    }
    throw e;
  }finally{clearTimeout(timer);}
}
const api=(url,method='GET',body)=>request(url,method,body,true);
const run=fn=>Promise.resolve().then(fn).catch(e=>say(e.message,true));
async function connect(code=''){
  const attempt=++connectionAttempt;
  connecting=true;connectionView(false);pairButton();$('connection-badge').textContent='正在连接';say('正在连接设备…');
  try{
    if(code){
      const data=await(await request('pair','POST',{code},false)).json();
      if(attempt!==connectionAttempt)return;
      if(!/^[0-9a-f]{32}$/.test(data.token))throw Error('设备响应不完整，请重试连接。');
      token=data.token;remember();
    }
    if(!token){connectionView(false);say('请输入设备屏幕上的连接令牌。');return;}
    for(const id of ['ai','homeassistant','music','voice','desktop']){
      await loadConfig(id,attempt);if(attempt!==connectionAttempt)return;
    }
    connectionView(true);$('pair-code').value='';say('已连接设备，可以开始配置。');
  }catch(e){
    if(attempt!==connectionAttempt)return;
    connectionView(false);$('retry-connection').hidden=!token||Boolean(e.status);
    if(e.retryAfter>0){
      pairBlockedUntil=Date.now()+Math.min(e.retryAfter,30)*1000;
      if(!pairTimer)pairTimer=setInterval(pairButton,250);
    }
    say(e.message,true);
  }finally{if(attempt===connectionAttempt){connecting=false;pairButton();}}
}
function scannedCode(){
  const value=location.hash.slice(1);history.replaceState(null,'',location.pathname+location.search);
  return /^[0-9]{6}$/.test(value)?value:'';
}
$('pair-code').addEventListener('input',e=>{e.target.value=e.target.value.replace(/[０-９]/g,c=>String(c.charCodeAt(0)-0xff10)).replace(/[\s-]/g,'').slice(0,6);});
$('pair-form').onsubmit=e=>{e.preventDefault();if(!connecting&&Date.now()>=pairBlockedUntil)connect($('pair-code').value);};
$('retry-connection').onclick=()=>connect();
$('disconnect').onclick=()=>{connectionAttempt++;connecting=false;forget();pairButton();say('已断开此网页，输入令牌即可重新连接。');};
window.addEventListener('hashchange',()=>{const code=scannedCode();if(code)connect(code);});
document.querySelectorAll('[data-tab]').forEach(b=>b.onclick=()=>{document.querySelectorAll('[data-tab]').forEach(x=>x.classList.toggle('active',x===b));for(const id of ['config','files','install'])$(id).hidden=id!==b.dataset.tab;if(b.dataset.tab==='files')run(list);});
async function loadConfig(id,attempt){const data=await(await api('config?section='+id)).json();if(attempt!==undefined&&attempt!==connectionAttempt)return;const form=$(id);for(const [key,value]of Object.entries(data)){if(key.endsWith('_set')){form.querySelector('.saved').textContent=value?'已保存密钥，留空即可保留':'未设置自定义密钥';continue;}const el=form.elements.namedItem(key);if(el)el.value=id==='ai'&&key==='timeout_ms'?Math.max(300000,value):value;}}
for(const id of ['ai','homeassistant','music','voice','desktop']){
  $(id).onsubmit=e=>{e.preventDefault();run(async()=>{const form=e.target,button=form.querySelector('button');button.disabled=true;try{const data=Object.fromEntries(new FormData(form));for(const key of ['api_key','token','secret_id','secret_key'])if(data[key]==='')delete data[key];for(const key of ['max_tokens','timeout_ms','light','palette','widget','reduced_motion'])if(key in data)data[key]=Number(data[key]);await api('config?section='+id,'POST',data);form.querySelectorAll('input[type=password]').forEach(x=>x.value='');say(id==='desktop'?'外观已提交，设备正在应用和保存。':id==='voice'?'语音输入配置已保存。':id==='homeassistant'?'智能家居配置已保存并应用。':'已保存，下次使用时生效。');if(id!=='desktop')await loadConfig(id);}finally{button.disabled=false;}});};
}
const size=n=>n<1024?n+' B':n<1048576?(n/1024).toFixed(1)+' KB':(n/1048576).toFixed(2)+' MB';
/* The manager browses the whole volume: show the real path a shortcut points at. */
function displayPath(p){if(p==='data')return '/data';if(p.startsWith('data/'))return '/'+p;if(p==='files')return '/data/files';if(p.startsWith('files/'))return '/data/'+p;if(p==='sdcard')return '/sdcard';if(p.startsWith('sdcard/'))return '/'+p;return p;}
async function list(){const data=await(await api('files?path='+encodeURIComponent(path))).json();$('path').textContent=displayPath(path);$('file-list').replaceChildren();for(const item of data.items.sort((a,b)=>Number(b.directory)-Number(a.directory)||a.name.localeCompare(b.name))){const row=document.createElement('div');row.className='file-row';const name=document.createElement('span');name.textContent=(item.directory?'▣  ':'▤  ')+item.name;const info=document.createElement('small');info.textContent=item.directory?'文件夹':size(item.size);const button=document.createElement('button');button.textContent=item.directory?'打开':'下载';button.onclick=()=>run(async()=>{if(item.directory){path+='/'+item.name;await list();}else{button.disabled=true;try{const blob=await(await api('file?path='+encodeURIComponent(path+'/'+item.name))).blob();if(blob.size!==item.size)throw Error('下载不完整，请重新扫描二维码后重试');const url=URL.createObjectURL(blob),a=document.createElement('a');a.href=url;a.download=item.name;a.click();setTimeout(()=>URL.revokeObjectURL(url),60000);}finally{button.disabled=false;}}});row.append(name,info,button);$('file-list').append(row);}if(!data.items.length)$('file-list').textContent='这里还没有文件，传入第一份吧。';if(data.truncated)say('此目录仅显示前 256 项，请使用子文件夹整理。');}
$('root').onchange=()=>{path=$('root').value;run(list);};$('up').onclick=()=>{if(path.includes('/'))path=path.slice(0,path.lastIndexOf('/'));run(list);};$('refresh').onclick=()=>run(list);
$('mkdir').onclick=()=>run(async()=>{const name=$('folder').value.trim();if(!validPath(name)||name.includes('/'))throw Error('请输入有效文件夹名称（最多 31 字节）');await api('mkdir?path='+encodeURIComponent(path+'/'+name),'POST',{});$('folder').value='';await list();});
$('upload').onchange=e=>run(async()=>{const input=e.target;input.disabled=true;try{for(const f of input.files){if(!validPath(f.name)||f.name.includes('/'))throw Error('文件名不符合要求：'+f.name);say('正在上传 '+f.name);await api('file?path='+encodeURIComponent(path+'/'+f.name),'PUT',f);}say('文件上传完成');await list();}finally{input.disabled=false;input.value='';}});
function validPath(name){const parts=name.split('/');return new TextEncoder().encode(name).length<=160&&parts.length<=4&&parts.every(p=>p&&!p.startsWith('.')&&!/[\\:*?"<>|%\x00-\x1f\x7f]/.test(p)&&new TextEncoder().encode(p).length<=31);}
async function decompressor(){if(window.fflate)return window.fflate;for(const src of ['https://cdn.jsdelivr.net/npm/fflate@0.8.2/umd/index.js','https://unpkg.com/fflate@0.8.2/umd/index.js']){try{await new Promise((resolve,reject)=>{const s=document.createElement('script'),timer=setTimeout(()=>{s.remove();reject(Error('timeout'));},12000);s.src=src;s.referrerPolicy='no-referrer';s.onload=()=>{clearTimeout(timer);resolve();};s.onerror=()=>{clearTimeout(timer);s.remove();reject(Error('load'));};document.head.append(s);});if(window.fflate)return window.fflate;}catch{}}throw Error('无法加载解压库，请让手机联网后重新选择安装包。');}
// Read the ZIP directory first. Expansion is bounded before any allocation by the decoder.
function crc32(bytes){let crc=0xffffffff;for(const b of bytes){crc^=b;for(let i=0;i<8;i++)crc=(crc>>>1)^((crc&1)?0xedb88320:0);}return (crc^0xffffffff)>>>0;}
function zipEntries(bytes){const v=new DataView(bytes.buffer,bytes.byteOffset,bytes.byteLength);let end=-1;for(let i=bytes.length-22;i>=Math.max(0,bytes.length-65557);i--)if(v.getUint32(i,true)===0x06054b50&&i+22+v.getUint16(i+20,true)===bytes.length){end=i;break;}if(end<0||v.getUint16(end+4,true)||v.getUint16(end+6,true))throw Error('不是支持的 ZIP / QPK 包');const count=v.getUint16(end+10,true);let off=v.getUint32(end+16,true),total=0;const entries=[],names=new Set();if(off+v.getUint32(end+12,true)!==end)throw Error('安装包目录不合法');for(let i=0;i<count;i++){if(off+46>end||v.getUint32(off,true)!==0x02014b50)throw Error('安装包目录损坏');const flags=v.getUint16(off+8,true),method=v.getUint16(off+10,true),n=v.getUint32(off+24,true),len=v.getUint16(off+28,true),extra=v.getUint16(off+30,true),comment=v.getUint16(off+32,true),local=v.getUint32(off+42,true);if(off+46+len+extra+comment>end||local+30>off)throw Error('安装包目录损坏');const name=new TextDecoder('utf-8',{fatal:true}).decode(bytes.subarray(off+46,off+46+len));if(flags&1||![0,8].includes(method)||names.has(name))throw Error('包包含加密或重复的文件');names.add(name);if(v.getUint32(local,true)!==0x04034b50)throw Error('文件头损坏');const localLen=v.getUint16(local+26,true),localExtra=v.getUint16(local+28,true);const localName=new TextDecoder('utf-8',{fatal:true}).decode(bytes.subarray(local+30,local+30+localLen));if(localName!==name||v.getUint16(local+8,true)!==method||v.getUint16(local+6,true)!==flags||local+30+localLen+localExtra+v.getUint32(off+20,true)>v.getUint32(end+16,true))throw Error('安装包内容不一致');if(!name.endsWith('/')){total+=n;entries.push({name,size:n,local,compressed:v.getUint32(off+20,true),crc:v.getUint32(off+16,true)});}off+=46+len+extra+comment;}if(off!==end)throw Error('安装包目录损坏');return entries;}
$('package').onchange=e=>run(async()=>{pack=null;$('preview').hidden=true;const f=e.target.files[0];if(!f)return;$('install-status').textContent='正在检查安装包…';const bytes=new Uint8Array(await f.arrayBuffer()),entries=zipEntries(bytes);const manifests=entries.filter(x=>/(^|\/)manifest\.json$/.test(x.name));if(manifests.length!==1)throw Error('需要唯一的 manifest.json');const prefix=manifests[0].name.slice(0,-13);for(const item of entries){if(!item.name.startsWith(prefix)||!validPath(item.name.slice(prefix.length)))throw Error('包中存在不支持的路径或过长文件名');}const lib=await decompressor();const files={};for(const item of entries){const v=new DataView(bytes.buffer),local=item.local,start=local+30+v.getUint16(local+26,true)+v.getUint16(local+28,true);const expected=item.size;const output=new Uint8Array(expected);let actual;if(v.getUint16(local+8,true)===0){output.set(bytes.subarray(start,start+expected));actual=output;}else actual=lib.inflateSync(bytes.subarray(start,start+item.compressed),{out:output});if(actual.length!==expected||crc32(actual)!==item.crc)throw Error('文件解压校验失败，安装包可能已损坏');files[item.name.slice(prefix.length)]=actual;}const manifest=JSON.parse(new TextDecoder('utf-8',{fatal:true}).decode(files['manifest.json']));const entry=manifest.entry||'app.js';if(!manifest.name||typeof manifest.package!=='string'||!validPath(manifest.package)||manifest.package.includes('/')||!entry.endsWith('.js')||!files[entry]||!files[entry].length)throw Error('包需要有效名称、包名和 JavaScript 入口');pack={manifest,files};$('app-name').textContent=manifest.name;$('app-detail').textContent=manifest.package+' · '+entries.length+' 个文件 · '+size(entries.reduce((s,e)=>s+e.size,0));$('preview').hidden=false;$('install-status').textContent='已在浏览器完成解压，可以安装。';});
$('install-button').onclick=()=>run(async()=>{if(!pack||busy)return;busy=true;const button=$('install-button'),input=$('package');button.disabled=input.disabled=true;$('progress').hidden=false;$('progress').value=0;try{await api('install/begin','POST',pack.manifest);const entries=Object.entries(pack.files).filter(([name])=>name!=='manifest.json');let n=0;for(const [name,data]of entries){$('install-status').textContent='正在传输 '+name;await api('install/file?path='+encodeURIComponent(name),'PUT',new Blob([data]));$('progress').value=++n/entries.length*95;}await api('install/commit','POST',{});$('progress').value=100;$('install-status').textContent='安装完成！在设备上重新打开应用列表即可找到它。';pack=null;$('preview').hidden=true;say('快应用安装完成');}catch(e){try{await api('install/abort','POST',{});}catch{}$('install-status').textContent='安装未完成，可重新尝试。';throw e;}finally{busy=false;button.disabled=input.disabled=false;}});
const initialCode=scannedCode();
if(initialCode||token)connect(initialCode);else connectionView(false);
