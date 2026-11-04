'use strict';

function createPet(options) {
  options = options || {};
  const LINES = [
    "本鸥路过，顺便接管你的薯条。",
    "你负责工作，我负责薯条。",
    "飞得不高，饭量不小。",
    "没有偷吃，是薯条先靠近我的。",
    "海很大，我的胃也不小。",
    "表面在巡逻，实际在找薯条。",
    "我不是胖，是羽绒服比较蓬。",
    "你继续忙，我替你盯着午饭。",
    "嘎一下，今天也要脆脆的。",
    "工作可以慢慢来，薯条凉了不行。",
    "这片桌面，归本鸥巡逻。",
    "你的薯条很好，现在归鸥了。"
];
  const FOOD = {
    fries: '嘎！这包薯条已被本鸥接管。',
    cake: '蛋糕！罪恶但快乐……',
    candy: '棒棒糖！转圈圈～',
    dango: '三色团子！软乎乎～',
    gem: '钻石？！这能吃吗……咕咚。真香！',
    fish: '小鱼干！我的最爱！'
  };
  let mode = 'wander';
  let dir = 'down';
  let size = 'medium';
  let x = 880;
  let y = 470;
  let w = 96;
  let h = 88;
  let mood = 72;
  let hunger = 28;
  let visible = true;
  let eventSeq = 0;
  let bubble = '';
  let inner = false;
  let lastEvent = '';
  let lastReply = '';
  let toast = '';
  let jump = 0;
  let rng = 1;

  function nextRand() {
    rng ^= rng << 13;
    rng ^= rng >>> 17;
    rng ^= rng << 5;
    rng >>>= 0;
    if (!rng) rng = 1;
    return rng;
  }

  function pick(list) {
    return list[nextRand() % list.length];
  }

  function emit(event, nextToast) {
    eventSeq += 1;
    lastEvent = event;
    toast = nextToast || '';
    if (options.toast && toast) options.toast(toast);
  }

  function say(text, isInner) {
    if (!text) return getState();
    bubble = text;
    inner = !!isInner;
    return getState();
  }

  function foodLine(food) {
    const key = String(food || 'fries');
    if (key.indexOf('fries') >= 0 || key.indexOf('薯条') >= 0) return FOOD.fries;
    if (key.indexOf('cake') >= 0 || key.indexOf('蛋糕') >= 0) return FOOD.cake;
    if (key.indexOf('candy') >= 0 || key.indexOf('糖') >= 0) return FOOD.candy;
    if (key.indexOf('dango') >= 0 || key.indexOf('团子') >= 0) return FOOD.dango;
    if (key.indexOf('gem') >= 0 || key.indexOf('钻石') >= 0) return FOOD.gem;
    return FOOD.fish;
  }

  function getState() {
    return {
      mode: mode,
      dir: dir,
      size: size,
      facing: 1,
      x: x,
      y: y,
      w: w,
      h: h,
      mood: mood,
      hunger: hunger,
      visible: visible,
      dragging: false,
      jump: jump,
      eventSeq: eventSeq,
      bubble: bubble,
      inner: inner,
      event: lastEvent,
      reply: lastReply,
      toast: toast
    };
  }

  function persist() {
    if (!options.storage) return;
    options.storage.set('dafeiyu', JSON.stringify({
      mode: mode, size: size, visible: visible, mood: mood, hunger: hunger
    }));
  }

  function restore() {
    if (!options.storage) return;
    const raw = options.storage.get('dafeiyu');
    if (!raw) return;
    try {
      const data = JSON.parse(raw);
      if (data.mode) mode = data.mode;
      if (data.size) size = data.size;
      if (typeof data.visible === 'boolean') visible = data.visible;
      if (typeof data.mood === 'number') mood = data.mood;
      if (typeof data.hunger === 'number') hunger = data.hunger;
    } catch (err) {
    }
  }

  restore();

  return {
    getState: getState,
    setMode: function (next) {
      mode = next === 'follow' || next === 'still' ? next : 'wander';
      emit('mode', mode === 'follow' ? '跟着你' : (mode === 'still' ? '原地待着' : '自由散步'));
      persist();
      return getState();
    },
    weather: function () {
      return this.chat('天气');
    },
    show: function () {
      visible = true;
      emit('show', '我回来了');
      persist();
      return getState();
    },
    hide: function () {
      visible = false;
      emit('hide', '我隐身了');
      persist();
      return getState();
    },
    toggle: function () {
      return visible ? this.hide() : this.show();
    },
    poke: function () {
      jump = 500;
      mood = Math.min(100, mood + 3);
      say(pick(LINES));
      emit('poke', bubble);
      persist();
      return getState();
    },
    feed: function (food) {
      const line = foodLine(food);
      hunger = Math.max(0, hunger - 18);
      mood = Math.min(100, mood + 8);
      jump = 400;
      say(line);
      emit('feed', line);
      persist();
      return getState();
    },
    say: function (text, isInner) {
      say(text, isInner);
      emit('say', text);
      return getState();
    },
    speak: function (text, isInner) {
      return this.say(text, isInner);
    },
    chat: function (text) {
      let reply = pick(LINES);
      const msg = String(text || '');
      if (!msg) reply = '嘎？是在叫我，还是在叫外卖？';
      else if (msg.indexOf('天气') >= 0) reply = '今天适合摸鱼，顺便等薯条。';
      else if (msg.indexOf('你好') >= 0 || msg.indexOf('嗨') >= 0) reply = '嘎！带薯条来了吗？';
      else if (msg.indexOf('薯条') >= 0 || msg.indexOf('fries') >= 0) reply = '薯条可以分你一根，袋子归我。';
      else if (msg.indexOf('饿') >= 0 || msg.indexOf('吃') >= 0) reply = '先来一包薯条，别忘了番茄酱。';
      else if (msg.indexOf('困') >= 0 || msg.indexOf('睡') >= 0) reply = '我睡一会儿，薯条帮我看好。';
      lastReply = reply;
      say(reply);
      emit('chat', reply);
      return getState();
    },
    setSize: function (next) {
      size = next === 'small' || next === 'large' ? next : 'medium';
      w = size === 'small' ? 72 : (size === 'large' ? 120 : 96);
      h = size === 'small' ? 66 : (size === 'large' ? 110 : 88);
      emit('size', size);
      persist();
      return getState();
    },
    follow: function (nx, ny) {
      x = nx;
      y = ny;
      mode = 'follow';
      emit('mode', '跟着你');
      persist();
      return getState();
    },
    tick: function (dt) {
      if (jump > 0) jump = Math.max(0, jump - (dt || 50));
      if (!visible) return getState();
      if (mode === 'wander') {
        x += (nextRand() % 5) - 2;
      } else if (mode === 'follow') {
        if (x < 480) x += 4;
        else if (x > 480) x -= 4;
      }
      return getState();
    }
  };
}

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
