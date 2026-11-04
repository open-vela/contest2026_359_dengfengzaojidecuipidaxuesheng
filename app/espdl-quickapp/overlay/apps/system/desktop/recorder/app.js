/* SPDX-License-Identifier: Apache-2.0 */
'use strict';

const tr = value => typeof system.translate === 'function' ? system.translate(value) : value;


const size = ui.getSize(), W = size.width, H = size.height;
const theme=ui.theme||{background:ui.surface,onAccent:0xffffff,danger:0xd95068,cardRadius:18};
const left = 24, top = 24, leftWidth = Math.round((W - 68) * 0.41);
const right = left + leftWidth + 20, rightWidth = W - right - 24;
const tight=H<520, narrow=rightWidth<460;
const LIST_ROWS=Math.min(4,Math.max(2,Math.floor((H-(narrow?236:228))/74)));
const meterCenter=tight?234:250;
const red = theme.danger;
let task = 0, operation = '', stopping = false, pendingClip = '';
let clips = [], selected = '', page = 0, nextNumber = 1, loaded = false;
let preferred = '', afterList = '', confirmClip = '', confirmTicks = 0;
let elapsed = 0, history = Array(28).fill(0);
let captionSegments = [], captionClip = '', pendingPlay = false;

function text(value, x, y, width, height, font, color) {
  const id = ui.text(value, x, y, font || 20, color === undefined ? ui.primary : color);
  ui.setSize(id, width, height); ui.setStyle(id, {ellipsis: 1}); return id;
}
function panel(x, y, width, height) {
  const id = ui.panel(x, y, width, height, ui.card);
  ui.setStyle(id, {radius: theme.cardRadius, borderWidth: 0}); return id;
}
function time(ms) {
  const seconds = Math.floor(ms / 1000);
  return String(Math.floor(seconds / 60)).padStart(2, '0') + ':' + String(seconds % 60).padStart(2, '0');
}
function title(clip) {
  clip=clip.replace(/^flash:/,'');
  const match = /^rec_(\d+)$/.exec(clip);
  return match ? tr('录音 ') + String(Number(match[1])).padStart(3, '0') : clip;
}
function errorText(error, kind) {
  const code = Math.abs(Number(error.error) || 0);
  if((kind==='transcribe'||kind==='captions')&&(code===19||code===2))return tr('请插入 SD 卡并检查本地识别模型');
  if(kind==='transcribe'&&code===61)return tr('这段录音未识别出文字');
  if(kind==='transcribe'&&code===22)return tr('这段录音的格式暂不支持转写');
  if(kind==='transcribe'&&code===7)return tr('录音或文字太长，请分段转写');
  if(kind==='transcribe'&&code===16)return tr('本地模型正忙，请稍后重试');
  if(kind==='transcribe'&&code===125)return tr('已停止转文字，原有文字保留');
  if(kind==='record' && (code===19 || code===2))return tr('请插入可用的 SD 卡后录音');
  if (code === 61 || code === 110) return kind === 'play' ?
    tr('播放超时，音频设备未返回数据，请重试') : tr('麦克风没有返回数据，请重试');
  return ({16: tr('音频设备正忙，请稍后重试'), 19: tr('音频设备未就绪'),
    28: tr('存储空间不足，请删除旧录音'), 5: tr('音频读写失败，请重试'),
    12: tr('内存不足，请稍后重试')})[code] || tr('操作失败，请重试');
}

ui.background(theme.background);
panel(left, top, leftWidth, H - 48);
panel(right, top, rightWidth, H - 48);
text(tr('新录音'), left + 22, 44, leftWidth - 44, 32, 28);
const status = text(tr('正在读取录音…'), left + 22, 88, leftWidth - 44, tight?20:24, tight?16:20, ui.secondary);
const clock = ui.number('00:00', left + 22, 122, leftWidth - 44, 48, ui.primary);
ui.setStyle(clock, {borderWidth: 0});
text(tr('音量'), left + 22, 186, leftWidth - 44, 20, 16, ui.secondary);
const bars = [];
const step = (leftWidth - 44) / history.length;
for (let i = 0; i < history.length; i++) {
  const bar = ui.rect(Math.round(left + 22 + i * step), meterCenter-2, Math.max(3, Math.floor(step - 4)), 4, ui.surface);
  ui.setStyle(bar, {radius: 2, borderWidth: 0}); bars.push(bar);
}

const captionText = text(tr('选择录音后可转文字'), left + 22, meterCenter + (tight?25:36), leftWidth - 44, 42, 16, ui.secondary);
ui.setStyle(captionText, {ellipsis: 0});

const recordButton = ui.button(tr('开始录音'), left + 22, H - (tight?174:202), leftWidth - 44, tight?44:56, record, red);
const stopWidth=Math.floor((leftWidth-56)/2);
const stopButton = ui.button(tr('停止并保存'), left + 22, H - (tight?116:132), stopWidth, 52, stop, ui.accent);
const cancelButton = ui.button(tr('放弃本次'), left + 34 + stopWidth, H - (tight?116:132), stopWidth, 52, cancel, ui.surface);
ui.setStyle(stopButton, {fontSize: 16});
ui.setStyle(cancelButton, {fontSize: 16});
text(tr('返回将放弃尚未保存的录音'), left + 22, H - (tight?54:66), leftWidth - 44, 24, 14, ui.secondary);

text(tr('录音列表'), right + 22, 44, Math.min(240, rightWidth - 168), 32, 28);
const countLabel = text(tr('0 条'), right + rightWidth - 154, 51, 132, 24, 14, ui.secondary);
ui.setStyle(countLabel, {rightAlign: 1});
const empty = text(tr('还没有录音\n点击左侧开始录音'), right + 38, 206, rightWidth - 76, 100, 20, ui.secondary);
ui.setStyle(empty, {center: 1, ellipsis: 0});
const rows = [];
for (let i = 0; i < LIST_ROWS; i++) {
  const y = (narrow ? 94 : 116) + i * 74;
  const button = ui.button('', right + 16, y, rightWidth - 32, 66, () => select(page * LIST_ROWS + i), ui.surface);
  ui.setStyle(button, {radius: 12, borderWidth: 0});
  rows.push({button,
    name: text('', right + 32, y + 8, rightWidth - 64, 27, 20),
    detail: text('', right + 32, y + 39, rightWidth - 64, 22, 16, ui.secondary)});
}
const selectionHint = text(tr('选择录音后可以播放或删除'), right + 22, 82, rightWidth - 44, 24, 14, ui.secondary);
ui.setHidden(selectionHint,rightWidth<460);
const actionWidth = Math.floor((rightWidth - 56) / 3);
const playButton = ui.button(tr('播放'), right + 16, H - 112, actionWidth, 44, play, ui.accent);
const transcribeButton = ui.button(tr('转文字'), right + 28 + actionWidth, H - 112, actionWidth, 44, transcribe, ui.surface);
const deleteButton = ui.button(tr('删除'), right + 40 + actionWidth * 2, H - 112, actionWidth, 44, remove, ui.surface);
for(const id of [playButton,transcribeButton,deleteButton])ui.setStyle(id,{fontSize:16});
const pagerDark=ui.primary>0x888888,pagerColor=pagerDark?0x2e303d:0xf3f2f7;
const pagerFace=ui.panel(0,0,40,24,pagerColor);ui.setStyle(pagerFace,{radius:14,borderWidth:0});
const pagerSlots=Array.from({length:5},()=>{
  const slot={page:-1};
  slot.button=ui.button('',0,0,24,30,()=>{if(slot.page>=0)turnPage(slot.page-page);},ui.card);
  ui.setStyle(slot.button,{radius:0,borderWidth:0});
  slot.mark=ui.panel(0,0,6,6,ui.secondary);ui.setStyle(slot.mark,{radius:3,borderWidth:0});
  return slot;
});
if(typeof ui.front==='function'){ui.front(pagerFace);pagerSlots.forEach(slot=>ui.front(slot.mark));}
function drawPager(){
  const total=Math.max(1,Math.ceil(clips.length/LIST_ROWS)),count=Math.min(5,total),first=Math.max(0,Math.min(page-Math.floor(count/2),total-count));
  const width=16+count*24,x=Math.round(right+rightWidth/2-width/2),y=H-30,visible=total>1;
  ui.setHidden(pagerFace,!visible);ui.setPos(pagerFace,x,y+6);ui.setSize(pagerFace,width,24);
  pagerSlots.forEach((slot,i)=>{
    const shown=visible&&i<count;slot.page=shown?first+i:-1;
    ui.setHidden(slot.button,!shown);ui.setHidden(slot.mark,!shown);if(!shown)return;
    const active=slot.page===page,w=active?20:6;
    ui.setPos(slot.button,x+8+i*24,y);ui.setStyle(slot.button,{enabled:!task});
    ui.setPos(slot.mark,x+8+i*24+(24-w)/2,y+15);ui.setSize(slot.mark,w,6);
    ui.setColor(slot.mark,active?(pagerDark?0xeeeef5:0x292c38):(pagerDark?0x5c606e:0xc5c6ce));
  });
}

function enable(button, enabled, color, bright) {
  ui.setStyle(button, {enabled: !!enabled, textColor: enabled && bright ? theme.onAccent : ui.primary});
  ui.setColor(button, enabled ? color : ui.surface);
}
function current() { return clips.find(clip => clip.clip === selected); }
function controls() {
  const busy = task !== 0, entry = current();
  const audio = ['record','play','transcribe','captions'].includes(operation);
  enable(recordButton, !busy, red, true);
  enable(stopButton, busy && audio && !stopping, ui.accent, true);
  enable(cancelButton, busy && operation === 'record' && !stopping, ui.surface, false);
  enable(playButton, !busy && !!entry && entry.valid !== false, ui.accent, true);
  enable(transcribeButton, !busy && !!entry && entry.valid !== false, ui.surface, false);
  ui.setText(transcribeButton, captionClip===selected&&captionSegments.length?tr('重新转写'):tr('转文字'));
  enable(deleteButton, !busy && !!entry && !entry.readOnly, confirmClip ? red : ui.surface, !!confirmClip);
  drawPager();
  ui.setText(recordButton, operation === 'record' ? tr('录音中') : loaded ? tr('开始录音') : tr('重试读取'));
  ui.setText(stopButton, operation === 'transcribe' ? tr('停止转写') : operation === 'play'||operation==='captions' ? tr('停止播放') : tr('停止并保存'));
  ui.setText(deleteButton, confirmClip ? tr('确认删除') : tr('删除'));
  for (let i = 0; i < rows.length; i++) ui.setStyle(rows[i].button, {enabled: !busy});
}
function drawList() {
  page = Math.min(page, Math.max(0, Math.ceil(clips.length / LIST_ROWS) - 1));
  ui.setText(countLabel, clips.length + tr(' 条'));
  ui.setHidden(empty, clips.length > 0);
  for (let i = 0; i < rows.length; i++) {
    const entry = clips[page * LIST_ROWS + i], row = rows[i];
    for (const key of ['button', 'name', 'detail']) ui.setHidden(row[key], !entry);
    if (!entry) continue;
    ui.setText(row.name, title(entry.clip));
    ui.setText(row.detail, entry.readOnly ? tr('旧 Flash 录音 · 只读') : entry.valid === false ? tr('文件不完整 · 可删除') :
      time(entry.durationMs || 0) + '  ·  ' + Math.max(1, Math.round(entry.fileBytes / 1024)) + ' KB');
    ui.setStyle(row.button, {borderWidth: entry.clip === selected ? 2 : 0, borderColor: ui.accent});
  }
  ui.setText(selectionHint, confirmClip ? tr('再次点击“确认删除”将删除这条录音') :
    selected ? tr('已选择：') + title(selected) : tr('选择录音后可以播放或删除'));
  controls();
}
function meter(peak) {
  const amplitude=Math.min(32768,Math.max(0,Number(peak)||0));
  history.shift(); history.push(Math.log1p(amplitude/64)/Math.log1p(32768/64));
  for (let i = 0; i < bars.length; i++) {
    const height = Math.max(4, Math.round(history[i] * (tight?32:56)));
    ui.setPos(bars[i], Math.round(left + 22 + i * step), meterCenter - Math.floor(height / 2));
    ui.setSize(bars[i], Math.max(3, Math.floor(step - 4)), height);
    ui.setColor(bars[i], history[i] > 0 ? red : ui.surface);
  }
}
function clearCaptions() {
  captionSegments=[];captionClip='';ui.setText(captionText,tr('暂无文字，点击转文字'));
}
function setCaptions(result,clip) {
  captionClip=clip;captionSegments=result.available&&Array.isArray(result.segments)?result.segments:[];
  ui.setText(captionText,captionSegments.length?Array.from(String(result.text||'')).slice(0,36).join(''):tr('暂无文字，点击转文字'));
}
function subtitleAt(ms) {
  if(captionClip!==selected||!captionSegments.length)return;
  let value='';
  for(const segment of captionSegments){if(Number(segment.atMs)>ms)break;value+=String(segment.text||'');}
  ui.setText(captionText,value?Array.from(value).slice(-36).join(''):tr('等待播放到人声位置…'));
}
function startPlayback() {
  if(begin('play',()=>system.audio.play({clip:selected,volume:40}),tr('正在播放：')+title(selected))){
    elapsed=0;ui.setText(clock,'00:00');history.fill(0);meter(0);subtitleAt(0);
  }
}
function transcribe() {
  const entry=current();if(task||!entry||entry.valid===false)return;
  pendingPlay=false;
  begin('transcribe',()=>system.audio.transcribe({clip:selected,force:true}),tr('正在本地转文字，可随时停止'));
}
function begin(kind, submit, message) {
  if (task) return false;
  try {
    task = submit(); operation = kind; stopping = false;
    confirmClip = ''; ui.setText(status, message); controls(); return true;
  } catch (error) {
    task = 0; operation = ''; ui.setText(status, tr('无法开始操作，请稍后重试')); controls(); return false;
  }
}
function refresh(clip, notice) {
  preferred = clip || ''; afterList = notice || tr('准备录音');
  loaded = false;
  begin('list', () => system.audio.list(), tr('正在读取录音…'));
}
function record() {
  if (task) return;
  if (!loaded) { refresh('', tr('准备录音')); return; }
  clearCaptions();
  pendingClip = 'rec_' + String(nextNumber).padStart(6, '0');
  if (begin('record', () => system.audio.record({clip: pendingClip, durationMs: 0}), tr('正在录音'))) {
    elapsed = 0; ui.setText(clock, '00:00'); history.fill(0); meter(0);
  }
}
function stop() {
  if (!task || stopping || !['record','play','transcribe','captions'].includes(operation)) return;
  try {
    if(operation==='transcribe'||operation==='captions'){pendingPlay=false;system.hardware.cancel(task);}else system.audio.stop(task); stopping = true;
    ui.setText(status, operation === 'record' ? tr('正在保存录音…') : operation==='transcribe'?tr('正在停止转文字…'):tr('正在停止播放…')); controls();
  } catch (error) { ui.setText(status, tr('停止请求失败，请重试')); }
}
function cancel() {
  if (!task || operation !== 'record' || stopping) return;
  try { system.hardware.cancel(task); stopping = true; ui.setText(status, tr('正在放弃录音…')); controls(); }
  catch (error) { ui.setText(status, tr('取消请求失败，请重试')); }
}
function select(index) {
  if (task || !clips[index]) return;
  if(selected!==clips[index].clip)clearCaptions();
  selected = clips[index].clip; confirmClip = ''; drawList();
}
function turnPage(delta) {
  if (task) return;
  page = Math.max(0, Math.min(Math.ceil(clips.length / LIST_ROWS) - 1, page + delta));
  confirmClip = ''; drawList();
}
function play() {
  const entry=current();if(task||!entry||entry.valid===false)return;
  pendingPlay=true;
  if(!begin('captions',()=>system.audio.transcript({clip:selected}),tr('准备播放…')))pendingPlay=false;
}
function remove() {
  if (task || !current()) return;
  if(current().readOnly)return;
  if (confirmClip !== selected) { confirmClip = selected; confirmTicks = 50; drawList(); return; }
  begin('remove', () => system.audio.remove({clip: selected}), tr('正在删除录音…'));
}
function poll() {
  if (confirmTicks > 0 && --confirmTicks === 0) { confirmClip = ''; drawList(); }
  if (!task) return;
  let state;
  try { state = system.hardware.poll(task); }
  catch (error) { ui.setText(status, tr('读取状态失败，正在重试…')); return; }
  if (!state.done) {
    if (operation === 'record' || operation === 'play') {
      elapsed = Number(state.elapsedMs) || 0; ui.setText(clock, time(elapsed));
      meter(state.peak);if(operation==='play')subtitleAt(elapsed);
    }
    if(operation==='transcribe'){elapsed=Number(state.elapsedMs)||0;ui.setText(clock,time(elapsed));ui.setText(status,[10,13].includes(Number(state.stage))?tr('正在准备本地模型…'):tr('本地转写 ')+time(elapsed)+' / '+time((current()||{}).durationMs||0));}
    return;
  }
  const finished = operation, wasStopping=stopping;
  task = 0; operation = ''; stopping = false;
  if(finished==='captions') {
    if(state.ok)setCaptions(state.result||{},selected);else clearCaptions();
    const shouldPlay=pendingPlay&&!wasStopping;pendingPlay=false;
    if(shouldPlay){startPlayback();return;}
    ui.setText(status,tr('播放已停止'));drawList();return;
  }
  if(finished==='transcribe') {
    if(state.ok)setCaptions(state.result||{},selected);
    ui.setText(status,state.ok?tr('文字已保存 · 播放时同步显示'):errorText(state,'transcribe'));
    drawList();return;
  }
  if (finished === 'list' && state.ok) {
    loaded = true;
    clips = state.result.clips || [];
    clips.sort((a, b) => {
      const an = /^rec_(\d+)$/.exec(a.clip), bn = /^rec_(\d+)$/.exec(b.clip);
      return an && bn ? Number(bn[1]) - Number(an[1]) : b.clip.localeCompare(a.clip);
    });
    nextNumber = 1;
    for (const clip of clips) {
      const number = /^rec_(\d+)$/.exec(clip.clip);
      if (number) nextNumber = Math.max(nextNumber, Number(number[1]) + 1);
    }
    if (preferred && clips.some(clip => clip.clip === preferred)) selected = preferred;
    if (!clips.some(clip => clip.clip === selected)) selected = clips.length ? clips[0].clip : '';
    if (preferred) page = Math.max(0, Math.floor(clips.findIndex(clip => clip.clip === selected) / LIST_ROWS));
    ui.setText(status, afterList); drawList(); return;
  }
  if (finished === 'record') {
    elapsed = Number(state.elapsedMs) || elapsed; ui.setText(clock, time(elapsed));
    refresh(state.ok ? pendingClip : selected, state.ok ? tr('录音已保存') :
      Math.abs(state.error) === 125 ? tr('已放弃本次录音') : errorText(state, 'record')); return;
  }
  if (finished === 'remove' && state.ok) { clearCaptions(); confirmClip = ''; refresh('', tr('录音已删除')); return; }
  if(finished==='play'&&state.ok){elapsed=Number((state.result||{}).playedMs)||Number(state.elapsedMs)||elapsed;ui.setText(clock,time(elapsed));subtitleAt(elapsed);}
  ui.setText(status, state.ok ? tr('播放已结束') : errorText(state, finished));
  drawList();
}

globalThis.RecorderApp = {record, stop, cancel, play, transcribe, remove, select, turnPage,
  state: () => ({task, operation, selected, count: clips.length, elapsed, stopping,captionClip,captionCount:captionSegments.length,page,pager:pagerSlots.filter(s=>s.page>=0).map(s=>({page:s.page,button:s.button}))})};
if(typeof ui.onBack==='function')ui.onBack(()=>{
  if(!confirmClip)return false;
  confirmClip='';confirmTicks=0;drawList();return true;
});
drawList(); refresh('', tr('准备录音')); setInterval(poll, 100);
