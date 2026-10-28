'use strict';

const size = ui.getSize();
const width = size.width;
const height = size.height;
const pageTop = 172;
let browser = system.netsurf || null;
let savedUrl = '';
let imagesVisible = true;
let readingEnabled = true;
try {
  if (system.storage) savedUrl = system.storage.get('netsurf.lastUrl') || '';
  if (system.storage) imagesVisible = system.storage.get('netsurf.imagesVisible') !== '0';
  if (system.storage) readingEnabled = system.storage.get('netsurf.readingEnabled') !== '0';
} catch (_) {}
let currentUrl = savedUrl || 'https://www.bing.com/';
let promptBusy = false;
const cancelNotification = prompt.cancelNotification === true;

ui.background(0xf4f6f8);

const backButton = ui.button('‹', 16, 10, 52, 42, function () {
  if (browser) browser.back();
}, 0x263746);
const forwardButton = ui.button('›', 76, 10, 52, 42, function () {
  if (browser) browser.forward();
}, 0x263746);
const reloadButton = ui.button('刷新', 136, 10, 96, 42, reloadOrStop, 0x263746);
const imagesButton = ui.button('图片', 240, 10, 64, 42, toggleImages, 0x263746);
const addressButton = ui.button(currentUrl, 16, 60, width - 32, 40, editAddress, 0xffffff);
const readingButton = ui.button('阅读', 16, 106, 64, 36, toggleReading, 0x263746);
const smallerButton = ui.button('A−', 88, 106, 52, 36, function () { changeZoom(-25); }, 0x263746);
const fitButton = ui.button('适配', 148, 106, 76, 36, function () { requestZoom(0); }, 0x263746);
const largerButton = ui.button('A+', 232, 106, 52, 36, function () { changeZoom(25); }, 0x263746);
const statusText = ui.text('正在启动浏览器…', 16, 148, 15, 0x52616b);
const widgetText = new Map([
  [backButton, '‹'], [forwardButton, '›'], [reloadButton, '刷新'],
  [imagesButton, '图片'], [readingButton, '阅读'],
  [addressButton, currentUrl], [statusText, '正在启动浏览器…']
]);

function setText(widget, text) {
  if (widgetText.get(widget) === text) return;
  ui.setText(widget, text);
  widgetText.set(widget, text);
}

function normalizeAddress(value) {
  const text = String(value || '').trim();
  if (!text) return '';
  if (/^(?:https?|file|data|about):/i.test(text)) return text;
  if (/^[a-z][a-z0-9+.-]*:\/\//i.test(text)) return text;
  if (/^[^\s./]+\.[^\s]+(?::\d+)?(?:\/.*)?$/.test(text) || /^localhost(?::\d+)?(?:\/.*)?$/.test(text)) {
    return 'https://' + text;
  }
  return 'https://www.bing.com/search?q=' + encodeURIComponent(text);
}

function editAddress() {
  if (promptBusy) return;
  promptBusy = cancelNotification;
  try {
  prompt.input({
    title: '打开网页或搜索',
    placeholder: '网址或搜索内容',
    value: currentUrl,
    notifyCancel: true,
    maxLength: 240
  }, function (value) {
    promptBusy = false;
    const url = normalizeAddress(value);
    if (!url) return;
    currentUrl = url;
    setText(addressButton, currentUrl);
    if (!browser || !browser.navigate(url)) {
      setText(statusText, '浏览器服务尚未连接');
      return;
    }
    try {
      if (system.storage) system.storage.set('netsurf.lastUrl', url);
    } catch (_) {}
    setText(statusText, '正在加载…');
  });
  } catch (_) {
    promptBusy = false;
    setText(statusText, '无法打开输入面板');
  }
}

function editPageInput() {
  if (promptBusy || !browser.takeInput || !browser.submitInput) return;
  const request = browser.takeInput();
  if (!request) return;
  const owner = browser;
  if (!cancelNotification) {
    owner.submitInput(request.id, null);
    setText(statusText, '网页输入需要更新系统输入面板');
    return;
  }
  promptBusy = true;
  try {
    prompt.input({
      title: request.password ? '输入网页密码' : '编辑网页输入框',
      placeholder: request.password ? '重新输入密码；取消保留原值' : '网页输入内容',
      value: request.password ? '' : request.value,
      password: !!request.password,
      notifyCancel: true,
      maxLength: request.maxLength
    }, function (value) {
      promptBusy = false;
      // null means cancel; an empty string deliberately clears the field.
      if (!owner.submitInput(request.id, value == null ? null : String(value))) {
        setText(statusText, '输入框已失效，请重新点击');
      }
    });
  } catch (_) {
    promptBusy = false;
    owner.submitInput(request.id, null);
    setText(statusText, '无法打开输入面板，请重新点击');
  }
}

function reloadOrStop() {
  if (!browser) return;
  const state = browser.status();
  if (state.loading) browser.stop();
  else browser.reload();
}

function toggleImages() {
  if (promptBusy) return;
  if (!browser || typeof browser.setImagesVisible !== 'function') {
    setText(statusText, browser ? '请更新浏览器模块以使用无图模式' : '浏览器正在启动…');
    return;
  }
  const next = !imagesVisible;
  if (!browser.setImagesVisible(next)) {
    setText(statusText, '暂时无法切换图片，请重试');
    return;
  }
  imagesVisible = next;
  setText(imagesButton, imagesVisible ? '有图' : '无图');
  try {
    if (system.storage) system.storage.set('netsurf.imagesVisible', imagesVisible ? '1' : '0');
  } catch (_) {}
}

function requestZoom(percent) {
  if (promptBusy) return;
  if (!browser || typeof browser.setZoom !== 'function') {
    setText(statusText, '请更新浏览器模块以使用缩放');
    return;
  }
  if (!browser.setZoom(percent)) setText(statusText, '暂时无法缩放，请重试');
}

function changeZoom(delta) {
  if (!browser || promptBusy) return;
  const actual = browser.status().zoomPercent;
  const percent = Number.isFinite(actual) && actual > 0 ? actual : 100;
  const minimum = browser.status().readingActive ? 100 : 50;
  requestZoom(Math.max(minimum, Math.min(300, Math.round(percent / 25) * 25 + delta)));
}

function toggleReading() {
  if (promptBusy) return;
  if (!browser || typeof browser.setReadingEnabled !== 'function') {
    setText(statusText, '请更新浏览器模块以使用阅读排版');
    return;
  }
  const state = browser.status();
  if (!state.readingAvailable) {
    setText(statusText, '当前页面未识别到可重排的新闻正文');
    return;
  }
  const next = !state.readingActive;
  if (!browser.setReadingEnabled(next)) {
    setText(statusText, '暂时无法切换阅读排版，请重试');
    return;
  }
  readingEnabled = next;
  try {
    if (system.storage) system.storage.set('netsurf.readingEnabled', next ? '1' : '0');
  } catch (_) {}
}

function refresh() {
  if (!browser && system.netsurf) {
    browser = system.netsurf;
    browser.setViewport(0, pageTop, width, Math.max(1, height - pageTop));
    if (typeof browser.setImagesVisible === 'function' && browser.setImagesVisible(imagesVisible)) {
      setText(imagesButton, imagesVisible ? '有图' : '无图');
    }
    if (typeof browser.setReadingEnabled === 'function') browser.setReadingEnabled(readingEnabled);
    browser.navigate(currentUrl);
  }
  if (!browser) {
    setText(statusText, system.browserError ||
      (system.browserLoading ? '正在加载浏览器模块…' : '未安装 NetSurf 原生浏览器服务'));
    return;
  }
  const state = browser.status();
  if (!state.available) {
    setText(statusText, state.message || '浏览器正在初始化…');
    return;
  }
  if (state.url && state.url !== currentUrl) {
    currentUrl = state.url;
    setText(addressButton, currentUrl);
  }
  const message = state.loading ? '正在加载…' :
    (state.errorPage ? ('网页加载失败：' + (state.title || '请点击刷新重试')) :
      (state.message || state.title || '就绪'));
  setText(statusText, Number.isFinite(state.zoomPercent) && state.zoomPercent > 0 ?
    (state.zoomPercent + '% · ' + message) : message);
  setText(readingButton, state.readingActive ? '原页' : '阅读');
  setText(backButton, state.canBack ? '‹' : '·');
  setText(forwardButton, state.canForward ? '›' : '·');
  setText(reloadButton, state.loading ? '停止' : '刷新');
  editPageInput();
}

let pointerActive = false;
function pointer(state, nx, ny) {
  if (promptBusy) { pointerActive = false; return; }
  if (!browser || !Number.isFinite(nx) || !Number.isFinite(ny)) return;
  if (state === 'down') {
    if (ny * height < pageTop) return;
    pointerActive = true;
  } else if (!pointerActive) return;
  if (state === 'up' || state === 'cancel') pointerActive = false;
  const x = Math.max(0, Math.min(width - 1, Math.floor(nx * width)));
  const y = Math.max(0, Math.min(height - pageTop - 1,
    Math.floor(ny * height) - pageTop));
  browser.pointer(state, x, y);
}

ui.onTouch(pointer);
// The native loader publishes system.netsurf asynchronously on the UI thread.
browser = null;
refresh();
setInterval(refresh, 500);
