'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

const widgets = [];
const intervals = [];
const intervalDelays = [];
const textWrites = [];
const storage = {};
let touchHandler;
let promptHandler;
let promptOptions;
let nextInput = null;
let promptThrows = false;
let submitAccepts = true;
let imageAccepts = true;
let statusReads = 0;
let inputReads = 0;
const inputCalls = [];
const calls = {navigate: [], viewport: null, pointer: [], images: [], zoom: [], reading: []};
const browserStatus = {
  available: true,
  loading: false,
  canBack: true,
  canForward: false,
  url: 'about:blank',
  title: 'Bing',
  message: ''
};
const browser = {
  navigate(url) { calls.navigate.push(url); browserStatus.url = url; return true; },
  back() { return true; },
  forward() { return true; },
  reload() { return true; },
  stop() { return true; },
  setZoom(value) { calls.zoom.push(value); return true; },
  setReadingEnabled(value) {
    calls.reading.push(value); browserStatus.readingActive = !!value && !!browserStatus.readingAvailable; return true;
  },
  setViewport(x, y, width, height) { calls.viewport = {x, y, width, height}; return true; },
  pointer(state, x, y) { calls.pointer.push({state, x, y}); return true; },
  status() { statusReads++; return browserStatus; },
  takeInput() { inputReads++; const request = nextInput; nextInput = null; return request; },
  submitInput(id, text) { inputCalls.push({id, text}); return submitAccepts; },
  setImagesVisible(value) {
    calls.images.push(value);
    if (!imageAccepts) return false;
    browserStatus.imagesVisible = value;
    return true;
  }
};
const ui = {
  getSize() { return {width: 1024, height: 600}; },
  background() {},
  text(text) { const widget = {text}; widgets.push(widget); return widget; },
  button(text, x, y, width, height, callback) {
    const widget = {text, x, y, width, height, callback};
    widgets.push(widget);
    return widget;
  },
  setText(widget, text) { textWrites.push({widget, text}); widget.text = text; },
  onTouch(callback) { touchHandler = callback; }
};
const context = {
  ui,
  system: {
    netsurf: browser,
    storage: {
      get(key) { return storage[key] || ''; },
      set(key, value) { storage[key] = value; }
    }
  },
  prompt: {
    cancelNotification: true,
    input(options, callback) {
      if (promptThrows) throw new Error('dialog unavailable');
      promptOptions = options; promptHandler = callback;
    }
  },
  setInterval(callback, delay) {
    intervals.push(callback); intervalDelays.push(delay); return intervals.length;
  }
};

function assertStableText(refresh) {
  const before = textWrites.length;
  for (let i = 0; i < 10; i++) refresh();
  assert.equal(textWrites.length, before, 'unchanged state must not rewrite UI text');
}

const manifest = JSON.parse(fs.readFileSync(__dirname + '/manifest.json', 'utf8'));
assert.equal(manifest.entry, 'app.js');
assert.equal(manifest.router.entry, 'app.js');

vm.runInNewContext(fs.readFileSync(__dirname + '/app.js', 'utf8'), {...context});

assert.deepEqual(calls.viewport, {x: 0, y: 172, width: 1024, height: 428});
assert.deepEqual(calls.navigate, ['https://www.bing.com/']);
assert.deepEqual(calls.images, [true]);
assert.equal(typeof touchHandler, 'function');
assert.deepEqual(intervalDelays, [500]);
const toolbar = [widgets[0], widgets[1], widgets[2], widgets[4], widgets[9]];
const imageButton = widgets[3];
assert.equal(imageButton.text, '有图');
const statusWidget = toolbar[4];
const pollsBefore = statusReads;
const inputsBefore = inputReads;
assertStableText(intervals[0]);
assert.equal(statusReads, pollsBefore + 10);
assert.equal(inputReads, inputsBefore + 10);

const initialStatus = {...browserStatus};
const writesBefore = textWrites.length;
Object.assign(browserStatus, {
  loading: true, canBack: false, canForward: true, url: 'https://changed.example/'
});
intervals[0]();
assert.deepEqual(textWrites.slice(writesBefore).map(({widget, text}) =>
  [toolbar.indexOf(widget), text]), [
  [3, 'https://changed.example/'], [4, '正在加载…'],
  [0, '·'], [1, '›'], [2, '停止']
]);
assertStableText(intervals[0]);
Object.assign(browserStatus, initialStatus);
intervals[0]();
assert.deepEqual(toolbar.map(widget => widget.text),
  ['‹', '·', '刷新', 'https://www.bing.com/', 'Bing']);
assertStableText(intervals[0]);
browserStatus.available = false;
browserStatus.message = '浏览器正在初始化…';
intervals[0]();
assert.equal(statusWidget.text, browserStatus.message);
assertStableText(intervals[0]);
Object.assign(browserStatus, initialStatus);
intervals[0]();

touchHandler('down', 0.5, 0.5);
touchHandler('down', 0.5, 0.1);
assert.deepEqual(calls.pointer, [{state: 'down', x: 512, y: 128}]);
touchHandler('move', 0.5, 0.05);
touchHandler('up', 0.5, 0.05);
assert.deepEqual(calls.pointer.slice(-2), [
  {state: 'move', x: 512, y: 0}, {state: 'up', x: 512, y: 0}
]);
const pointerCount = calls.pointer.length;
touchHandler('move', 0.5, 0.6);
touchHandler('down', NaN, 0.6);
assert.equal(calls.pointer.length, pointerCount);

const address = widgets.find(widget => widget.text === 'https://www.bing.com/');
assert.ok(address);
address.callback();
promptHandler('example.com');
assert.equal(calls.navigate[calls.navigate.length - 1], 'https://example.com');
assert.equal(storage['netsurf.lastUrl'], 'https://example.com');

intervals[0]();
assert.equal(statusWidget.text, 'Bing');
assertStableText(intervals[0]);

nextInput = {id: 17, maxLength: 8, password: false, value: '原值'};
intervals[0]();
assert.equal(promptOptions.value, '原值');
assert.equal(promptOptions.notifyCancel, true);
const formCallback = promptHandler;
address.callback();
assert.equal(promptHandler, formCallback); // serialize address and form prompts
const beforeModal = calls.pointer.length;
touchHandler('down', 0.5, 0.8);
touchHandler('up', 0.5, 0.8);
assert.equal(calls.pointer.length, beforeModal);
promptHandler('');
assert.deepEqual(inputCalls.at(-1), {id: 17, text: ''});
nextInput = {id: 18, maxLength: 120, password: true, value: 'must-not-show'};
intervals[0]();
assert.equal(promptOptions.password, true);
assert.equal(promptOptions.value, '');
promptHandler(null);
assert.deepEqual(inputCalls.at(-1), {id: 18, text: null});
address.callback();
promptHandler(null);
const afterCancel = calls.navigate.length;
address.callback(); promptHandler('example.com');
assert.equal(calls.navigate.length, afterCancel + 1);
nextInput = {id: 19, maxLength: 8, password: false, value: 'old'};
promptThrows = true; intervals[0](); promptThrows = false;
assert.deepEqual(inputCalls.at(-1), {id: 19, text: null});
assert.equal(statusWidget.text, '无法打开输入面板，请重新点击');
address.callback(); promptHandler(null); // failed prompts release the lock
intervals[0]();
assert.equal(statusWidget.text, 'Bing');
assertStableText(intervals[0]);
nextInput = {id: 20, maxLength: 8, password: false, value: 'old'};
intervals[0]();
submitAccepts = false; promptHandler('new'); submitAccepts = true;
assert.equal(statusWidget.text, '输入框已失效，请重新点击');
intervals[0]();
assert.equal(statusWidget.text, 'Bing');
assertStableText(intervals[0]);
promptThrows = true; address.callback(); promptThrows = false;
assert.equal(statusWidget.text, '无法打开输入面板');
intervals[0]();
assert.equal(statusWidget.text, 'Bing');
assertStableText(intervals[0]);
browserStatus.errorPage = true;
browserStatus.title = 'Connection timed out';
browserStatus.message = 'Done (0.1s)';
intervals[0]();
assert.ok(widgets.some(widget => widget.text === '网页加载失败：Connection timed out'));
assertStableText(intervals[0]);
browserStatus.errorPage = false;
browserStatus.title = 'Bing'; browserStatus.message = '';

const navigationsBeforeImageMode = calls.navigate.length;
const pointerCallsBeforeImageMode = calls.pointer.length;
imageButton.callback();
assert.equal(calls.images.at(-1), false);
assert.equal(imageButton.text, '无图');
assert.equal(storage['netsurf.imagesVisible'], '0');
assert.equal(calls.navigate.length, navigationsBeforeImageMode);
assert.equal(calls.pointer.length, pointerCallsBeforeImageMode);
imageAccepts = false;
imageButton.callback();
assert.equal(imageButton.text, '无图');
assert.equal(storage['netsurf.imagesVisible'], '0');
imageAccepts = true;
imageButton.callback();
assert.equal(imageButton.text, '有图');
assert.equal(storage['netsurf.imagesVisible'], '1');
intervals[0]();
assertStableText(intervals[0]);

const intervalCount = intervals.length;
vm.runInNewContext(fs.readFileSync(__dirname + '/app.js', 'utf8'), {
  ...context, system: {browserError: '浏览器模块与系统 ABI 不兼容'}
});
assert.equal(widgets[widgets.length - 1].text, '浏览器模块与系统 ABI 不兼容');
assert.equal(intervals.length, intervalCount + 1);
assertStableText(intervals.at(-1));

storage['netsurf.imagesVisible'] = '0';
const reopenStart = widgets.length;
vm.runInNewContext(fs.readFileSync(__dirname + '/app.js', 'utf8'), {...context});
assert.equal(calls.images.at(-1), false);
assert.equal(widgets[reopenStart + 3].text, '无图');
assertStableText(intervals.at(-1));

const legacyStart = widgets.length;
vm.runInNewContext(fs.readFileSync(__dirname + '/app.js', 'utf8'), {
  ...context, system: {...context.system, netsurf: {...browser, setImagesVisible: undefined}}
});
assert.equal(widgets[legacyStart + 3].text, '图片');
widgets[legacyStart + 3].callback();
assert.equal(widgets.at(-1).text, '请更新浏览器模块以使用无图模式');
assert.equal(storage['netsurf.imagesVisible'], '0');
const delayed = {browserLoading: true};
const navigations = calls.navigate.length;
vm.runInNewContext(fs.readFileSync(__dirname + '/app.js', 'utf8'), {
  ...context, system: delayed
});
assert.equal(widgets.at(-1).text, '正在加载浏览器模块…');
assertStableText(intervals.at(-1));
delayed.netsurf = browser;
intervals.at(-1)();
assert.equal(calls.navigate.length, navigations + 1);
intervals.at(-1)();
assert.equal(calls.navigate.length, navigations + 1);
assertStableText(intervals.at(-1));

vm.runInNewContext(fs.readFileSync(__dirname + '/app.js', 'utf8'), {
  ...context, prompt: {...context.prompt, cancelNotification: false}
});
nextInput = {id: 21, maxLength: 8, password: false, value: 'old'};
intervals.at(-1)();
assert.deepEqual(inputCalls.at(-1), {id: 21, text: null});
assert.equal(widgets.at(-1).text, '网页输入需要更新系统输入面板');
intervals.at(-1)();
assert.equal(widgets.at(-1).text, 'Bing');
assertStableText(intervals.at(-1));

browserStatus.readingAvailable = true;
browserStatus.zoomPercent = 100;
const readerStart = widgets.length;
vm.runInNewContext(fs.readFileSync(__dirname + '/app.js', 'utf8'), {...context});
const readerControls = widgets.slice(readerStart + 5, readerStart + 9);
const readerStatus = widgets[readerStart + 9];
assert.equal(readerControls[0].text, '原页');
readerControls[3].callback(); assert.equal(calls.zoom.at(-1), 125);
browserStatus.zoomPercent = 200;
readerControls[1].callback(); assert.equal(calls.zoom.at(-1), 175);
browserStatus.zoomPercent = 100;
readerControls[1].callback(); assert.equal(calls.zoom.at(-1), 100);
readerControls[2].callback(); assert.equal(calls.zoom.at(-1), 0);
readerControls[0].callback(); assert.equal(calls.reading.at(-1), false);
assert.equal(storage['netsurf.readingEnabled'], '0');
intervals.at(-1)(); assert.equal(readerControls[0].text, '阅读');
readerControls[0].callback(); assert.equal(calls.reading.at(-1), true);
intervals.at(-1)(); assert.equal(readerControls[0].text, '原页');
assert.match(readerStatus.text, /^100% · /);
assertStableText(intervals.at(-1));
for (const width of [320, 372]) {
  const start = widgets.length;
  vm.runInNewContext(fs.readFileSync(__dirname + '/app.js', 'utf8'), {
    ...context, ui: {...ui, getSize() { return {width, height: 660}; }}
  });
  for (const widget of widgets.slice(start).filter(w => w.callback)) {
    assert.ok(widget.x >= 0 && widget.x + widget.width <= width);
  }
  assert.equal(calls.viewport.y, 172);
}
console.log('NetSurf Quick App smoke test passed (stable text, input recovery, image/reading preferences and manual zoom)');
