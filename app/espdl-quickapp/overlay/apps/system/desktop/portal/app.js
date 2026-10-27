'use strict';
const $=id=>document.getElementById(id);
const sessionKey='device-space-session-v1';
let token='',path='data',pack=null,busy=false,connectionAttempt=0,connecting=false;
let pairBlockedUntil=0,pairTimer=0;
let activeRequests=0,loadedToken='',connectionInterrupted=false;
let networkEpoch=0,networkReading=false,networkSubmitting=false,networkBusy=false;
let networkWatching=false,networkRequested=false,networkUncertain=false;
try{token=sessionStorage.getItem(sessionKey)||'';}catch{}
if(!/^[0-9a-f]{32}$/.test(token))token='';
const say=(message,error=false)=>{ $('status').textContent=message; $('status').classList.toggle('error',error); };
function remember(){try{if(token)sessionStorage.setItem(sessionKey,token);else sessionStorage.removeItem(sessionKey);}catch{}}
function connectionView(connected){
  if(connected)connectionInterrupted=false;
  $('workspace').hidden=!connected;$('connect').hidden=connected;$('disconnect').hidden=!connected;
  $('connection-badge').textContent=connected?'已连接设备':'等待连接';
  $('connection-badge').classList.toggle('offline',!connected);
  $('retry-connection').hidden=true;
}
function forget(){token='';loadedToken='';remember();connectionView(false);$('pair-code').value='';resetNetwork();}
function pairButton(){
  const seconds=Math.max(0,Math.ceil((pairBlockedUntil-Date.now())/1000));
  $('pair-button').disabled=connecting||seconds>0;
  $('pair-button').textContent=connecting?'正在连接…':seconds?seconds+' 秒后重试':'连接设备';
  if(!seconds&&pairTimer){clearInterval(pairTimer);pairTimer=0;}
}
async function exchange(url,method,body,authenticate,attempt){
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
      if(authenticate&&r.status===401&&token===requestToken&&attempt===connectionAttempt)forget();
      const error=Error(data?.error||'连接失败，请检查设备网络');
      error.status=r.status;error.retryAfter=Number(data?.retry_after)||0;throw error;
    }
    return r;
  }finally{clearTimeout(timer);}
}
async function request(url,method='GET',body,authenticate=true){
  const attempt=connectionAttempt,requestToken=token;
  // Reads and the code exchange can be repeated; configuration writes cannot.
  const retries=method==='GET'||(!authenticate&&url==='pair')?2:0;
  activeRequests++;
  try{
    for(let retry=0;;retry++){
      try{
        const response=await exchange(url,method,body,authenticate,attempt);
        if(authenticate&&attempt===connectionAttempt&&token===requestToken&&
            loadedToken===token&&connectionInterrupted&&!connecting)connectionView(true);
        return response;
      }catch(e){
        const network=e.name==='AbortError'||e instanceof TypeError;
        const current=attempt===connectionAttempt&&token===requestToken;
        if(network&&current&&retry<retries){
          await new Promise(resolve=>setTimeout(resolve,250*(retry+1)));
          if(attempt===connectionAttempt&&token===requestToken)continue;
        }
        if(network){
          if(current){
            connectionInterrupted=true;
            $('retry-connection').hidden=!token;$('connection-badge').textContent='连接中断';
            $('connection-badge').classList.add('offline');
          }
          throw Error(method==='POST'&&url==='network/connect'?
            '连接请求的回执未收到，设备可能正在试连。请保留设备热点连接并查询状态，不要重复提交。':method==='POST'&&url.startsWith('config?')?
            '保存回执未收到，配置可能已保存。请恢复连接后核对，再决定是否重新提交。':
            '暂时无法连接设备，已保留填写内容，请检查 Wi-Fi 后重试。');
        }
        throw e;
      }
    }
  }finally{activeRequests--;}
}
const api=(url,method='GET',body)=>request(url,method,body,true);
const run=fn=>Promise.resolve().then(fn).catch(e=>say(e.message,true));
async function connect(code=''){
  const attempt=++connectionAttempt;
  resetNetwork();
  connecting=true;connectionView(false);pairButton();$('connection-badge').textContent='正在连接';say('正在连接设备…');
  try{
    if(code){
      const data=await(await request('pair','POST',{code},false)).json();
      if(attempt!==connectionAttempt)return;
      if(!/^[0-9a-f]{32}$/.test(data.token))throw Error('设备响应不完整，请重试连接。');
      token=data.token;remember();
    }
    if(!token){connectionView(false);say('请输入设备屏幕上的连接令牌。');return;}
    if(!code&&loadedToken===token){
      await api('session');if(attempt!==connectionAttempt)return;
    }else{
      for(const id of ['ai','homeassistant','music','voice','desktop','agent']){
        await loadConfig(id,attempt);if(attempt!==connectionAttempt)return;
      }
      loadedToken=token;
    }
    connectionView(true);$('pair-code').value='';say('已连接设备，可以开始配置。');
    networkWatching=true;
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
async function keepSession(){
  if(document.hidden||!token||loadedToken!==token||connecting||busy||activeRequests)return;
  try{await api('session');}catch(e){if(e.status===401)say(e.message,true);}
}
setInterval(keepSession,60000);
document.addEventListener('visibilitychange',()=>{if(!document.hidden)keepSession();});
window.addEventListener('online',keepSession);
window.addEventListener('hashchange',()=>{const code=scannedCode();if(code)connect(code);});
document.querySelectorAll('[data-tab]').forEach(b=>b.onclick=()=>{document.querySelectorAll('[data-tab]').forEach(x=>x.classList.toggle('active',x===b));for(const id of ['config','files','install'])$(id).hidden=id!==b.dataset.tab;if(b.dataset.tab==='files')run(list);});
async function loadConfig(id,attempt){const data=await(await api('config?section='+id)).json();if(attempt!==undefined&&attempt!==connectionAttempt)return;const form=$(id);for(const [key,value]of Object.entries(data)){if(key.endsWith('_set')){if(id!=='agent')form.querySelector('.saved').textContent=value?'已保存密钥，留空即可保留':'未设置自定义密钥';continue;}const el=form.elements.namedItem(key);if(el)el.value=id==='ai'&&key==='timeout_ms'?Math.max(300000,value):value;}if(id==='agent'){const saved=Object.entries(data).filter(([key,value])=>key.endsWith('_token_set')&&value).length;form.querySelector('.saved').textContent=saved?'已保存 '+saved+' 个 MCP 服务令牌，留空保留':'尚未保存 MCP 服务令牌';}}
function validateHomeAssistant(data){
  data.url=String(data.url||'').trim();
  const match=/^http:\/\/([a-zA-Z0-9._-]+)(?::([0-9]{1,5}))?\/?$/.exec(data.url);
  if(!match||data.url.length>127||(match[2]&&(Number(match[2])<1||Number(match[2])>65535)))
    throw Error('Home Assistant 地址应为 http://主机:端口，例如 http://192.168.1.10:8123；端口前用冒号，不是 /8123。');
  if('token' in data){
    data.token=data.token.trim();
    if(data.token.length>511||/[^\x21-\x7e]/.test(data.token))throw Error('长期访问令牌格式不正确（最多 511 字符），请重新复制完整令牌。');
    if(!data.token)delete data.token;
  }
}
for(const id of ['ai','homeassistant','music','voice','desktop','agent']){
  $(id).onsubmit=e=>{
    e.preventDefault();run(async()=>{
      const form=e.target,button=form.querySelector('button');button.disabled=true;
      try{
        const data=Object.fromEntries(new FormData(form));
        if(id==='homeassistant')validateHomeAssistant(data);
        for(const key of ['api_key','token','secret_id','secret_key','mcp1_token','mcp2_token','mcp3_token'])if(data[key]==='')delete data[key];
        if(id==='agent')for(let i=1;i<=3;i++){
          if(data['mcp'+i+'_clear_token'])data['mcp'+i+'_token']='';
          delete data['mcp'+i+'_clear_token'];
        }
        for(const key of ['max_tokens','timeout_ms','light','palette','widget','reduced_motion','skills_enabled','home_control_enabled','mcp1_enabled','mcp2_enabled','mcp3_enabled'])if(key in data)data[key]=Number(data[key]);
        await api('config?section='+id,'POST',data);
        form.querySelectorAll('input[type=password]').forEach(x=>x.value='');
        form.querySelectorAll('input[type=checkbox]').forEach(x=>x.checked=false);
        say(id==='desktop'?'外观已提交，设备正在应用和保存。':id==='voice'?'语音输入配置已保存。':id==='homeassistant'?'智能家居配置已保存并应用。':'已保存，下次使用时生效。');
        if(id!=='desktop'){
          try{await loadConfig(id);}catch(e){
            say('配置已保存，暂时无法读取最新配置。请恢复连接后核对。',true);
          }
        }
      }finally{button.disabled=false;}
    });
  };
}
function networkSay(message,error=false){
  $('network-status').textContent=message;
  $('network-status').classList.toggle('error',error);
}
function networkControls(){
  $('network-connect').disabled=networkSubmitting||networkBusy;
  $('network-refresh').disabled=networkReading;
}
function resetNetwork(){
  networkEpoch++;networkReading=networkSubmitting=networkBusy=false;
  networkWatching=networkRequested=networkUncertain=false;
  $('network-password').value='';$('network-ap').textContent='';
  networkSay('正在读取网络状态…');networkControls();
}
function networkCurrent(epoch,attempt,bearer){
  return epoch===networkEpoch&&attempt===connectionAttempt&&bearer===token&&!!token;
}
function validateNetwork(ssid,password){
  const length=new TextEncoder().encode(ssid).length;
  if(!length||length>32||/[\x00-\x1f\x7f]/.test(ssid))
    throw Error('网络名称须为 1–32 个 UTF-8 字节，不能包含控制字符；中文通常占 3 个字节。');
  if(password!==''&&(!/^[\x20-\x7e]{8,63}$/.test(password)&&!/^[0-9a-fA-F]{64}$/.test(password)))
    throw Error('密码须为 8–63 个可打印 ASCII 字符，或 64 位十六进制密钥；开放网络可留空。');
}
function networkSnapshot(data){
  if(data?.ok!==true||typeof data.busy!=='boolean'||!Number.isInteger(data.error)||
      typeof data.associated!=='boolean'||typeof data.ipv4_ready!=='boolean'||
      typeof data.ipv4!=='string'||typeof data.ap_running!=='boolean'||
      !Number.isInteger(data.ap_state)||!Number.isInteger(data.ap_error)||
      typeof data.ap_ssid!=='string'||typeof data.ap_ip!=='string')
    throw Error('设备网络状态响应不完整，请稍后查询。');
  networkBusy=data.busy;
  $('network-ap').textContent=data.ap_running?
    '设备热点：'+data.ap_ssid+' · http://'+data.ap_ip+':8080。无互联网也请保留连接。':
    data.ap_error?'设备热点操作失败（'+data.ap_error+'），请在设备 Wi-Fi 页面核对。':
    '设备热点未开启。需要配网时，请在设备 Wi-Fi 页面手动开启。';
  if(data.error){
    networkWatching=data.busy;networkRequested=false;$('network-password').value='';
    networkSay(data.associated?
      '路由器已关联，但获取 IP 或保存新配置失败（'+data.error+'）。请核对设备提示后重试，不能视为配网成功。':
      '连接路由器失败（'+data.error+'）。请检查网络名称、密码和路由器；未确认保存新配置。',true);
  }else if(data.busy){
    networkWatching=true;
    networkSay(data.associated?'路由器已关联，正在获取 IP 并确认保存，请稍候…':
      '正在尝试连接路由器，尚未确认关联、IP 地址或保存结果…');
  }else if(data.associated&&data.ipv4_ready&&
      /^(?:\d{1,3}\.){3}\d{1,3}$/.test(data.ipv4)&&
      data.ipv4.split('.').every(part=>Number(part)<=255)&&data.ipv4!=='0.0.0.0'){
    networkWatching=false;networkRequested=false;
    networkSay(networkUncertain?'当前路由器已连接 · IP：'+data.ipv4+
      '。本次请求回执未收到，请在设备上核对新网络和保存结果，不要重复提交。':
      '路由器连接成功 · IP：'+data.ipv4+'。设备热点会保持开启，完成后请在设备上关闭。',networkUncertain);
  }else{
    networkWatching=true;
    networkSay(data.associated?'路由器已关联，正在等待可用 IP 地址…':networkRequested?
      '连接请求已受理，等待设备确认关联、IP 地址和保存结果…':
      '尚未连接路由器。填写路由器网络名称和密码后，点击「连接路由器」。');
  }
  networkControls();
}
async function refreshNetwork(force=false){
  if(document.hidden||!token||loadedToken!==token||connecting||networkReading||
      networkSubmitting||(!networkWatching&&!force))return;
  const epoch=networkEpoch,attempt=connectionAttempt,bearer=token;
  networkReading=true;networkControls();
  try{
    const data=await(await api('network')).json();
    if(networkCurrent(epoch,attempt,bearer))networkSnapshot(data);
  }catch(e){
    if(networkCurrent(epoch,attempt,bearer))networkSay(
      '暂时无法读取网络状态。请保持设备热点连接后查询；连接动作不会自动重发。'+e.message,true);
  }finally{
    if(networkCurrent(epoch,attempt,bearer)){networkReading=false;networkControls();}
  }
}
async function submitNetwork(){
  if(networkSubmitting||networkBusy||!token||loadedToken!==token||connecting)return;
  const ssid=$('network-ssid').value,password=$('network-password').value;
  try{validateNetwork(ssid,password);}catch(e){$('network-password').value='';networkSay(e.message,true);return;}
  const attempt=connectionAttempt,bearer=token;
  // Invalidate a status read which started before this new connection request.
  networkEpoch++;const submitEpoch=networkEpoch;networkReading=false;
  networkSubmitting=networkBusy=true;networkRequested=networkWatching=true;
  networkUncertain=false;networkControls();
  networkSay('正在提交连接请求，提交成功只代表设备已受理…');
  try{
    const data=await(await api('network/connect','POST',{ssid,password})).json();
    if(!networkCurrent(submitEpoch,attempt,bearer))return;
    if(data?.ok!==true)throw Error('设备未确认受理连接请求，请查询状态后再决定是否重试。');
    networkSay('连接请求已受理，正在等待路由器关联、IP 地址和保存结果…');
  }catch(e){
    if(networkCurrent(submitEpoch,attempt,bearer)){
      networkUncertain=!e.status;
      if(e.status){networkBusy=false;networkRequested=false;}
      networkSay(e.message,true);
    }
  }finally{
    if(networkCurrent(submitEpoch,attempt,bearer)){
      $('network-password').value='';networkSubmitting=false;networkControls();
    }
  }
}
$('network').onsubmit=e=>{e.preventDefault();return submitNetwork();};
$('network-refresh').onclick=()=>refreshNetwork(true);
setInterval(()=>refreshNetwork(),2000);
window.addEventListener('pagehide',()=>{$('network-password').value='';});
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
$('install-button').onclick=()=>run(async()=>{if(!pack||busy)return;busy=true;const button=$('install-button'),input=$('package');button.disabled=input.disabled=true;$('progress').hidden=false;$('progress').value=0;try{const storage=$('install-storage').value;await api('install/begin','POST',{...pack.manifest,storage});const entries=Object.entries(pack.files).filter(([name])=>name!=='manifest.json');let n=0;for(const [name,data]of entries){$('install-status').textContent='正在传输 '+name;await api('install/file?path='+encodeURIComponent(name),'PUT',new Blob([data]));$('progress').value=++n/entries.length*95;}await api('install/commit','POST',{});$('progress').value=100;$('install-status').textContent='已安装到'+(storage==='sdcard'?'SD 卡':'内置存储')+'！重新打开应用列表即可找到它。';pack=null;$('preview').hidden=true;say('快应用安装完成');}catch(e){try{await api('install/abort','POST',{});}catch{}$('install-status').textContent='安装未完成，可重新尝试。';throw e;}finally{busy=false;button.disabled=input.disabled=false;}});
const initialCode=scannedCode();
if(initialCode||token)connect(initialCode);else connectionView(false);
