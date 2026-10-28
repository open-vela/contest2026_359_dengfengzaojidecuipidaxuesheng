'use strict';

const tr = value => typeof system.translate === 'function' ? system.translate(value) : value;


const view = ui.getSize();
const rowCount = 3;
const sx = v => Math.round(v * view.width / 1024);
const sy = v => Math.round(v * view.height / 600);
const colors = {
  page: 0x000000, card: 0x202022,
  primary: 0xf5f5f7, secondary: 0x9ba0a8,
  accent: 0xf13c2e, onAccent: 0xffffff,
  border: 0x2b2e33, selected: 0x24272c, success: 0x74d5a0
};
const labels = new Map(), enabled = new Map();
const hitTargets = new Set();
let devices = [], selected = null, modes = [], modeIndex = 0, intervalIndex = 0;
let pending = false, running = false, requestAt = 0, message = tr('请选择摄像头后开始预览');
let signature = '', intervals = [], photoBaseline = '';
let devicePage = 0, lastScan = -Infinity;
let settingsOpen = false, resumeAfterSelect = false, colorStyle = 0;
let filtersOpen=false;
let aspectRatio=typeof system.camera.aspect==='function'?system.camera.aspect():0;
const aspectNames=[tr('原始'),'16:10','16:9','4:3','1:1'];
const filterBounds={x:772,y:116,w:56,h:296};
const filterNames=[tr('原色'),tr('鲜艳'),tr('黑白'),tr('暖色'),tr('冷色'),tr('复古')];
const filterWidgets=[];
const menuBounds={x:800,y:24,w:176,h:552};
let settingsTouch=null,dismissTap=false;
let controlSignature = "";
let shutterReady = false;
let videoMode = false, recording = {status: 0, revision: 0};
let statusVisible = false;
const hasVideo = typeof system.camera.recordStatus === 'function' && typeof system.camera.setVideoMode === 'function';
const settingsWidgets = [];
function part(id) { settingsWidgets.push(id); ui.setHidden(id, true); return id; }
function setSettingsOpen(open) {
  if(open&&filtersOpen)setFiltersOpen(false);
  settingsOpen = !!open;
  zoomGesture = null;
  settingsTouch=null;dismissTap=false;
  settingsWidgets.forEach(id => ui.setHidden(id, !settingsOpen));
  standbyWidgets.forEach(id => ui.setHidden(id, settingsOpen));
  renderRows();renderMode();
}

function write(id, value) {
  value = String(value);
  if (labels.get(id) !== value) { ui.setText(id, value); labels.set(id, value); }
}
function label(value, x, y, w, h, font, color, center) {
  const id = ui.text(value, sx(x), sy(y), font, color);
  ui.setSize(id, sx(w), sy(h)); ui.setStyle(id, {center: !!center, ellipsis: true});
  labels.set(id, value); return id;
}
function card(x, y, w, h, radius, fill) {
  const id = ui.panel(sx(x), sy(y), sx(w), sy(h), fill === undefined ? colors.card : fill);
  ui.setStyle(id, {radius: radius, borderColor: colors.border, borderWidth: 1, borderOpacity: 100});
  return id;
}
function button(value, x, y, w, h, action, primary) {
  const id = ui.button(value, sx(x), sy(y), sx(w), sy(h), action, primary ? colors.accent : colors.card);
  ui.setStyle(id, {radius: 12, borderColor: colors.border, borderWidth: primary ? 0 : 1,
    borderOpacity: 110, textColor: primary ? colors.onAccent : colors.primary, fontSize: 16});
  labels.set(id, value); return id;
}
function enable(id, value) {
  if (enabled.get(id) !== value) {
    ui.setStyle(id, {enabled: value});
    if(!hitTargets.has(id))ui.setOpacity(id,value?255:100);
    enabled.set(id, value);
  }
}

ui.background(0x000000);
// Shared read-only tiles keep the idle page and native live toolbar consistent.
const cameraScene=ui.cameraReference();
const zoomValues=[100,150,200,300];
let zoomPercent=100,zoomGesture=null;
function applyZoom(index){
  index=Math.max(0,Math.min(3,index));
  const percent=zoomValues[index];
  if(typeof system.camera.setZoom==='function'&&!system.camera.setZoom(percent))return;
  zoomPercent=percent;ui.cameraReferenceZoom(cameraScene,percent);
  syncControls();
}
function syncZoom(){
  const percent=typeof system.camera.zoom==='function'?system.camera.zoom():zoomPercent;
  if(zoomValues.includes(percent)&&percent!==zoomPercent){zoomPercent=percent;ui.cameraReferenceZoom(cameraScene,percent);syncControls();}
}
ui.onTouch((state,nx,ny)=>{
  const x=nx*1024,y=ny*600;
  if(settingsOpen||filtersOpen){
    zoomGesture=null;
    if(state==='down'){
      const b=filtersOpen?filterBounds:menuBounds;
      settingsTouch={x,y,outside:x<b.x||x>=b.x+b.w||y<b.y||y>=b.y+b.h,moved:false};
      dismissTap=false;
    }
    if(settingsTouch&&Math.hypot(x-settingsTouch.x,y-settingsTouch.y)>12)settingsTouch.moved=true;
    if(state==='up'){dismissTap=!!settingsTouch&&settingsTouch.outside&&!settingsTouch.moved;settingsTouch=null;}
    if(state==='cancel'){settingsTouch=null;dismissTap=false;}
    return;
  }
  if(state==='down'){
    zoomGesture=x>=700&&x<764&&y>=160&&y<440?{y,index:zoomValues.indexOf(zoomPercent)}:null;
    return;
  }
  if(!zoomGesture)return;
  if(state==='cancel'||x<680||x>=784){zoomGesture=null;return;}
  const delta=y-zoomGesture.y;
  if(state==='move')applyZoom(zoomGesture.index+Math.trunc(-delta/48));
  if(state==='up'){
    applyZoom(Math.abs(delta)<10?3-Math.trunc((y-172)/64):zoomGesture.index+Math.trunc(-delta/48));
    zoomGesture=null;
  }
});
syncZoom();
function hit(value,x,y,w,h,action) {
  const id=button(value,x,y,w,h,action,false);
  hitTargets.add(id);ui.setOpacity(id,0);return id;
}
const deviceName=label(tr('尚未选择设备'),140,22,344,22,14,colors.secondary);
const captureInfo=label('',488,24,196,20,12,colors.secondary);
ui.setStyle(captureInfo,{rightAlign:true});
const previewTitle=label(tr('未连接摄像头'),24,284,672,28,16,colors.secondary,true);
const previewHint=label(tr('连接 CSI / USB 摄像头'),24,318,672,24,14,colors.secondary,true);
const status=label(message,140,542,544,28,12,colors.secondary);
const standbyWidgets=[deviceName,captureInfo,previewTitle,previewHint,status];
function syncControls(){
  const next=typeof system.camera.style==='function'?system.camera.style():0;
  if(Number.isInteger(next)&&next>=0&&next<filterNames.length)colorStyle=next;
  const csi=!selected||selected.transport==='csi',mode=modes[modeIndex];
  const width=csi?1024:mode?mode.width:896,height=csi?600:mode?mode.height:600,canvas=csi?1024:896;
  if(typeof system.camera.aspect==='function')aspectRatio=system.camera.aspect();
  const signature=[colorStyle,videoMode,shutterReady,settingsOpen,filtersOpen,width,height,canvas,zoomPercent,aspectRatio].join(':');
  if(signature!==controlSignature){ui.cameraReferenceControls(cameraScene,colorStyle,videoMode,shutterReady,settingsOpen,filtersOpen,width,height,canvas,zoomPercent,aspectRatio);controlSignature=signature;}
}
const deviceButton=hit(tr('切换相机'),856,24,78,72,()=>settingsOpen?closeSettings():setSettingsOpen(true));
const colorButton=hit(tr('色彩风格'),700,28,64,90,()=>setFiltersOpen(!filtersOpen));
const albumButton=hit(tr('预览相册'),856,504,78,78,()=>{
  pending=false;running=false;resumeAfterSelect=false;setSettingsOpen(false);
  if(typeof system.camera.openAlbum!=='function'||!system.camera.openAlbum())setStatus(tr('相册暂时无法打开'));
});
const captureMode=hit(tr('切换录制'),836,128,112,80,toggleVideo);
const startButton=hit(tr('开始预览'),836,244,112,112,start);
// The thin system-return edge is retained in the landscape translation.
const returnEdge=ui.panel(sx(1007),sy(234),Math.max(2,sx(3)),sy(132),0x999999);
ui.setStyle(returnEdge,{radius:2,borderWidth:0});

// A transparent input shield dismisses on an outside tap without click-through.
const shade=part(hit(tr('收起设置'),0,0,988,600,()=>{if(dismissTap)closeSettings();}));
const choices=part(card(800,24,176,204,0,colors.page));
ui.setStyle(choices,{borderWidth:0});
const icons={camera:'\ue002',usb:'\ue029',left:'\uf053',right:'\uf054',
  frame:'\uf03e',rate:'\uf079',refresh:'\uf021'};
function glyph(value,x,y,color=colors.secondary){
  const id=part(label(value,x+12,y+10,24,30,24,color,true));
  ui.setStyle(id,{icon:1});return id;
}
function iconAction(name,icon,x,y,action,w=48,h=48){
  return {hit:part(hit(name,x,y,w,h,action)),icon:glyph(icon,x,y)};
}
function dots(x,y){
  return Array.from({length:5},(_,i)=>{
    const id=part(card(x+i*12,y,4,4,2,colors.border));
    ui.setStyle(id,{borderWidth:0});return id;
  });
}
function showDots(group,count,index,on){
  const shown=Math.min(5,count),start=Math.max(0,Math.min(count-shown,index-2));
  group.forEach((id,i)=>{
    ui.setHidden(id,!on||i>=shown);
    if(i<shown)ui.setColor(id,start+i===index?colors.accent:colors.border);
  });
}
const done=iconAction(tr('完成'),icons.right,868,36,closeSettings);
const doneButton=done.hit;
const rows=[],rowMarkers=[],rowIcons=[],rowWells=[];
for(let i=0;i<rowCount;i++){
  const x=812+i*52;
  const well=part(card(x+2,118,44,44,22,colors.selected));
  ui.setStyle(well,{borderWidth:0});rowWells.push(well);
  const row=part(hit('',x,116,48,56,()=>{
    if(select(devicePage*rowCount+i))closeSettings();
  }));
  rows.push(row);
  rowIcons.push(glyph(icons.camera,x,116,colors.primary));
  const marker=part(card(x+22,170,4,4,2,colors.accent));
  ui.setStyle(marker,{borderWidth:0});rowMarkers.push(marker);
}
const emptyIcon=rowIcons[0];
const previous=iconAction(tr('上一页'),icons.left,812,184,()=>turnPage(-1));
const next=iconAction(tr('下一页'),icons.right,916,184,()=>turnPage(1));
const previousButton=previous.hit,nextButton=next.hit;
const pageDots=dots(860,206);
const resolution=iconAction(tr('分辨率'),icons.frame,820,384,()=>{
  if(pending||running||!modes.length)return;
  modeIndex=(modeIndex+1)%modes.length;chooseIntervals();
},144,56);
const rate=iconAction(tr('帧率'),icons.rate,820,448,()=>{
  if(pending||running||!intervals.length)return;
  intervalIndex=(intervalIndex+1)%intervals.length;renderMode();
},144,56);
const modeButton=resolution.hit,fpsButton=rate.hit;
const modeDots=dots(884,408),fpsDots=dots(884,472);
const refreshControl=iconAction(tr('刷新设备'),icons.refresh,868,516,()=>refresh(true));
const refreshButton=refreshControl.hit;
const menuShutter=part(hit(tr('菜单快门'),836,244,112,112,()=>{
  resumeAfterSelect=false;start();
}));

function filterPart(id){filterWidgets.push(id);ui.setHidden(id,true);return id;}
filterPart(hit(tr('收起滤镜'),0,0,988,600,()=>{if(dismissTap)setFiltersOpen(false);}));
filterPart(hit(tr('滤镜开关'),700,28,64,90,()=>setFiltersOpen(false)));
const filterShutter=filterPart(hit(tr('滤镜快门'),836,244,112,112,()=>{setFiltersOpen(false);start();}));
const filterChoices=filterNames.map((name,i)=>
  filterPart(hit(name,780,124+i*48,40,40,()=>chooseFilter(i))));
const aspectButton=hit(tr('画幅比例'),800,504,56,78,()=>{
  const next=aspectRatio%4+1;
  if(typeof system.camera.setAspect==='function'&&system.camera.setAspect(next)){
    aspectRatio=next;syncControls();setStatus(tr('画幅：')+aspectNames[next]);
  }
});

function setFiltersOpen(open){
  if(open&&(settingsOpen||pending||running||system.camera.status().busy||recordingBusy()))return;
  filtersOpen=!!open;zoomGesture=null;settingsTouch=null;dismissTap=false;
  filterWidgets.forEach(id=>ui.setHidden(id,!filtersOpen));
  controls();
}
function chooseFilter(index){
  if(typeof system.camera.setStyle==='function'&&system.camera.setStyle(index)){
    colorStyle=index;setFiltersOpen(false);setStatus(tr('色彩：')+filterNames[index]);
  }else setStatus(tr('滤镜暂时无法应用'));
}

function qualityAvailable(){
  return !!selected&&!!selected.previewSupported&&(selected.transport==='csi'||modes.length>0);
}
function layoutSettings(){
  const paged=devices.length>rowCount;
  for(const id of [previousButton,previous.icon])ui.setHidden(id,!settingsOpen||!paged||devicePage===0);
  for(const id of [nextButton,next.icon])ui.setHidden(id,!settingsOpen||!paged||(devicePage+1)*rowCount>=devices.length);
  showDots(pageDots,Math.ceil(devices.length/rowCount),devicePage,settingsOpen&&paged);
  const quality=settingsOpen&&qualityAvailable();
  for(const id of [resolution.icon,rate.icon])ui.setHidden(id,!quality);
  showDots(modeDots,selected&&selected.transport==='csi'?1:modes.length,modeIndex,quality);
  showDots(fpsDots,selected&&selected.transport==='csi'?1:intervals.length,intervalIndex,quality);
}

function closeSettings(){
  setSettingsOpen(false);
  if(resumeAfterSelect){resumeAfterSelect=false;start();}
}
function recordingBusy() { return recording.status >= 1 && recording.status <= 3; }
function syncRecording() {
  if (!hasVideo) return;
  const next = system.camera.recordStatus() || {status: 0};
  const changed = next.status !== recording.status || next.revision !== recording.revision;
  recording = next;
  videoMode = !!next.video;
  syncControls();
  if(changed)renderMode();

  if (changed && next.status === 4 && next.path) setStatus(tr('录像已保存 · ') + next.path);
  else if (changed && next.status === 3) setStatus(tr('正在保存录像，请勿拔出 SD 卡'));
  else if (changed && next.status < 0) setStatus(next.status === -28 ? tr('SD 卡空间不足') :
    next.status === -19 || next.status === -2 ? tr('录像需要可用的 SD 卡和麦克风') : tr('录像失败（') + next.status + '）');
}
function toggleVideo() {
  if (!hasVideo || pending || running || system.camera.status().busy || recordingBusy()) return;
  if (system.camera.setVideoMode(!videoMode)) {
    syncRecording();
    renderMode();
    setStatus(videoMode ? tr('视频 · 麦克风开启 · 保存到 SD 卡') : tr('照片'));
  }
}

function turnPage(delta) {
  if (pending || running || system.camera.status().busy) return;
  devicePage = Math.max(0, Math.min(Math.ceil(devices.length / rowCount) - 1, devicePage + delta));
  renderRows();
}

function key(d) { return d.id + ':' + d.generation; }
function setStatus(value) {
  message=value;write(status,value);
  const error=/失败|超时|断开|不足|需要可用|没有可用|无法|停止（/.test(value);
  statusVisible=error||/已保存|取消|色彩：|视频 ·|正在/.test(value);
  ui.setStyle(status,{textColor:error?0xff8178:/已保存/.test(value)?colors.success:colors.secondary});
  ui.setHidden(status,settingsOpen||!statusVisible);
}
function controls(busy) {
  busy = busy || pending || running || system.camera.status().busy || recordingBusy();
  const ready = !!selected && !!selected.previewSupported && (selected.transport === 'csi' || (modes.length > 0 && intervals.length > 0));
  shutterReady=!busy&&ready;
  enable(startButton,shutterReady&&!settingsOpen&&!filtersOpen);
  enable(menuShutter,shutterReady);syncControls();
  enable(filterShutter,shutterReady);filterChoices.forEach(id=>enable(id,!busy));
  enable(aspectButton,!busy&&!filtersOpen);ui.setHidden(aspectButton,filtersOpen);
  enable(deviceButton, !busy&&!filtersOpen);
  enable(captureMode, !busy && hasVideo && !settingsOpen&&!filtersOpen);
  enable(colorButton, !busy && !settingsOpen);
  enable(albumButton, !busy && !settingsOpen&&!filtersOpen);
  enable(refreshButton, !busy);
  enable(modeButton, !busy && modes.length > 1);
  enable(fpsButton, !busy && intervals.length > 1);
  const quality=settingsOpen&&qualityAvailable();
  ui.setHidden(modeButton,!quality);ui.setHidden(fpsButton,!quality);
  ui.setOpacity(resolution.icon,!busy&&modes.length>1?255:150);
  ui.setOpacity(rate.icon,!busy&&intervals.length>1?255:150);
  enable(previousButton, !busy && devicePage > 0);
  enable(nextButton, !busy && (devicePage + 1) * rowCount < devices.length);
  rows.forEach(row => enable(row, !busy));
}
function renderMode() {
  const ready=selected&&selected.previewSupported;
  const name=selected?shortName(selected.name):'';
  const transport=selected&&selected.transport==='csi'?'CSI':'USB';
  write(deviceName, selected ? name.toUpperCase().startsWith(transport)?name:transport+' · '+name : 'CAMERA');
  write(previewTitle, ready ? tr('按快门开始取景') : selected?tr('设备不支持预览'):tr('未连接摄像头'));
  ui.setHidden(previewHint,settingsOpen||!!ready);
  ui.setHidden(status,settingsOpen||!statusVisible);
  const m = modes[modeIndex];
  write(modeButton, selected && selected.transport === 'csi' ? '1024 x 600' :
    m ? m.width + ' x ' + m.height : tr('分辨率不可用'));
  const interval = intervals[intervalIndex];
  const csi=selected&&selected.transport==='csi';
  const fps=csi?30:interval?10000000/interval:0;
  write(captureInfo,csi?'1024 x 600 · 30 fps':m&&fps?
    m.width+' x '+m.height+' · '+Number(fps.toFixed(1))+' fps':'');
  write(fpsButton, selected && selected.transport === 'csi' ? '30 fps' : interval ?
    Number((10000000 / interval).toFixed(1)) + ' fps' : tr('帧率不可用'));
  layoutSettings();
  syncControls();
}
function chooseIntervals() {
  intervals = []; intervalIndex = 0;
  const m = modes[modeIndex];
  if (m) {
    if (m.continuous && m.intervals.length >= 3) {
      const min = m.intervals[0], max = m.intervals[1], step = m.intervals[2];
      if (Number.isFinite(min) && Number.isFinite(max) && min > 0 && max >= min && Number.isFinite(step) && step >= 0)
      [m.defaultInterval, min, 333333, 666666, 1000000, max].filter(v => Number.isFinite(v) && v > 0).forEach(v => {
        let n = Math.max(min, Math.min(max, v));
        if (step) n = min + Math.floor((n - min) / step) * step;
        if (!intervals.includes(n)) intervals.push(n);
      });
    } else intervals = m.intervals.filter(v => Number.isFinite(v) && v > 0);
    const preferred = intervals.findIndex(v => v >= 666666);
    intervalIndex = preferred >= 0 ? preferred : Math.max(0, intervals.indexOf(m.defaultInterval));
  }
  renderMode();
}
function select(index) {
  if (pending || running || system.camera.status().busy || !devices[index]) return false;
  selected = devices[index]; modes = []; modeIndex = 0;
  if (selected.transport === 'usb') {
    try { modes = system.camera.modes(selected.id, selected.generation); }
    catch (_) { setStatus(tr('设备已断开，请刷新')); selected = null; }
    let best = modes.findIndex(m => m.width === 640 && m.height === 480);
    if (best < 0) best = modes.findIndex(m => m.width <= 1280 && m.height <= 720);
    if (best >= 0) modeIndex = best;
  }
  chooseIntervals();
  if (selected) setStatus(tr('已选择 ') + shortName(selected.name));
  renderRows(); controls(); renderMode();
  return !!selected && !!selected.previewSupported;
}
function shortName(name) {
  const text = Array.from(name || '');
  return (text.length > 28 ? text.slice(0, 27).join('') + '…' : text.join(''));
}
function renderRows() {
  const slots=Math.min(rowCount,devices.length-devicePage*rowCount);
  for (let i = 0; i < rows.length; i++) {
    const d = devices[devicePage * rowCount + i];
    ui.setHidden(rows[i], !d || !settingsOpen);
    const current = d&&selected&&key(d)===key(selected);
    ui.setHidden(rowMarkers[i],!settingsOpen||!current);
    ui.setHidden(rowIcons[i],!settingsOpen||!d);
    ui.setHidden(rowWells[i],!settingsOpen||!current);
    if (!d) continue;
    const x=812+(3-slots)*26+i*52;
    ui.setPos(rows[i],sx(x),sy(116));ui.setPos(rowWells[i],sx(x+2),sy(118));
    ui.setPos(rowIcons[i],sx(x+12),sy(126));ui.setPos(rowMarkers[i],sx(x+22),sy(170));
    write(rowIcons[i],d.transport==='csi'?icons.camera:icons.usb);
    ui.setOpacity(rowIcons[i],d.previewSupported?255:70);
    write(rows[i], shortName(d.name) + (d.previewSupported ? '' : tr(' · 不支持')));
  }
  if(!devices.length){
    write(emptyIcon,icons.camera);ui.setPos(emptyIcon,sx(880),sy(126));
    ui.setOpacity(emptyIcon,110);ui.setHidden(emptyIcon,!settingsOpen);
  }
  layoutSettings();
  controls();
}
function refresh(force) {
  if (pending || running || system.camera.status().busy) return;
  lastScan = Date.now();
  let current;
  try { current = system.camera.devices(); }
  catch (_) { setStatus(tr('设备查询失败，请重试')); return; }
  const next = current.map(key).join(',');
  if (!force && next === signature) return;
  signature = next; devices = current;
  devicePage = Math.max(0, Math.min(devicePage, Math.ceil(devices.length / rowCount) - 1));
  if (selected && !devices.some(d => key(d) === key(selected))) {
    pending = false; system.camera.stop(); running = false; selected = null;
    modes = []; intervals = []; setStatus(tr('摄像头已断开，请重新选择'));
  }
  if (!selected && devices.length) {
    const supported = devices.findIndex(d => d.previewSupported);
    select(supported >= 0 ? supported : 0);
    devicePage = Math.floor(Math.max(0, supported) / rowCount);
  }
  renderRows(); renderMode();
  if (!devices.length && !pending) setStatus(tr('未检测到摄像头，请连接 CSI 或 USB 摄像头'));
  controls();
}
function start() {
  syncRecording();
  if (pending || running || system.camera.status().busy || recordingBusy()) return;
  if(filtersOpen)setFiltersOpen(false);
  setSettingsOpen(false);
  refresh(false);
  if (!selected || !selected.previewSupported) { setStatus(tr('请选择可预览的摄像头')); return; }
  if (selected.transport === 'usb' && (!modes.length || !intervals.length)) { setStatus(tr('该摄像头没有可用的视频模式')); return; }
  const photo = system.camera.photoStatus();
  photoBaseline = photo && photo.path ? photo.path : '';
  system.camera.stop(); pending = true; requestAt = Date.now();
  setStatus(tr('正在准备预览…')); controls();
}
function stop() {
  pending = false; running = false; system.camera.stop();
  setStatus(stoppedStatus(0)); controls();
}
// The live preview paints its own toolbar, so the saved path is reported on
// the page after the preview closes.
function stoppedStatus(error) {
  if (error) return tr('预览已停止（') + error + tr('），请检查连接或更换视频模式');
  if (hasVideo) {
    const video = system.camera.recordStatus() || {};
    if (video.status === 3) return tr('正在保存录像，请勿拔出 SD 卡');
    if (video.status === 4 && video.path) return tr('录像已保存 · ') + video.path;
    if (video.status < 0) return tr('录像失败（') + video.status + '）';
  }
  const photo = system.camera.photoStatus() || {};
  if (photo.status === 2 && photo.path && photo.path !== photoBaseline)
    return tr('照片已保存 · ') + photo.path;
  if (photo.status === -125) return tr('已取消拍照');
  if (photo.status < 0 && !photo.path) return tr('拍照失败（') + photo.status + '）';
  return tr('预览已停止');
}
function tick() {
  syncZoom();
  const state = system.camera.status();
  syncRecording();
  if(!state.busy&&typeof system.camera.takeAction==='function'&&system.camera.takeAction()===3){
    pending=false;running=false;resumeAfterSelect=true;refresh(true);setSettingsOpen(true);return;
  }
  if (pending) {
    if (Date.now() - requestAt > 10000) {
      pending = false; setStatus(tr('相机释放超时，请稍后重试')); controls(); return;
    }
    if (state.busy) return;
    pending = false;
    const m = modes[modeIndex];
    running = system.camera.startDevice(selected.id, selected.generation,
      m ? m.index : 0, intervals[intervalIndex] || 0);
    if (!running) setStatus(tr('启动失败，请刷新设备后重试'));
    controls();
    return;
  }
  if (running && !state.busy) {
    running = false;
    setStatus(stoppedStatus(state.error));
    controls();
  }
  if (!state.busy && Date.now() - lastScan >= 2000) refresh(false);
  controls();
}
syncRecording();
refresh(true);
setInterval(tick, 500);
if(typeof ui.onBack==='function')ui.onBack(()=>{
  if(settingsOpen){resumeAfterSelect=false;setSettingsOpen(false);return true;}
  if(filtersOpen){setFiltersOpen(false);return true;}
  return false;
});
globalThis.CameraPreview = { start, stop, refresh: () => refresh(true), frames: () => system.camera.frames(), zoom: () => zoomPercent, style: () => colorStyle, video: () => videoMode, settings: () => settingsOpen, filters: () => filtersOpen, page: () => devicePage };
