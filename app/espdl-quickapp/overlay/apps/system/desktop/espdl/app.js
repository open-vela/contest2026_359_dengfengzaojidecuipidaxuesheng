'use strict';

const tr = value => typeof system.translate === 'function' ? system.translate(value) : value;

const size = ui.getSize(), dl = system.espdl;
const names = [tr('物体分类'), tr('人脸跟随')];
// The runtime publishes the same card, surface, text and accent colours the
// desktop uses, so the app follows the current theme without hardcoding it.
const light = ((ui.card >> 16) & 255) + ((ui.card >> 8) & 255) + (ui.card & 255) > 384;
const colors = {
  background: ui.theme ? ui.theme.background : ui.card, card: ui.card, inset: ui.surface, primary: ui.primary,
  secondary: ui.secondary, accent: ui.accent, onAccent: ui.theme ? ui.theme.onAccent : light ? 0xffffff : 0x252d46,
  border: ui.theme ? ui.theme.border : light ? 0xcbd0e6 : 0x505975,
  good: ui.theme ? ui.theme.success : light ? 0x237f73 : 0x7bd8bb,
  error: ui.theme ? ui.theme.danger : light ? 0xa53949 : 0xffa4af
};
let mode = 1, request = 0, frame = 0, devices = [], deviceIndex = 0, switching = false;
let afterStop = '', lastDeviceCheck = 0, fpsStart = 0, fpsFrame = 0, fps = '--';
const labels = new Map(), enabled = new Map();
function write(id, value) {
  value = String(value);
  if (labels.get(id) !== value) { ui.setText(id, value); labels.set(id, value); }
}
function label(value, x, y, w, h, font = 16, color = colors.primary, center = false) {
  const id = ui.text(value, x, y, font, color);
  ui.setSize(id, w, h); ui.setStyle(id, {center, ellipsis: true});
  labels.set(id, value); return id;
}
function card(x, y, w, h, fill = colors.card, radius = 18) {
  const id = ui.panel(x, y, w, h, fill);
  ui.setStyle(id, {radius, borderColor: colors.border, borderWidth: 1, borderOpacity: 100});
  return id;
}
function button(value, x, y, w, action, primary = false, h = 48) {
  const id = ui.button(value, x, y, w, h, action, primary ? colors.accent : colors.card);
  ui.setStyle(id, {radius: 12, borderColor: colors.border, borderWidth: primary ? 0 : 1,
    borderOpacity: 110, textColor: primary ? colors.onAccent : colors.primary, fontSize: 20});
  labels.set(id, value); return id;
}
function enable(id, value) {
  if (enabled.get(id) !== value) { ui.setStyle(id, {enabled: value}); enabled.set(id, value); }
}
ui.background(colors.background);
card(24, 4, 320, 48, colors.card, 16);
const buttons = [];
[1, 0].forEach((i, position) => {
  buttons[i] = button(names[i], 28 + position * 158, 8, 154, () => selectMode(i), false, 40);
});
const modeHint = label(tr('选择模式后点击开始'), 364, 18, 260, 24, 16, colors.secondary);
card(size.width - 188, 8, 164, 36, colors.card, 18);
label(tr('本地识别 · 无需联网'), size.width - 184, 14, 156, 24, 16, colors.accent, true);
ui.setHidden(modeHint,size.width<900);

const bodyY = 64, footerY = size.height - 64, bodyH = footerY - bodyY - 16;
const leftW = Math.min(512, Math.floor((size.width - 64) * 0.54));
const rightX = 24 + leftW + 16, rightW = size.width - rightX - 24;
card(24, bodyY, leftW, bodyH);
// Integral scale steps keep the display aspect ratio exactly 4:3.
const previewW = Math.min(480, Math.floor(Math.min(leftW - 32, (bodyH - 32) * 4 / 3) / 40) * 40);
const previewH = previewW * 3 / 4;
const previewX = 24 + Math.floor((leftW - previewW) / 2);
const previewY = bodyY + Math.floor((bodyH - previewH) / 2);
const centerX = 24 + leftW / 2, centerY = bodyY + bodyH / 2;
const placeholder = [card(centerX - 36, centerY - 76, 72, 72, colors.inset, 24)];
// LV_SYMBOL_EYE_OPEN for following, LV_SYMBOL_IMAGE for classification.
const modeIcon = ui.text('\uf06e', centerX - 12, centerY - 52, 24, colors.accent, 1);
placeholder.push(modeIcon);
const emptyTitle = label(tr('相机已就绪'), 44, centerY + 16, leftW - 40, 32, 20, colors.primary, true);
const emptyHint = label(tr('点击开始跟随，查看实时画面'), 44, centerY + 56, leftW - 40, 28, 16, colors.secondary, true);
placeholder.push(emptyTitle, emptyHint);
const preview = dl.preview(previewX, previewY, previewW);
ui.setHidden(preview, true);
let previewVisible = false;
function showPreview(show) {
  if (previewVisible === show) return;
  previewVisible = show;
  ui.setHidden(preview, !show); placeholder.forEach(id => ui.setHidden(id, show));
}

const compact=size.height<504,deviceH=compact?72:100;
const resultH = bodyH - deviceH - 16;
card(rightX, bodyY, rightW, resultH);
const section = label(tr('跟随状态'), rightX + 24, bodyY + (compact?8:16), rightW - 48, 24, compact?14:16, colors.secondary);
const title = label('', rightX + 24, bodyY + (compact?32:48), rightW - 48, compact?28:40, compact?20:28);
const detail = label('', rightX + 24, bodyY + (compact?60:92), rightW - 48, compact?22:40, compact?14:16, colors.secondary);
const rows = [];
for (let i = 0; i < 3; i++) {
  const y = bodyY + (compact?86:140) + i * (compact?24:36);
  const name = label('', rightX + 24, y, rightW - 146, compact?20:28,compact?14:16);
  const value = label('--', rightX + rightW - 114, y, 90, compact?20:28, compact?14:16, colors.accent);
  const line = ui.rect(rightX + 24, y + (compact?21:30), rightW - 48, 2, colors.border);
  const bar = ui.rect(rightX + 24, y + (compact?21:30), 1, 2, colors.accent);
  ui.setHidden(bar, true);
  rows.push({name, value, line, bar});
}
const deviceY = bodyY + resultH + 16;
card(rightX, deviceY, rightW, deviceH);
label(tr('当前相机'), rightX + 20, deviceY + (compact?4:12), rightW - 40, compact?20:24, compact?14:16, colors.secondary);
const source = button(tr('检测相机'), rightX + 16, deviceY + (compact?24:42), rightW - 32, cycleDevice, false, 44);
const status = label(tr('请选择相机后开始'), 24, footerY + 4, leftW, 44, 16, colors.secondary);
const startButton = button(tr('开始跟随'), rightX, footerY, rightW - 116, start, true);
const stopButton = button(tr('停止'), rightX + rightW - 104, footerY, 104, cancel);

function setStatus(value, error = false) {
  write(status, value); ui.setColor(status, error ? colors.error : colors.secondary);
}
function controls(busy = dl.status().busy) {
  enable(startButton, !busy && !switching && devices.length > 0);
  enable(stopButton, !!busy && !switching);
  enable(source, !busy && !switching);
}
function refreshDevices() {
  const selected = devices[deviceIndex];
  try { devices = system.camera.devices().filter(d => d.transport === 'usb' && d.previewSupported); }
  catch (_) { devices = []; }
  const index = selected ? devices.findIndex(d => d.id === selected.id && d.generation === selected.generation) : 0;
  deviceIndex = Math.max(0, index); lastDeviceCheck = Date.now(); showDevice();
}
function showDevice() {
  write(source, devices.length ? Array.from(devices[deviceIndex].name).slice(0, 32).join('') : tr('未连接 USB 相机 · 点击刷新'));
  write(emptyTitle, devices.length ? tr('相机已就绪') : tr('等待连接相机'));
  write(emptyHint, devices.length ? (mode ? tr('点击开始跟随，查看实时画面') : tr('将物体对准相机，点击拍摄并分类')) : tr('连接 USB 相机后即可开始'));
  controls();
}
function cycleDevice() {
  if (dl.status().busy || switching) return;
  refreshDevices();
  if (devices.length > 1) deviceIndex = (deviceIndex + 1) % devices.length;
  showDevice();
}
function resetRows() {
  rows.forEach((row, i) => {
    write(row.name, mode ? [tr('横向偏移'), tr('纵向偏移'), tr('画面内人脸')][i] : tr('候选类别 ') + (i + 1));
    write(row.value, '--'); ui.setHidden(row.bar, true);
  });
}
function describeMode() {
  buttons.forEach((id, i) => {
    ui.setColor(id, mode === i ? colors.accent : colors.card);
    ui.setStyle(id, {borderWidth: 0, textColor: mode === i ? colors.onAccent : colors.secondary});
  });
  write(startButton, mode ? tr('开始跟随') : tr('拍摄并分类'));
  write(modeIcon, mode ? '\uf06e' : '\uf03e');
  write(modeHint, mode ? tr('人脸跟随 · 实时本地推理') : tr('物体分类 · 拍摄一帧识别'));
  write(section, mode ? tr('跟随状态') : tr('分类结果'));
  write(title, mode ? tr('让人脸进入画面') : tr('发现眼前的物体'));
  write(detail, mode ? tr('自动标记目标，持续显示位置与方向') : tr('拍摄一帧，查看最可能的三种类别'));
  resetRows(); showDevice();
}
function selectMode(next) {
  if ((next !== 0 && next !== 1) || mode === next) return;
  dl.cancel(); dl.clear(); request = frame = 0; switching = dl.status().busy; mode = next;
  showPreview(false); afterStop = names[mode] + tr(' · 已就绪');
  describeMode(); setStatus(switching ? tr('正在切换模式…') : afterStop);
}
function errorMessage(error) {
  if (error === -16) return tr('设备正忙，请稍后重试');
  if (error === -19 || error === -116) return tr('相机已断开，请重新连接');
  if (error === -2 || error === -74) return tr('识别模型不可用，请检查模型包');
  if (error === -12) return tr('内存不足，请关闭应用后重试');
  if (error === -110) return tr('相机取帧超时，请检查连接');
  if (error === -95) return tr('暂不支持当前相机模式');
  if (error === -125) return tr('已停止');
  return tr('识别失败（') + error + tr('），可重试');
}
function start() {
  if (dl.status().busy || switching) return;
  refreshDevices();
  const device = devices[deviceIndex];
  if (!device) { setStatus(tr('请连接 USB 相机后重试'), true); return; }
  dl.clear(); showPreview(false); frame = 0; resetRows();
  fpsStart = Date.now(); fpsFrame = 0; fps = '--';
  request = dl.start(mode, device.id, device.generation);
  if (request <= 0) { setStatus(errorMessage(request), true); request = 0; controls(); return; }
  write(title, mode ? tr('正在寻找人脸') : tr('正在识别物体'));
  write(detail, mode ? tr('让人脸正对相机，并保持光线充足') : tr('请稍候，结果将在这里显示'));
  write(emptyTitle, tr('正在获取画面')); write(emptyHint, tr('请保持相机连接')); controls(); tick();
}
function cancel() {
  dl.cancel(); dl.clear(); request = frame = 0; switching = dl.status().busy;
  afterStop = tr('已停止 · 可以重新开始'); showPreview(false); describeMode();
  setStatus(switching ? tr('正在停止…') : afterStop); controls();
}
function showResult(state) {
  if (mode === 0) {
    write(title, state.items.length ? state.items[0].label : tr('没有识别结果'));
    write(detail, tr('前三个候选类别 · 分数表示模型置信度'));
    rows.forEach((row, i) => {
      const item = state.items[i];
      write(row.name, item ? item.label : tr('候选类别 ') + (i + 1));
      write(row.value, item ? (item.score * 100).toFixed(1) + '%' : '--');
      ui.setHidden(row.bar, !item || item.score <= 0);
      if (item) ui.setSize(row.bar, Math.max(1, Math.round((rightW - 48) * Math.min(1, Math.max(0, item.score)))), 2);
    });
    return;
  }
  const t = state.track, tracking = t.state === 1;
  if (!tracking) {
    write(title, t.state === 2 ? tr('目标暂时丢失') : tr('正在寻找人脸'));
    write(detail, t.state === 2 ? tr('等待原目标重新进入画面') : tr('请正对相机，并保持光线充足'));
  } else {
    const direction = [];
    if (t.offsetX) direction.push(t.offsetX > 0 ? tr('向右') : tr('向左'));
    if (t.offsetY) direction.push(t.offsetY > 0 ? tr('向下') : tr('向上'));
    write(title, direction.join(' / ') || tr('目标已居中'));
    write(detail, tr('正在跟随目标 ') + t.id + tr(' · 绿色框标记位置'));
  }
  write(rows[0].value, tracking ? (t.offsetX / 10).toFixed(1) + '%' : '--');
  write(rows[1].value, tracking ? (t.offsetY / 10).toFixed(1) + '%' : '--');
  write(rows[2].value, state.items.length + tr(' 张'));
}
function tick() {
  const state = dl.status();
  if (switching) {
    if (!state.busy) { switching = false; setStatus(afterStop); controls(false); }
    return;
  }
  if (!request || state.request !== request) {
    if (!state.busy && Date.now() - lastDeviceCheck >= 2000) refreshDevices();
    return;
  }
  if (state.error) {
    request = 0; dl.clear(); showPreview(false); resetRows();
    write(title, state.error === -125 ? tr('已停止') : tr('暂时无法识别'));
    write(detail, errorMessage(state.error)); setStatus(errorMessage(state.error), state.error !== -125);
    refreshDevices(); return;
  }
  if (state.preview && state.frame !== frame) {
    // Only display results after their matching image has been copied successfully.
    if (!dl.render(request, state.frame)) return;
    frame = state.frame; showPreview(true); showResult(state);
    const now = Date.now();
    if (now - fpsStart >= 1000) {
      fps = ((frame - fpsFrame) * 1000 / (now - fpsStart)).toFixed(1);
      fpsStart = now; fpsFrame = frame;
    }
    setStatus(mode ? tr('正在跟随 · ') + fps + ' fps' : tr('分类完成 · 可再次拍摄'));
  } else if (!frame && state.busy) {
    setStatus(['', tr('正在采集画面…'), tr('正在准备识别…'), tr('正在识别…')][state.stage] || tr('正在处理…'));
  }
  controls(state.busy);
  if (!state.busy) request = 0;
}
describeMode(); refreshDevices(); setInterval(tick, 50);
globalThis.EspDlApp = {start, cancel, selectMode, status: () => dl.status()};
