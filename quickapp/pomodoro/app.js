'use strict';

const storage = system.storage;
const W = ui.getSize().width;
const hosted = !!(system.pomodoro && system.pomodoro.getState);
const CX = Math.floor(W / 2);
const FACE = 160;

function notifyToast(text) {
  if (text) prompt.showToast(text);
}

const backend = hosted ? system.pomodoro : createPomodoro({
  storage: storage,
  toast: notifyToast
});

let lastSeq = -1;

function phaseColor(phase) {
  if (phase === 'short') return 0x6bc4a6;
  if (phase === 'long') return 0x7aa2e3;
  return 0xe0b089;
}

ui.background(0x0b0f10);

const phaseLabel = ui.text('工作', 36, 16, 20, 0xe0b089);
const dots = [
  ui.rect(W - 108, 24, 10, 10, 0x2a3336),
  ui.rect(W - 90, 24, 10, 10, 0x2a3336),
  ui.rect(W - 72, 24, 10, 10, 0x2a3336),
  ui.rect(W - 54, 24, 10, 10, 0x2a3336)
];

const face = createPomodoroFace({
  x: CX - Math.floor(FACE / 2),
  y: 24,
  size: FACE,
  barX: CX - 160,
  barY: 286,
  barW: 320,
  onChange: function (msg) {
    if (typeof globalThis !== 'undefined' && globalThis.PomodoroFace) {
      globalThis.PomodoroFace(msg);
    }
  }
});

const clock = ui.number('25:00', CX - 150, 192, 300, 72, 0xe0b089);
const statusLabel = ui.text('已暂停', CX - 36, 258, 16, 0x8b989c);

const startBtn = ui.button('开始', CX - 90, 308, 180, 50, toggle, 0x1fa97a);
ui.button('跳过', CX - 168, 372, 88, 40, skip, 0x1e272a);
ui.button('重置', CX - 68, 372, 88, 40, reset, 0x1e272a);
ui.button('工作', CX + 32, 372, 88, 40, sceneWork, 0x1e272a);

ui.button('−', 36, 372, 44, 40, function () { addWork(-1); }, 0x1e272a);
const workVal = ui.text('25', 88, 382, 16, 0xc5d0d3);
ui.button('+', 122, 372, 44, 40, function () { addWork(1); }, 0x1e272a);

ui.button('−', W - 166, 372, 44, 40, function () { addBreak(-1); }, 0x1e272a);
const breakVal = ui.text('5', W - 114, 382, 16, 0xc5d0d3);
ui.button('+', W - 80, 372, 44, 40, function () { addBreak(1); }, 0x1e272a);

function statusLine(state) {
  if (state.running && state.phase === 'work') return '专注中';
  if (state.running && state.phase === 'short') return '休息中';
  if (state.running && state.phase === 'long') return '长休中';
  return '已暂停';
}

function render() {
  const state = backend.getState();
  const color = phaseColor(state.phase);
  ui.setText(phaseLabel, state.label);
  ui.setColor(phaseLabel, color);
  ui.setText(clock, state.clock);
  ui.setColor(clock, color);
  ui.setText(startBtn, state.running ? '暂停' : '开始');
  ui.setText(statusLabel, statusLine(state));
  ui.setText(workVal, '' + state.workMin);
  ui.setText(breakVal, '' + state.shortMin);
  const filled = state.completed % 4;
  for (let i = 0; i < 4; i++) {
    ui.setColor(dots[i], i < filled ? color : 0x2a3336);
  }
  face.setState(state);
  if (hosted && state.eventSeq !== lastSeq) {
    lastSeq = state.eventSeq;
    if (state.eventSeq > 0) notifyToast(state.toast);
  }
  return state;
}

function toggle() {
  backend.toggle();
  render();
}

function skip() {
  backend.skip();
  render();
}

function reset() {
  backend.reset();
  render();
}

function sceneWork() {
  backend.sceneWork();
  render();
}

function addWork(delta) {
  backend.addWork(delta);
  render();
}

function addBreak(delta) {
  backend.addBreak(delta);
  render();
}

function tick() {
  if (!hosted) backend.tick();
  render();
}

render();
setInterval(tick, 1000);
setInterval(function () { face.frame(); }, 80);
face.frame();

globalThis.PomodoroLayout = face.layout;
globalThis.Pomodoro = {
  toggle: toggle,
  skip: skip,
  reset: reset,
  tick: tick,
  addWork: addWork,
  addBreak: addBreak,
  sceneWork: sceneWork,
  poke: function () { face.poke(); },
  getExpression: function () { return face.getExpression(); },
  getState: function () { return backend.getState(); }
};
