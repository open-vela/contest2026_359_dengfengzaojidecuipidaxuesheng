const {test} = require('node:test');
const assert = require('node:assert/strict');
const vm = require('node:vm');
const fs = require('node:fs');
const path = require('node:path');
const root = path.join(__dirname, '..', 'overlay', 'apps', 'system', 'desktop');
const engine = fs.readFileSync(path.join(root, 'dafeiyu/engine.js'), 'utf8');
const ui = fs.readFileSync(path.join(root, 'dafeiyu/app.js'), 'utf8');
const code = engine + '\n' + ui;

function mockUi(labels, nextRef, extra) {
  function add(type, text, x, y, width, height, color, action) {
    const id = nextRef.n++;
    labels.set(id, {type, text, x, y, width, height, color, action, hidden: false, style: {}});
    return id;
  }
  const writes = {text: 0, style: 0, color: 0, size: 0};
  return {
    primary: extra.light ? 0x252d46 : 0xf4f2ff, writes,
    getSize: () => extra.size || ({width: 960, height: 470}),
    isVisible: () => extra.visible !== false,
    background(color) { this.backgroundColor = color; },
    panel: (x, y, w, h, color) => add('panel', '', x, y, w, h, color),
    text: (text, x, y, font, color) => add('text', text, x, y, 0, font, color),
    button: (text, x, y, w, h, action, color) => add('button', text, x, y, w, h, color, action),
    setText(id, text) { labels.get(id).text = text; writes.text++; },
    setColor(id, color) { labels.get(id).color = color; writes.color++; },
    setStyle(id, style) { Object.assign(labels.get(id).style, style); writes.style++; },
    setSize(id, width, height) { Object.assign(labels.get(id), {width, height}); writes.size++; },
    setHidden(id, hidden) { labels.get(id).hidden = hidden; }
  };
}

function setup(store = {}, extra = {}) {
  const labels = new Map();
  const toasts = [];
  const inputs = [];
  const nextRef = { n: 1 };
  let tick;
  const context = {
    ui: mockUi(labels, nextRef, extra),
    system: {
      storage: {
        get: (key) => store[key] == null ? null : store[key],
        set: (key, value) => { store[key] = String(value); },
        delete: (key) => { delete store[key]; }
      },
      pet: extra.pet
    },
    prompt: {
      showToast: (text) => toasts.push(text),
      dialog: () => {},
      input: (opts, cb) => {
        inputs.push(opts);
        if (extra.inputValue) cb(extra.inputValue);
      }
    },
    setInterval: (callback) => { tick = callback; return 1; },
    globalThis: {}
  };
  context.globalThis = context;
  vm.runInNewContext(code, context);
  return {
    api: context.Dafeiyu || context.globalThis.Dafeiyu,
    store, toasts, inputs, tick, labels, ui: context.ui, context,
    click(text) {
      const row = [...labels.values()].find(row => row.type === 'button' && row.text === text && !row.hidden);
      assert.ok(row, 'visible button: ' + text); row.action();
    }
  };
}

test('starts in wander with a visible pet', () => {
  const s = setup();
  const state = s.api.getState();
  assert.equal(state.mode, 'wander');
  assert.equal(state.visible, true);
  assert.equal(state.mood, 72);
});

test('poke and feed change bubble and meters', () => {
  const s = setup();
  s.api.poke();
  assert.ok(s.api.getState().bubble.length > 0);
  s.api.feed('fish');
  assert.match(s.api.getState().bubble, /小鱼干/);
  assert.ok(s.api.getState().hunger < 28);
});

test('chat uses local lexicon', () => {
  const s = setup();
  s.api.chat('你好');
  assert.match(s.api.getState().reply, /薯条/);
});

test('uses native system.pet when hosted', () => {
  const calls = [];
  const hosted = {
    getState: () => ({
      mode: 'still', dir: 'down', size: 'medium', facing: 1,
      x: 10, y: 10, w: 96, h: 88, mood: 80, hunger: 10,
      visible: true, dragging: false, jump: 0, eventSeq: 2,
      bubble: '本地鱼', inner: false, event: 'poke', reply: '', toast: 'hi'
    }),
    setMode: (mode) => { calls.push(['setMode', mode]); return hosted.getState(); },
    poke: () => { calls.push(['poke']); return hosted.getState(); },
    feed: (food) => { calls.push(['feed', food]); return hosted.getState(); },
    say: () => hosted.getState(),
    chat: () => hosted.getState(),
    toggle: () => hosted.getState(),
    setSize: () => hosted.getState()
  };
  const s = setup({}, {pet: hosted});
  s.api.setMode('follow');
  s.api.poke();
  s.api.feed('cake');
  assert.deepEqual(calls[0], ['setMode', 'follow']);
  assert.deepEqual(calls[1], ['poke']);
  assert.deepEqual(calls[2], ['feed', 'cake']);
  assert.equal(s.toasts.includes('hi'), true);
});

test('navigation keeps every control usable and reflects mode, size and visibility', () => {
  const s = setup({}, {inputValue: '你好'});
  s.click('跟随');
  assert.equal(s.api.getState().mode, 'follow');
  s.click('聊天');
  assert.match(s.api.getState().reply, /薯条/);
  s.click('喂食');
  for (const text of ['小鱼干', '蛋糕', '棒棒糖', '团子', '钻石']) s.click(text);
  s.click('设置'); s.click('大');
  assert.equal(s.api.getState().size, 'large');
  s.click('隐藏桌宠'); assert.equal(s.api.getState().visible, false);
  s.click('显示桌宠'); assert.equal(s.api.getState().visible, true);
  s.click('陪伴'); s.click('待着');
  assert.equal(s.api.getState().mode, 'still');
  const active = [...s.labels.values()].find(row => row.type === 'button' && row.text === '待着');
  assert.equal(active.style.borderWidth, 1);
  assert.ok(![...s.labels.values()].some(row => row.text === '小鱼干' && !row.hidden));
});

for (const size of [{width:640,height:400},{width:800,height:450},{width:1024,height:504},{width:1024,height:600}]) {
  for (const light of [true, false]) test(`theme and bounds ${size.width}x${size.height} light=${light}`, () => {
    const s = setup({}, {size, light});
    const native = fs.readFileSync(path.join(root, 'glass_sidebar.h'), 'utf8');
    for (const [name, value] of [['WIDTH',164],['PAD',12],['ITEM_WIDTH',140],['ITEM_HEIGHT',44],['GAP',8],['RADIUS',12]])
      assert.match(native, new RegExp(`GLASS_SIDEBAR_${name} ${value}\\b`));
    assert.equal(s.ui.backgroundColor, light ? 0xf0ece9 : 0x19191f);
    for (const page of ['company','food','appearance','settings']) {
      s.api.selectPage(page);
      const visible = [...s.labels.values()].filter(row => !row.hidden);
      for (const row of visible) {
        assert.ok(row.x >= 0 && row.y >= 0 && row.width > 0 && row.height > 0, row.text);
        assert.ok(row.x + row.width <= size.width && row.y + row.height <= size.height, 'clipped: ' + row.text);
      }
      const buttons = visible.filter(row => row.type === 'button');
      for (const row of buttons) assert.ok(row.width >= 80 && row.height >= 44, row.text);
      for (let i=0;i<buttons.length;i++) for(let j=i+1;j<buttons.length;j++) {
        const a=buttons[i],b=buttons[j];
        assert.ok(a.x+a.width<=b.x || b.x+b.width<=a.x || a.y+a.height<=b.y || b.y+b.height<=a.y, 'overlapping controls');
      }
    }
  });
}

test('unchanged hosted ticks do not rewrite widgets, allocate controls or repeat toast', () => {
  const state = {mode:'wander',size:'medium',visible:true,mood:72,hunger:28,eventSeq:3,toast:'hi'};
  const s = setup({}, {pet:{getState:()=>state}});
  const before = {...s.ui.writes}, count = s.labels.size;
  for (let i=0;i<120;i++) s.tick();
  assert.deepEqual(s.ui.writes, before);
  assert.equal(s.toasts.length, 1);
  for(let i=0;i<60;i++) for(const page of ['food','appearance','settings','company']) s.api.selectPage(page);
  assert.equal(s.labels.size, count);
});

test('seagull loading reports actual success and preserves an already loaded appearance on failure', () => {
  const state = {mode:'still',size:'medium',visible:true,mood:80,hunger:10,eventSeq:0,appearance:'fallback'};
  let ok = true;
  const s = setup({}, {pet:{getState:()=>state,loadAppearance(){if(ok)state.appearance='seagull';return ok;}}});
  s.click('外观'); s.click('恢复海鸥形象');
  assert.equal(s.api.getState().appearance, 'seagull');
  assert.equal(s.toasts.at(-1), '海鸥形象已加载');
  ok = false; s.click('恢复海鸥形象');
  assert.equal(s.api.getState().appearance, 'seagull');
  assert.match(s.toasts.at(-1), /失败/);
  const fallback = setup();
  assert.equal(fallback.api.loadAppearance(), false);
  assert.match(fallback.toasts.at(-1), /不支持/);
});

test('generated standalone script and firmware resource match the canonical sources', () => {
  const data = Buffer.from(code);
  assert.deepEqual(fs.readFileSync(path.join(root,'dafeiyu/qpk/app.js')), data);
  const resource = fs.readFileSync(path.join(root,'dafeiyu_resource.c'),'utf8').split('g_dafeiyu_app_js[] =')[1].split('};')[0];
  const bytes = Buffer.from([...resource.matchAll(/0x([0-9a-f]{2})/g)].map(match=>parseInt(match[1],16)));
  assert.deepEqual(bytes, Buffer.concat([data,Buffer.from([0])]));
});

test('seagull treats fries as food and answers with its own fries dialogue', () => {
  const s = setup();
  s.click('喂食'); s.click('薯条');
  assert.match(s.api.getState().bubble, /薯条/);
  assert.ok(s.api.getState().hunger < 28);
  s.api.chat('我的薯条呢'); assert.match(s.api.getState().reply, /薯条/);
  s.api.chat('困了'); assert.match(s.api.getState().reply, /薯条/);
});
