'use strict';
// No cookies, URL credentials or browser storage. Uploaded scripts are never executed here.
const el = id => document.getElementById(id);
const bytes = text => new TextEncoder().encode(text).length;
const categories = ['实用工具', '专注效率', '轻松趣味'];
const builtins = [
  {package:'store.calculator',name:'轻量计算器',version:'1.0.0',icon:'calculator',category:0,summary:'四则运算、小数与正负数。无需联网，随开随用。'},
  {package:'store.focus',name:'专注计时',version:'1.0.0',icon:'timer',category:1,summary:'25 分钟专注与 5 分钟休息。支持暂停和继续，退出后停止。'},
  {package:'store.counter',name:'随手计数',version:'1.0.0',icon:'counter',category:0,summary:'记录次数、加减与归零。手动保存计数，重开继续。'}
];
const state = {connected:false,busy:false,releases:[],apps:[],home:{headline:'',tagline:'',featured:[]},homeDirty:false,homeJsonError:'',view:'home',selected:null};
function node(tag, className='', text) {
  const item = document.createElement(tag);if(className)item.className=className;
  if(text!==undefined)item.textContent=text;return item;
}
function action(text, className, handler) {
  const button=node('button',className,text);button.type='button';button.addEventListener('click',handler);return button;
}
function status(message,error=false) {el('status').textContent=message;el('status').classList.toggle('error',error);}
function updateControls() {
  document.querySelectorAll('[data-admin]').forEach(button=>button.disabled=!state.connected||state.busy);
  el('connect').disabled=state.busy;
  el('connection-dot').classList.toggle('connected',state.connected);
  el('connection-label').textContent=state.connected?'管理员已连接':'等待管理员连接';
  el('auth-state').textContent=state.connected?'已连接 · 可发布与编辑':'未连接';
  el('add-feature').disabled=state.home.featured.length>=2;
}
async function request(path,options={}) {
  const response=await fetch(new URL(path,location.href),{...options,cache:'no-store',signal:AbortSignal.timeout(20000)});
  if(!response.ok) {
    if(response.status===401||response.status===503){state.connected=false;updateControls();}
    let detail;try{detail=(await response.json()).detail;}catch{detail=null;}
    if(response.status===401)throw Error('管理员令牌不正确，请重新连接');
    if(response.status===503)throw Error('服务端尚未启用管理员发布');
    throw Error(typeof detail==='string'?detail:detail?detail.map(item=>item.loc.join('.')+'：'+item.msg).join('；'):'请求失败（'+response.status+'）');
  }
  return response;
}
async function api(path,options={}) {
  const token=el('token').value;if(!token)throw Error('请先输入管理员令牌');
  return (await request('v1/admin/'+path,{...options,headers:{'Content-Type':'application/json',Authorization:'Bearer '+token}})).json();
}
async function run(work) {
  if(state.busy)return;state.busy=true;updateControls();
  try{await work();}catch(error){status(error.name==='TimeoutError'?'连接超时，请稍后重试':error.message,true);}
  finally{state.busy=false;updateControls();}
}
function homeFields(home) {
  return {headline:home.headline,tagline:home.tagline,featured:home.featured.map(feature=>({...feature}))};
}
function applyCatalog(home,apps) {
  state.apps=apps;
  el('stat-apps').textContent=apps.length;
  el('stat-features').textContent=home.featured.length;
  if(!state.homeDirty) {state.home=homeFields(home);state.homeJsonError='';syncHomeFields();}
  else renderFeatureEditors();
  validateHome();renderPreview();
}
async function refresh() {
  const data=await api('apps');state.connected=true;state.releases=data.releases;
  applyCatalog(data.home,data.releases.filter(app=>app.current&&app.active));
  el('stat-versions').textContent=data.releases.length;el('versions-note').hidden=true;
  el('library-count').textContent=data.releases.length;
  el('auth-panel').open=false;renderLibrary();updateControls();
  status('管理员已连接 · '+state.apps.length+' 个上架应用 · '+data.releases.length+' 个版本'+(state.homeDirty?' · 首页草稿已保留':''));
}
async function loadPublic() {
  try{
    const data=await (await request('v1/catalog')).json();
    if(!state.connected){applyCatalog(data,data.apps);status('已载入公开目录。连接管理员后，即可发布与保存。');}
  }catch(error){if(!state.connected)status('目录加载失败：'+error.message,true);}
}
function icon(kind) {
  const paths={calculator:['M6 3h12v18H6z','M9 6h6','M9 10h1m4 0h1m-6 4h1m4 0h1m-6 4h1m4 0h1'],timer:['M9 2h6M12 2v3M18 5l2 2','M20 13a8 8 0 1 1-16 0 8 8 0 0 1 16 0','M12 9v4l3 2'],counter:['M5 5h14v14H5z','M8 12h8M12 8v8'],grid:['M4 4h6v6H4zM14 4h6v6h-6zM4 14h6v6H4zM14 14h6v6h-6z']};
  const valid=Object.hasOwn(paths,kind)?kind:'grid',wrap=node('span','app-icon '+valid);wrap.setAttribute('aria-hidden','true');
  const svg=document.createElementNS('http://www.w3.org/2000/svg','svg');svg.setAttribute('viewBox','0 0 24 24');
  for(const d of paths[valid]){const path=document.createElementNS(svg.namespaceURI,'path');path.setAttribute('d',d);svg.append(path);}
  wrap.append(svg);return wrap;
}
function draft() {
  const form=el('publish');
  return {package:form.elements.package.value||'store.your_app',name:form.elements.name.value||'你的下一个好应用',version:form.elements.version.value||'1.0.0',summary:form.elements.summary.value||'写下一句话介绍，让用户一眼看懂它的用处。',category:Number(form.elements.category.value),icon:form.elements.icon.value,bytes:bytes(form.elements.source.value)};
}
function setView(view,selected=null) {state.view=view;state.selected=selected;renderPreview();}
function renderPreview() {
  const content=el('preview-content');content.replaceChildren();
  el('preview-home').setAttribute('aria-pressed',String(state.view==='home'));
  el('preview-app').setAttribute('aria-pressed',String(state.view==='app'));
  el('preview-state').textContent=state.view==='app'?(state.selected?'已发布应用 · 外观预览':'应用草稿 · 尚未发布'):(state.homeDirty?'首页草稿 · 尚未保存':'已发布目录');
  if(state.view==='app'){renderDetail(content,state.selected||draft());return;}
  const home=state.home,heading=node('div','device-heading');heading.append(node('span','edition','CURATED FOR YOUR DESKTOP'),node('h3','',home.headline||'让日常，轻松一点。'),node('p','',home.tagline||'小而精的应用，恰好满足你的需要。'));content.append(heading);
  const posters=node('div','device-posters'+(home.featured.length===1?' single':''));
  for(const feature of home.featured) {
    const app=[...state.apps,...builtins].find(item=>item.package===feature.package);
    const tone=['forest','sand','lavender'].includes(feature.tone)?feature.tone:'forest';
    const poster=action('', 'poster tone-'+tone,()=>{if(app)setView('app',app);});
    poster.setAttribute('aria-label','预览推荐：'+(feature.title||'未命名推荐').replaceAll('\n',''));poster.disabled=!app;
    const copy=node('div','poster-copy');copy.append(node('span','','EDITOR’S PICK'),node('h4','',feature.title||'写下推荐标题'),node('p','',feature.subtitle||''),node('b','',(app?.name||'选择推荐应用')+' ↗'));poster.append(copy);
    if(feature.art==='timer') {const art=node('div','poster-art timer');art.setAttribute('aria-hidden','true');art.append(node('strong','',feature.value||'25'),node('span','',feature.unit||'MINUTES'));poster.append(art);}
    else if(feature.art==='bars') {const art=node('div','poster-art bars');art.setAttribute('aria-hidden','true');art.append(node('i'),node('i'),node('i'));poster.append(art);}
    posters.append(poster);
  }
  if(home.featured.length)content.append(posters);
  const title=node('h4','device-section-title','发现小而美');title.append(node('span','',state.apps.length+' 个云端应用'));content.append(title);
  const apps=node('div','device-apps');
  for(const app of state.apps) {const card=action('','device-app',()=>setView('app',app));card.setAttribute('aria-label','预览应用：'+app.name);card.append(icon(app.icon),node('strong','',app.name),node('p','',app.summary));apps.append(card);}
  if(!state.apps.length)apps.append(node('p','detail-description','还没有上架应用，发布后会出现在这里。'));
  content.append(apps);
}
function renderDetail(content,app) {
  content.append(action('← 返回精选','preview-back',()=>setView('home')));
  const detail=node('article','device-detail'),hero=node('div','detail-hero'),titles=node('div');
  titles.append(node('h3','',app.name),node('p','',categories[app.category]||categories[0]));hero.append(icon(app.icon),titles);
  const meta=node('div','detail-metadata');
  for(const [label,value] of [['版本',app.version],['大小',app.bytes===undefined?'内置应用':(app.bytes/1024).toFixed(1)+' KiB'],['应用类型','轻量脚本']]){const item=node('div','',label);item.append(node('strong','',value));meta.append(item);}
  detail.append(hero,node('p','detail-description',app.summary),meta,node('p','detail-package',app.package),node('span','preview-badge',state.selected?'目录详情预览':'应用草稿 · 发布后加入目录'));content.append(detail);
}
function selectEditor(home) {
  for(const [id,active] of [['app',!home],['home',home]]){el('tab-'+id).setAttribute('aria-selected',String(active));el('tab-'+id).tabIndex=active?0:-1;el(id+'-editor').hidden=!active;}
  setView(home?'home':'app');
}
function field(label,value,change,choices) {
  const wrap=node('label','',label),input=node(choices?'select':'input');
  if(choices)for(const [value,text] of choices){const option=node('option','',text);option.value=value;input.append(option);}
  input.value=value;input.addEventListener(choices?'change':'input',()=>change(input.value));wrap.append(input);return wrap;
}
function homeChanged(rebuild=false) {
  state.homeDirty=true;state.homeJsonError='';el('home').value=JSON.stringify(state.home,null,2);
  if(rebuild)renderFeatureEditors();validateHome();setView('home');updateControls();
}
function syncHomeFields() {
  el('home-headline').value=state.home.headline;el('home-tagline').value=state.home.tagline;
  el('home').value=JSON.stringify(state.home,null,2);renderFeatureEditors();
}
function renderFeatureEditors() {
  const list=el('feature-editors');list.replaceChildren();
  if(!state.home.featured.length)list.append(node('p','muted small','还没有推荐海报。添加一张，或保留简洁的应用目录。'));
  state.home.featured.forEach((feature,index)=>{
    const item=node('section','feature-editor'),head=node('div','row'),actions=node('div');head.append(node('strong','','精选海报 '+String(index+1).padStart(2,'0')));
    if(index)actions.append(action('↑ 上移','text-button',()=>{[state.home.featured[index-1],state.home.featured[index]]=[feature,state.home.featured[index-1]];homeChanged(true);}));
    actions.append(action('移除','text-button',()=>{state.home.featured.splice(index,1);homeChanged(true);}));head.append(actions);item.append(head);
    const available=[...builtins,...state.apps];
    const choices=available.map(app=>[app.package,app.name+' · '+app.package]);
    if(!available.some(app=>app.package===feature.package))choices.push([feature.package,'不可用：'+feature.package]);
    const edit=(key,label,options)=>field(label,feature[key]||'',value=>{feature[key]=value;homeChanged();},options);
    item.append(edit('package','推荐应用',choices));
    const titleLabel=node('label','','海报标题（最多两行）'),title=node('textarea');title.rows=2;title.value=feature.title;title.addEventListener('input',()=>{feature.title=title.value;homeChanged();});titleLabel.append(title);item.append(titleLabel,edit('subtitle','海报副标题'));
    const pair=node('div','pair');pair.append(edit('tone','海报配色',[['forest','森林绿'],['sand','暖沙色'],['lavender','浅薰衣草']]),edit('art','装饰图案',[['timer','计时圆环'],['bars','计数立柱'],['none','纯文字']]));item.append(pair);
    const values=node('div','pair');values.append(edit('value','展示数字'),edit('unit','数字单位'));item.append(values);list.append(item);
  });updateControls();
}
function parseHome(raw) {
  const home=JSON.parse(raw);
  if(!home||typeof home!=='object'||Array.isArray(home)||Object.keys(home).some(key=>!['headline','tagline','featured'].includes(key))||typeof home.headline!=='string'||typeof home.tagline!=='string'||!Array.isArray(home.featured)||home.featured.length>2)throw Error('首页需包含 headline、tagline 和最多两项 featured');
  for(const feature of home.featured) {
    if(!feature||typeof feature!=='object'||Array.isArray(feature)||Object.keys(feature).some(key=>!['package','title','subtitle','tone','art','value','unit'].includes(key))||typeof feature.package!=='string'||typeof feature.title!=='string')throw Error('推荐需包含 package 和 title，且只能使用支持的配置字段');
    for(const key of ['subtitle','tone','art','value','unit'])if(feature[key]!==undefined&&typeof feature[key]!=='string')throw Error('推荐字段 '+key+' 必须是文字');
    feature.art??='none';feature.tone??='forest';
  }
  return home;
}
function validateHome() {
  let error=state.homeJsonError;
  const check=(text,max,label,empty=false,multiline=false)=>{
    if((!empty&&!text.trim())||bytes(text)>max)throw Error(label+'需'+(empty?'不超过':'为 1–')+max+' UTF-8 字节');
    if([...text].some(char=>(char.charCodeAt(0)<32&&!(multiline&&char==='\n'))||char.charCodeAt(0)===127))throw Error(label+'包含不支持的控制字符');
    if(multiline&&text.split('\n').length>2)throw Error(label+'最多两行');
  };
  if(!error)try {
    check(state.home.headline,95,'首页标题');check(state.home.tagline,127,'首页副标题');
    const seen=new Set(),known=new Set([...builtins,...state.apps].map(app=>app.package));
    for(const feature of state.home.featured) {
      if(!known.has(feature.package))throw Error('请选择已上架或内置的推荐应用');
      if(seen.has(feature.package))throw Error('两张海报不能推荐同一个应用');seen.add(feature.package);
      if(!['forest','sand','lavender'].includes(feature.tone)||!['timer','bars','none'].includes(feature.art))throw Error('海报样式不受支持');
      check(feature.title,95,'海报标题',false,true);check(feature.subtitle||'',127,'海报副标题',true);check(feature.value||'',23,'展示数字',true);check(feature.unit||'',23,'数字单位',true);
    }
  }catch(problem){error=problem.message;}
  el('home-error').textContent=error;el('home').setAttribute('aria-invalid',String(Boolean(error)));return !error;
}
function renderLibrary() {
  const list=el('apps');list.replaceChildren();
  const query=el('app-search').value.toLowerCase().trim();
  const visible=state.releases.filter(app=>[app.name,app.package,app.version].some(text=>text.toLowerCase().includes(query)));
  if(!visible.length){const empty=node('div','empty-state');empty.append(node('span','','▦'),node('h3','',query?'没有找到匹配的版本':'你的应用，从这里出发'),node('p','',query?'试试其他名称、包名或版本号。':state.connected?'发布第一个应用，为桌面带来新可能。':'连接管理员后，查看与编辑所有发布版本。'));list.append(empty);return;}
  for(const app of visible) {
    const row=node('article','release'),body=node('div','release-content'),heading=node('div','release-heading');heading.append(node('h3','',app.name),node('span','version','v'+app.version),node('span','tag'+(app.active?'':' inactive'),app.current?(app.active?'上架中':'已下架'):'历史版本'));
    const details=node('details'),date=new Date(app.created);details.append(node('summary','','版本信息与校验摘要'),node('code','','SHA-256 '+app.sha256),node('p','',Number.isNaN(date.getTime())?'':date.toLocaleString('zh-CN')));
    body.append(heading,node('p','',app.summary),node('p','',app.package+' · '+(app.bytes/1024).toFixed(1)+' KiB'),details);
    const actions=node('div','release-actions');actions.append(action('预览','',()=>{setView('app',app);el('preview-frame').scrollIntoView({behavior:'smooth',block:'center'});}),action('编辑新版本','',()=>run(()=>editRelease(app))));
    if(app.current)actions.append(action(app.active?'下架':'重新上架',app.active?'danger':'',()=>run(async()=>{
      if(app.active&&!confirm('将 '+app.name+' 从公开目录下架？已发布的版本文件仍保留。'))return;
      await api('apps/'+encodeURIComponent(app.package)+(app.active?'':'/restore'),{method:app.active?'DELETE':'POST'});await refresh();status(app.active?'应用已下架；原版本仍保留':'应用已重新上架');
    })));
    row.append(icon(app.icon),body,actions);list.append(row);
  }
}
async function editRelease(app) {
  if(el('publish').elements.source.value&&!confirm('载入已有版本会替换当前应用草稿，是否继续？'))return;
  const source=await (await request('v1/blobs/'+app.sha256+'/app.js')).text(),form=el('publish');
  for(const key of ['package','name','summary','category','icon'])form.elements[key].value=app[key];
  const version=/^(\d+)\.(\d+)\.(\d+)$/.exec(app.version);form.elements.version.value=version?version[1]+'.'+version[2]+'.'+(Number(version[3])+1):'';
  form.elements.source.value=source;el('file').value='';el('file-label').textContent='已载入 '+app.name+' 的源码';updateSourceSize();selectEditor(false);
  el('app-editor').scrollIntoView({behavior:'smooth',block:'start'});form.elements.name.focus({preventScroll:true});status('已载入 '+app.name+' '+app.version+'；修改后按新版本发布，原版本保留');
}
function updateSourceSize() {el('source-size').textContent=(bytes(el('publish').elements.source.value)/1024).toFixed(1)+' / 64 KiB';}
el('connect').addEventListener('click',()=>run(refresh));el('refresh').addEventListener('click',()=>run(refresh));
el('token').addEventListener('input',()=>{state.connected=false;updateControls();});
el('token').addEventListener('keydown',event=>{if(event.key==='Enter')run(refresh);});
el('file').addEventListener('change',()=>run(async()=>{
  const file=el('file').files[0];if(!file)return;if(file.size>65536)throw Error('app.js 超过 64 KiB');
  const source=await file.text();if(bytes(source)>65536||source.includes('\0'))throw Error('源码需为不含 NUL 的 UTF-8 文字，最大 64 KiB');
  el('publish').elements.source.value=source;el('file-label').textContent=file.name;updateSourceSize();setView('app');status('已载入源码，请核对应用信息后发布');
}));
el('publish').addEventListener('input',()=>{updateSourceSize();setView('app');});
el('publish').addEventListener('change',()=>setView('app'));
el('publish').addEventListener('submit',event=>{
  event.preventDefault();run(async()=>{
    const values=Object.fromEntries(new FormData(event.target));values.category=Number(values.category);
    if(bytes(values.source)>65536||values.source.includes('\0'))throw Error('源码需为不含 NUL 的 UTF-8 文字，最大 64 KiB');
    const result=await api('apps',{method:'POST',body:JSON.stringify(values)});await refresh();setView('app',result.application);status(result.already_published?'版本已存在，上架状态保持原样':'已发布 '+result.application.name+' '+result.application.version);
  });
});
el('save-home').addEventListener('click',()=>run(async()=>{
  if(!validateHome())throw Error(el('home-error').textContent);
  // Snapshot the submitted draft: edits made during the request must not be discarded.
  const snapshot=JSON.stringify(state.home),raw=el('home').value,result=await api('home',{method:'PUT',body:snapshot});
  if(JSON.stringify(state.home)===snapshot&&el('home').value===raw&&!state.homeJsonError){state.homeDirty=false;state.home=homeFields(result.home);syncHomeFields();}
  el('stat-features').textContent=result.home.featured.length;renderPreview();status(state.homeDirty?'首页已保存；后续修改仍是未保存草稿':'首页配置已保存；设备刷新后获取');
}));
for(const key of ['headline','tagline'])el('home-'+key).addEventListener('input',event=>{state.home[key]=event.target.value;homeChanged();});
el('home').addEventListener('input',()=>{
  state.homeDirty=true;
  try {const home=parseHome(el('home').value);state.home=home;state.homeJsonError='';el('home-headline').value=home.headline;el('home-tagline').value=home.tagline;renderFeatureEditors();setView('home');}
  catch(error){state.homeJsonError='JSON 配置无效：'+error.message+'。预览保留上一次可用配置。';}
  validateHome();el('preview-state').textContent=state.homeJsonError?'JSON 无效 · 保留上次预览':'首页草稿 · 尚未保存';
});
el('add-feature').addEventListener('click',()=>{
  if(state.home.featured.length>=2)return;
  const next=[...state.apps,...builtins].find(app=>!state.home.featured.some(feature=>feature.package===app.package));if(!next)return;
  state.home.featured.push({package:next.package,title:next.name,subtitle:next.summary,art:'none',tone:'forest',value:'',unit:''});homeChanged(true);
});
el('tab-app').addEventListener('click',()=>selectEditor(false));el('tab-home').addEventListener('click',()=>selectEditor(true));
el('nav-home').addEventListener('click',()=>{selectEditor(true);el('home-editor').scrollIntoView({behavior:'smooth',block:'start'});});
document.querySelector('.editor-tabs').addEventListener('keydown',event=>{if(['ArrowLeft','ArrowRight','Home','End'].includes(event.key)){event.preventDefault();const home=event.key==='End'||(event.key!=='Home'&&el('tab-app').getAttribute('aria-selected')==='true');selectEditor(home);el(home?'tab-home':'tab-app').focus();}});
el('preview-home').addEventListener('click',()=>setView('home'));el('preview-app').addEventListener('click',()=>setView('app'));
el('preview-theme').addEventListener('click',()=>{const dark=el('preview-frame').classList.toggle('dark');el('preview-theme').setAttribute('aria-pressed',String(dark));el('preview-theme').setAttribute('aria-label',dark?'切换为浅色预览':'切换为深色预览');el('preview-theme').querySelector('span').textContent=dark?'深色':'浅色';});
el('expand-preview').addEventListener('click',()=>{el('large-preview-slot').append(el('preview-frame'));el('preview-dialog').showModal();});
el('close-preview').addEventListener('click',()=>el('preview-dialog').close());
el('preview-dialog').addEventListener('close',()=>{el('preview-slot').append(el('preview-frame'));el('expand-preview').focus({preventScroll:true});});
el('app-search').addEventListener('input',renderLibrary);
updateControls();renderPreview();loadPublic();
