'use strict';

const hosted = !!(system.pet && typeof system.pet.getState === 'function');
function notifyToast(text) { if (text) prompt.showToast(text); }
const backend = hosted ? system.pet : createPet({storage: system.storage, toast: notifyToast});
const view = ui.getSize(), W = view.width, H = view.height;
const dark = typeof ui.primary === 'number' ? ui.primary > 0x888888 : true;
// Match glass_sidebar.h and the native settings / music / HA navigation.
const sidebar = {width: 164, pad: 12, itemWidth: 140, itemHeight: 44, gap: 8, radius: 12};
const colors = {
  page: dark ? 0x19191f : 0xf0ece9, paper: dark ? 0x22252f : 0xffffff,
  soft: dark ? 0x2e303d : 0xf3f2f7, ink: dark ? 0xeeeef5 : 0x292c38,
  muted: dark ? 0xa6a7b6 : 0x737280, accent: dark ? 0xc1aedb : 0x77658f,
  warning: dark ? 0xe7ba80 : 0x98672e
};
if(ui.theme)Object.assign(colors,{page:ui.theme.background,paper:ui.theme.card,soft:ui.theme.surface,
  ink:ui.theme.primary,muted:ui.theme.secondary,accent:ui.theme.accent,warning:ui.theme.warning});
const x = sidebar.width + 20, width = W - x - 20;
const inset = 18, gap = 12, compact = H < 450;
const texts = new Map(), fills = new Map(), styles = new Map();
const pages = {company: [], food: [], appearance: [], settings: []};
const choices = [], nav = [];
let building = '', lastSeq = -1;

function keep(id) { if (building) pages[building].push(id); return id; }
function label(value, px, py, w, h, font, color, wrap) {
  const id = keep(ui.text(value, px, py, font || 16, color === undefined ? colors.ink : color));
  ui.setSize(id, w, h); ui.setStyle(id, {ellipsis: !wrap}); texts.set(id, value); return id;
}
function panel(px, py, w, h, color, radius) {
  const id = keep(ui.panel(px, py, w, h, color === undefined ? colors.paper : color));
  ui.setStyle(id, {radius: radius === undefined ? 18 : radius, borderWidth: 0}); return id;
}
function button(value, px, py, w, action, primary) {
  const id = keep(ui.button(value, px, py, w, 44, action, primary ? colors.accent : colors.soft));
  ui.setStyle(id, {radius: 12, borderWidth: 0, fontSize: 16,
    textColor: primary ? colors.paper : colors.ink}); texts.set(id, value); return id;
}
function write(id, value) {
  value = String(value);
  if (texts.get(id) !== value) { ui.setText(id, value); texts.set(id, value); }
}
function fill(id, color) {
  if (fills.get(id) !== color) { ui.setColor(id, color); fills.set(id, color); }
}
function selected(id, active, navigation) {
  const key = String(active) + ':' + String(navigation);
  if (styles.get(id) === key) return;
  fill(id, active || !navigation ? colors.soft : colors.paper);
  ui.setStyle(id, {textColor: active ? colors.accent : colors.ink,
    borderColor: colors.accent, borderWidth: active && !navigation ? 1 : 0});
  styles.set(id, key);
}
function selectPage(name) {
  if (!Object.prototype.hasOwnProperty.call(pages, name)) return;
  Object.keys(pages).forEach(key => pages[key].forEach(id => ui.setHidden(id, key !== name)));
  nav.forEach(row => selected(row.id, row.key === name, true));
  write(heading, ({company: '陪伴', food: '喂食', appearance: '外观', settings: '设置'})[name]);
  write(subheading, ({company: '一起散步，聊聊今天', food: '给海鸥一点好吃的',
    appearance: '圆团海鸥 · 内置形象', settings: '让陪伴更合你的心意'})[name]);
  render();
}

ui.background(colors.page);
panel(0, 0, sidebar.width, H, colors.paper, 0);
label('海鸥', 24, 28, 116, 30, 22);
label('桌面伙伴', 24, 64, 116, 24, 14, colors.muted);
[['company', '陪伴'], ['food', '喂食'], ['appearance', '外观'], ['settings', '设置']].forEach((entry, i) => {
  const id = button(entry[1], sidebar.pad, 110 + i * (sidebar.itemHeight + sidebar.gap),
    sidebar.itemWidth, () => selectPage(entry[0]));
  ui.setStyle(id, {radius: sidebar.radius, fontSize: 14});
  nav.push({id, key: entry[0]});
});
label('陪你摸鱼每一天', 24, H - 44, 124, 22, 14, colors.muted);
const heading = label('陪伴', x, 22, width, 32, 28);
const subheading = label('一起散步，聊聊今天', x, 60, width, 24, 14, colors.muted);

// The shared status card stays visible when changing sections.
const summaryY = 98, summaryH = compact ? 128 : 158;
panel(x, summaryY, width, summaryH);
const status = label('', x + inset, summaryY + 14, width - 142, 28, 20);
const visibility = label('', x + width - 118, summaryY + 18, 100, 24, 14, colors.muted);
ui.setStyle(visibility, {rightAlign: true});
const bubble = label('', x + inset, summaryY + 48, width - inset * 2,
  compact ? 36 : 60, 16, colors.ink, true);
const meterY = summaryY + summaryH - 34, meterW = Math.floor((width - inset * 2 - 24) / 2);
function meter(name, px, color) {
  const value = label('', px, meterY, meterW, 22, 14, colors.muted);
  panel(px, meterY + 24, meterW, 4, colors.soft, 2);
  return {name, value, bar: panel(px, meterY + 24, meterW, 4, color, 2), valueNow: -1};
}
const mood = meter('心情', x + inset, colors.accent);
const hunger = meter('饥饿', x + inset + meterW + 24, colors.warning);
const bodyY = summaryY + summaryH + gap, bodyH = H - bodyY - 18;
const innerX = x + inset, innerW = width - inset * 2;

building = 'company';
panel(x, bodyY, width, bodyH);
label('活动方式', innerX, bodyY + 12, innerW, 24, 16, colors.muted);
const third = Math.floor((innerW - gap * 2) / 3);
[['wander', '散步'], ['follow', '跟随'], ['still', '待着']].forEach((entry, i) => {
  choices.push({kind: 'mode', value: entry[0], id: button(entry[1], innerX + i * (third + gap),
    bodyY + 42, third, () => setMode(entry[0]))});
});
const actionY = compact ? bodyY + 100 : bodyY + bodyH - 60;
const fourth = Math.floor((innerW - gap * 3) / 4);
[['戳一下', poke], ['说句话', talk], ['聊天', chat], ['天气', weather]].forEach((entry, i) =>
  button(entry[0], innerX + i * (fourth + gap), actionY, fourth, entry[1], i === 2));

building = 'food';
panel(x, bodyY, width, bodyH);
label('今日小食堂', innerX, bodyY + 12, innerW, 24, 16, colors.muted);
const foods = [['fries', '薯条'], ['fish', '小鱼干'], ['cake', '蛋糕'], ['candy', '棒棒糖'], ['dango', '团子'], ['gem', '钻石']];
const foodColumns = width >= 600 ? 5 : 3, foodW = Math.floor((innerW - gap * (foodColumns - 1)) / foodColumns);
foods.forEach((entry, i) => button(entry[1], innerX + i % foodColumns * (foodW + gap),
  bodyY + 42 + Math.floor(i / foodColumns) * 56, foodW, () => feed(entry[0]), i === 0));

building = 'settings';
panel(x, bodyY, width, bodyH);
label('桌宠体型', innerX, bodyY + 12, innerW, 24, 16, colors.muted);
[['small', '小'], ['medium', '中'], ['large', '大']].forEach((entry, i) => {
  choices.push({kind: 'size', value: entry[0], id: button(entry[1], innerX + i * (third + gap),
    bodyY + 42, third, () => setSize(entry[0]))});
});
const toggleButton = button('', innerX, actionY, Math.min(180, innerW), toggle);
if (!compact) label(hosted ? '桌面长按海鸥，即可说话' : '戳一戳，和海鸥打个招呼',
  innerX + 196, actionY + 9, Math.max(0, innerW - 196), 26, 14, colors.muted);

building = 'appearance';
panel(x, bodyY, width, bodyH);
label('海鸥形象', innerX, bodyY + 12, innerW, 28, 20);
const appearanceStatus = label('', innerX, bodyY + 46, innerW, 44, 14, colors.muted, true);
button('恢复海鸥形象', innerX, actionY, Math.min(180, innerW), loadAppearance, true);
building = '';

function modeLabel(mode) {
  return ({wander: '自由散步', follow: '跟随指针', still: '原地待着'})[mode] || '自由散步';
}
function updateMeter(widget, raw) {
  const value = Math.round(Math.max(0, Math.min(100, Number(raw) || 0)));
  write(widget.value, widget.name + '  ' + value);
  if (widget.valueNow !== value) {
    ui.setSize(widget.bar, Math.max(1, Math.round(meterW * value / 100)), 4);
    ui.setHidden(widget.bar, value === 0); widget.valueNow = value;
  }
}
function render() {
  const state = backend.getState();
  write(status, modeLabel(state.mode));
  write(visibility, state.visible ? '陪伴中' : '已隐藏');
  write(bubble, state.bubble || '附近有薯条吗？本鸥就问问。');
  updateMeter(mood, state.mood); updateMeter(hunger, state.hunger);
  choices.forEach(row => selected(row.id, row.value === state[row.kind], false));
  write(toggleButton, state.visible ? '隐藏桌宠' : '显示桌宠');
  write(appearanceStatus, state.appearance === 'seagull' ? '已加载 · 正面、背面与侧面随行走切换' :
    hosted ? '内置海鸥形象，可点击恢复' : '海鸥形象已内置设备固件');
  if (hosted && state.eventSeq !== lastSeq) {
    lastSeq = state.eventSeq;
    if (state.eventSeq > 0 && state.toast) notifyToast(state.toast);
  }
  return state;
}
function setMode(mode) { backend.setMode(mode); return render(); }
function poke() { backend.poke(); return render(); }
function feed(food) { backend.feed(food); return render(); }
function talk() { backend.say('你的薯条很好，现在归鸥了。'); return render(); }
function chat() {
  prompt.input({title: '给海鸥发消息', placeholder: '说点什么', maxLength: 40}, value => {
    if (value && value.trim()) backend.chat(value.trim());
    render();
  });
}
function toggle() { backend.toggle(); return render(); }
function setSize(size) { backend.setSize(size); return render(); }
function weather() {
  if (backend.weather) backend.weather(); else backend.chat('天气');
  return render();
}
function loadAppearance() {
  if (typeof backend.loadAppearance !== 'function') {
    notifyToast('当前运行环境不支持恢复海鸥形象');
    return false;
  }
  const ok = backend.loadAppearance() === true;
  notifyToast(ok ? '海鸥形象已加载' : '恢复失败，请重新打开桌宠');
  render();
  return ok;
}
function tick() {
  if (!hosted && backend.tick) backend.tick(1000);
  if (typeof ui.isVisible !== 'function' || ui.isVisible()) render();
}
selectPage('company');
setInterval(tick, 1000);
globalThis.Dafeiyu = {
  getState: () => backend.getState(), setMode, poke, feed, talk,
  chat: text => { backend.chat(text); return render(); }, toggle, setSize, weather, tick, selectPage, loadAppearance
};
