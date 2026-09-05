'use strict';

const storage = system.storage;
const ha = system.homeAssistant;
const size = ui.getSize();
const W = size.width;
const H = size.height;
const PAGE_SIZE = 4;
const COLS = 2;
const PAD = 24;
const GAP = 12;
const CARD_W = Math.floor((W - PAD * 2 - GAP * (COLS - 1)) / COLS);
const CARD_H = 88;
const GRID_Y = 108;
const HUB_BG = 0xfafafa;
const HUB_CARD = 0xffffff;
const HUB_PICK = 0xd7eef5;
const HUB_TEXT = 0x141414;
const HUB_MUTED = 0x5e5e5e;
const HUB_ACCENT = 0x009ac7;
const BTN_W = 96;
const BTN_H = 36;

let base = storage.get('ha_url') || 'http://homeassistant.local:8123';
let token = storage.get('ha_token') || '';
let entities = [];
let page = 0;
let selected = -1;
let pending = '';
let advancedOpen = false;
let advancedDomain = storage.get('ha_domain') || 'light';
let advancedService = storage.get('ha_service') || 'toggle';
let advancedData = storage.get('ha_data') ||
                   '{"entity_id":"light.living_room"}';
let catalog = [];
let slots = [];
try {
  slots = JSON.parse(storage.get('ha_slots') || '[]');
} catch (e) {
  slots = [];
}
if (!Array.isArray(slots)) slots = [];
let deleted = [];
try {
  deleted = JSON.parse(storage.get('ha_deleted') || '[]');
} catch (e) {
  deleted = [];
}
if (!Array.isArray(deleted)) deleted = [];

function slugify(text) {
  const src = String(text || '').toLowerCase();
  let out = '';
  let gap = false;
  let i;
  for (i = 0; i < src.length; i++) {
    const c = src.charCodeAt(i);
    const ok = (c >= 48 && c <= 57) || (c >= 97 && c <= 122);
    if (ok) {
      out += src.charAt(i);
      gap = false;
    } else if (out.length && !gap) {
      out += '_';
      gap = true;
    }
  }
  if (out.length && out.charAt(out.length - 1) === '_') {
    out = out.slice(0, -1);
  }
  return out || 'unknown';
}

function validSlug(text, isDomain) {
  const s = String(text || '');
  if (!s.length || s.charAt(0) === '_' || s.charAt(s.length - 1) === '_') {
    return false;
  }
  if (isDomain && s.indexOf('__') >= 0) return false;
  let i;
  for (i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    if (!((c >= 48 && c <= 57) || (c >= 97 && c <= 122) || c === 95)) {
      return false;
    }
  }
  return true;
}

function validEntityId(entityId) {
  const id = String(entityId || '');
  const i = id.indexOf('.');
  if (i < 1 || id.indexOf('.', i + 1) >= 0) return false;
  return validSlug(id.slice(0, i), true) && validSlug(id.slice(i + 1), false);
}

function splitEntityId(entityId) {
  const id = String(entityId || '');
  const i = id.indexOf('.');
  if (i < 1 || id.indexOf('.', i + 1) >= 0) return null;
  const domain = id.slice(0, i);
  const objectId = id.slice(i + 1);
  if (!domain || !objectId) return null;
  return [domain, objectId];
}

function randomUuidHex() {
  let s = '';
  let i;
  for (i = 0; i < 32; i++) {
    s += '0123456789abcdef'.charAt((Math.random() * 16) | 0);
  }
  return s;
}

function isUuidHex(value) {
  const s = String(value || '');
  if (s.length !== 32) return false;
  let i;
  for (i = 0; i < 32; i++) {
    const c = s.charCodeAt(i);
    if (!((c >= 48 && c <= 57) || (c >= 97 && c <= 102))) return false;
  }
  return true;
}

function normalizeEntityId(value) {
  const raw = String(value || '').trim().toLowerCase();
  const parts = splitEntityId(raw);
  if (!parts) return '';
  const id = slugify(parts[0]) + '.' + slugify(parts[1]);
  return validEntityId(id) ? id : '';
}

function normalizeEntry(raw, index) {
  raw = raw || {};
  let entityId = normalizeEntityId(raw.entity_id);
  if (!entityId) {
    const domain = slugify(raw.domain || 'sensor');
    const objectId = slugify(raw.unique_id || ('device_' + index));
    entityId = domain + '.' + objectId;
  }
  const parts = splitEntityId(entityId);
  const domain = parts[0];
  const objectId = parts[1];
  return {
    id: isUuidHex(raw.id) ? raw.id : randomUuidHex(),
    entity_id: entityId,
    unique_id: String(raw.unique_id || objectId),
    platform: slugify(raw.platform || 'hub'),
    domain: domain,
    name: raw.name || null,
    original_name: raw.original_name || raw.name || null,
    device_id: raw.device_id || null,
    area_id: raw.area_id || null
  };
}

function saveSlots() {
  storage.set('ha_slots', JSON.stringify(slots));
}

function saveDeleted() {
  storage.set('ha_deleted', JSON.stringify(deleted));
}

function findDeleted(domain, platform, uniqueId) {
  let i;
  for (i = 0; i < deleted.length; i++) {
    if (deleted[i].domain === domain &&
        deleted[i].platform === platform &&
        deleted[i].unique_id === uniqueId) {
      return i;
    }
  }
  return -1;
}

function pushDeleted(entry) {
  if (!entry || !entry.entity_id) return;
  const key = findDeleted(entry.domain, entry.platform, entry.unique_id);
  const row = {
    id: entry.id,
    entity_id: entry.entity_id,
    unique_id: entry.unique_id,
    platform: entry.platform,
    domain: entry.domain,
    name: entry.name || null,
    original_name: entry.original_name || null,
    area_id: entry.area_id || null,
    orphaned_timestamp: Date.now() / 1000
  };
  if (key >= 0) deleted[key] = row;
  else deleted.push(row);
  while (deleted.length > 8) deleted.shift();
  saveDeleted();
}

function isEnergyItem(item) {
  const id = String(item.entity_id || '');
  const dc = String((item.attributes || {}).device_class || '');
  if (dc === 'power' || dc === 'energy' || dc === 'voltage' ||
      dc === 'current') {
    return true;
  }
  return id.indexOf('sensor.electric') === 0 ||
         id.indexOf('energy') >= 0 ||
         id.indexOf('tesla_wall') >= 0;
}

function isRegistered(entityId) {
  let i;
  for (i = 0; i < slots.length; i++) {
    if (slots[i].entity_id === entityId) return true;
  }
  return false;
}

function findByKey(domain, platform, uniqueId) {
  let i;
  for (i = 0; i < slots.length; i++) {
    if (slots[i].domain === domain &&
        slots[i].platform === platform &&
        slots[i].unique_id === uniqueId) {
      return i;
    }
  }
  return -1;
}

function getAvailableEntityId(domain, suggested, current) {
  const preferred = domain + '.' + slugify(suggested);
  let test = preferred;
  let n = 1;
  while (isRegistered(test) && test !== current) {
    n++;
    test = preferred + '_' + n;
  }
  return test;
}

function getOrCreate(domain, platform, uniqueId, extras) {
  extras = extras || {};
  const hit = findByKey(domain, platform, uniqueId);
  if (hit >= 0) {
    if (extras.name) slots[hit].name = extras.name;
    if (extras.original_name) slots[hit].original_name = extras.original_name;
    if (extras.area_id) slots[hit].area_id = extras.area_id;
    if (extras.device_id) slots[hit].device_id = extras.device_id;
    return slots[hit];
  }
  const tomb = findDeleted(domain, platform, uniqueId);
  let restored = null;
  if (tomb >= 0) {
    restored = deleted[tomb];
    deleted.splice(tomb, 1);
    saveDeleted();
  }
  const suggested = extras.suggested_object_id || uniqueId;
  const wanted = restored && restored.entity_id;
  const entry = {
    id: restored && isUuidHex(restored.id) ? restored.id : randomUuidHex(),
    entity_id: wanted && !isRegistered(wanted) ?
               wanted : getAvailableEntityId(domain, suggested, null),
    unique_id: String(uniqueId),
    platform: slugify(platform || 'hub'),
    domain: domain,
    name: extras.name || (restored && restored.name) || null,
    original_name: extras.original_name || extras.name ||
                   (restored && restored.original_name) || null,
    device_id: extras.device_id || null,
    area_id: extras.area_id || (restored && restored.area_id) || null
  };
  slots.push(entry);
  return entry;
}

function writeUnavailable(slot) {
  const attrs = { restored: true };
  const name = slot.name || slot.original_name;
  if (name) attrs.friendly_name = name;
  return {
    entity_id: slot.entity_id,
    state: 'unavailable',
    attributes: attrs
  };
}

if (slots.length) {
  slots = slots.map(function (item, i) { return normalizeEntry(item, i); });
  saveSlots();
}

function applySlots(list) {
  catalog = Array.isArray(list) ? list : [];
  if (slots.length) {
    slots = slots.map(function (item, i) { return normalizeEntry(item, i); });
  } else if (catalog.length) {
    const n = catalog.length < 8 ? catalog.length : 8;
    let i;
    for (i = 0; i < n; i++) {
      const item = catalog[i];
      const entityId = normalizeEntityId(item.entity_id);
      const parts = splitEntityId(entityId);
      if (!parts) continue;
      const attrs = item.attributes || {};
      getOrCreate(parts[0], 'hub', parts[1], {
        suggested_object_id: parts[1],
        name: attrs.friendly_name || parts[1],
        original_name: attrs.friendly_name || parts[1],
        area_id: attrs.area_id || null
      });
    }
  }
  if (catalog.length) {
    let extra = 0;
    let i;
    for (i = 0; i < catalog.length && extra < 4; i++) {
      const item = catalog[i];
      if (!isEnergyItem(item)) continue;
      const entityId = normalizeEntityId(item.entity_id);
      const parts = splitEntityId(entityId);
      if (!parts || isRegistered(entityId)) continue;
      const attrs = item.attributes || {};
      getOrCreate(parts[0], 'hub', parts[1], {
        suggested_object_id: parts[1],
        name: attrs.friendly_name || parts[1],
        original_name: attrs.friendly_name || parts[1],
        area_id: attrs.area_id || '能源'
      });
      extra++;
    }
  }
  saveSlots();
  entities = slots.map(function (slot) {
    let hit = null;
    let j;
    for (j = 0; j < catalog.length; j++) {
      if (catalog[j].entity_id === slot.entity_id) {
        hit = catalog[j];
        break;
      }
    }
    return hit || writeUnavailable(slot);
  });
}

ui.background(HUB_BG);
const dateLabel = ui.text('家', PAD, 16, 22, HUB_TEXT);
const clockLabel = ui.text('', PAD, 46, 16, HUB_MUTED);
const weatherLabel = ui.text('16°  57%', W - 200, 20, 16, HUB_TEXT);
const statusLabel = ui.text('未连接', PAD, 76, 16, HUB_MUTED);
const pageLabel = ui.text('', W - 160, 76, 16, HUB_MUTED);
const rows = [];
let actionButtons = [];
let configButtons = [];
let advancedButtons = [];
let moreButton;

function setStatus(text) {
  ui.setText(statusLabel, text);
}

function clip(text, max) {
  const value = String(text || '');
  return value.length > max ? value.slice(0, max - 1) + '…' : value;
}

function entityName(item) {
  const attrs = item.attributes || {};
  return attrs.friendly_name || item.entity_id;
}

function isOn(state) {
  const key = String(state || '').toLowerCase();
  return key === 'on' || key === 'open' || key === 'unlocked' ||
         key === 'playing' || key === 'heat' || key === 'cool' ||
         key === 'auto';
}

function stateLabel(state) {
  const map = {
    on: '开启', off: '关闭',
    open: '已打开', opening: '打开中', closed: '已关闭', closing: '关闭中',
    locked: '已上锁', unlocked: '已解锁',

    playing: '正在播放', paused: '已暂停', idle: '待机',
    not_home: '离家', away: '离家', home: '在家',
    heat: '制热中', cool: '制冷中', auto: '自动',
    unavailable: '离线', unknown: '未知'
  };
  const key = String(state || '').toLowerCase();
  return map[key] || String(state || '未知');
}

function pad2(n) {
  return n < 10 ? '0' + n : String(n);
}

function paintClock() {
  const d = new Date();
  const weeks = ['星期日', '星期一', '星期二', '星期三',
                 '星期四', '星期五', '星期六'];
  ui.setText(dateLabel, '家');
  ui.setText(clockLabel, weeks[d.getDay()] + '  ' + d.getDate() + '  ' +
             pad2(d.getHours()) + ':' + pad2(d.getMinutes()));
}

function cardColor(index, item) {
  if (index === selected) return HUB_PICK;
  return HUB_CARD;
}

function render() {
  paintClock();
  for (let i = 0; i < PAGE_SIZE; i++) {
    const index = page * PAGE_SIZE + i;
    if (index < entities.length) {
      const item = entities[index];
      const unit = (item.attributes || {}).unit_of_measurement;
      const shown = unit ? stateLabel(item.state) + ' ' + unit :
                    stateLabel(item.state);
      ui.setText(rows[i], clip(entityName(item), 10) + '\n' +
                          clip(shown, 16));
      ui.setColor(rows[i], cardColor(index, item));
      ui.show(rows[i]);
    } else {
      ui.hide(rows[i]);
    }
  }
  const pages = Math.max(1, Math.ceil(entities.length / PAGE_SIZE));
  ui.setText(pageLabel, entities.length ?
             (page + 1) + ' / ' + pages : '');
  const selectedParts = selected >= 0 ?
                 splitEntityId(entities[selected].entity_id) : null;
  const domain = selectedParts ? selectedParts[0] : '';
  const labels = {
    cover: ['打开', '停止', '关闭'],
    lock: ['上锁', '开门', '解锁'],
    media_player: ['播放', '音量+', '音量-'],
    vacuum: ['启动', '停止', '回充'],
    climate: ['开启', '关闭', '切换'],
    scene: ['执行', '执行', '执行'],
    button: ['执行', '执行', '执行'],
    input_button: ['执行', '执行', '执行']
  };
  const names = labels[domain] || ['打开', '关闭', '切换'];
  for (let i = 0; i < actionButtons.length; i++) {
    ui.setText(actionButtons[i], names[i]);
    ui.setColor(actionButtons[i], i === 0 ? HUB_ACCENT : HUB_CARD);
  }
  for (let i = 0; i < configButtons.length; i++) {
    if (advancedOpen) ui.hide(configButtons[i]);
    else ui.show(configButtons[i]);
  }
  for (let i = 0; i < advancedButtons.length; i++) {
    if (advancedOpen) ui.show(advancedButtons[i]);
    else ui.hide(advancedButtons[i]);
  }
  ui.setText(moreButton, advancedOpen ? '收起' : '更多');
}

function credentialsReady() {
  if (!token) {
    setStatus('请先配置访问令牌');
    return false;
  }
  return true;
}

function discover() {
  if (!credentialsReady()) return;
  if (ha.get(base, token, 'states')) {
    pending = 'states';
    setStatus('正在同步…');
  } else {
    setStatus('请求忙或配置无效');
  }
}

function requestState() {
  if (selected < 0 || !credentialsReady()) return;
  if (ha.getState(base, token, entities[selected].entity_id)) {
    pending = 'state';
    setStatus('正在读取 ' + clip(entityName(entities[selected]), 12) + '…');
  }
}

function call(domain, service, data) {
  if (!credentialsReady()) return;
  let text;
  try {
    text = JSON.stringify(data || {});
  } catch (e) {
    setStatus('服务参数不是有效 JSON');
    return;
  }
  if (ha.callService(base, token, domain, service, text)) {
    pending = 'service';
    setStatus('执行 ' + domain + '.' + service + '…');
  } else {
    setStatus('请求忙或参数无效');
  }
}

function selectedCall(service, extra) {
  if (selected < 0) {
    setStatus('请先选择设备');
    return;
  }
  const id = entities[selected].entity_id;
  const parts = splitEntityId(id);
  const domain = parts ? parts[0] : '';
  const data = extra || {};
  data.entity_id = id;
  call(domain, service, data);
}

function contextual(action) {
  if (selected < 0) {
    setStatus('请先选择设备');
    return;
  }
  const actionParts = splitEntityId(entities[selected].entity_id);
  const domain = actionParts ? actionParts[0] : '';
  const services = {
    cover: ['open_cover', 'stop_cover', 'close_cover'],
    lock: ['lock', 'open', 'unlock'],
    media_player: ['media_play_pause', 'volume_up', 'volume_down'],
    vacuum: ['start', 'stop', 'return_to_base'],
    climate: ['turn_on', 'turn_off', 'toggle'],
    scene: ['turn_on', 'turn_on', 'turn_on'],
    script: ['turn_on', 'turn_off', 'toggle'],
    automation: ['turn_on', 'turn_off', 'toggle'],
    button: ['press', 'press', 'press'],
    input_button: ['press', 'press', 'press']
  };
  const names = services[domain] || ['turn_on', 'turn_off', 'toggle'];
  selectedCall(names[action]);
}

function previousPage() {
  if (page > 0) page--;
  render();
}

function nextPage() {
  if ((page + 1) * PAGE_SIZE < entities.length) page++;
  render();
}

function configureUrl() {
  prompt.input({title: 'Home Assistant 地址', value: base, maxLength: 120},
               function (value) {
    if (!value) return;
    base = value;
    storage.set('ha_url', base);
    setStatus('地址已保存');
  });
}

function configureToken() {
  prompt.input({title: '长期访问令牌', placeholder: '粘贴 Token',
                maxLength: 512, password: true}, function (value) {
    if (!value) return;
    token = value;
    storage.set('ha_token', token);
    setStatus('令牌已保存');
  });
}

function configureAdvancedData() {
  prompt.input({title: '服务 JSON 数据', value: advancedData, maxLength: 2047},
               function (value) {
    if (!value) return;
    try {
      JSON.parse(value);
    } catch (e) {
      setStatus('JSON 格式错误');
      return;
    }
    advancedData = value;
    storage.set('ha_data', advancedData);
    setStatus('服务数据已保存');
  });
}

function configureAdvancedService() {
  prompt.input({title: 'domain.service',
                value: advancedDomain + '.' + advancedService,
                maxLength: 96}, function (value) {
    const parts = (value || '').split('.');
    if (parts.length !== 2 || !parts[0] || !parts[1]) {
      setStatus('格式应为 domain.service');
      return;
    }
    advancedDomain = parts[0];
    advancedService = parts[1];
    storage.set('ha_domain', advancedDomain);
    storage.set('ha_service', advancedService);
    setStatus('高级服务已保存');
  });
}

function runAdvanced() {
  let data;
  try {
    data = JSON.parse(advancedData);
  } catch (e) {
    setStatus('JSON 格式错误');
    return;
  }
  call(advancedDomain, advancedService, data);
}

function toggleAdvanced() {
  advancedOpen = !advancedOpen;
  render();
}

function registerId() {
  if (selected < 0) {
    setStatus('请先选择设备');
    return;
  }
  const cur = entities[selected];
  prompt.input({
    title: 'entity_id',
    value: cur.entity_id,
    maxLength: 96
  }, function (value) {
    if (!value) return;
    const entityId = normalizeEntityId(value);
    const parts = splitEntityId(entityId);
    if (!parts) {
      setStatus('格式应为 domain.object_id');
      return;
    }
    let i;
    for (i = 0; i < slots.length; i++) {
      if (i !== selected && slots[i].entity_id === entityId) {
        setStatus('entity_id 已注册');
        return;
      }
    }
    const platform = (slots[selected] && slots[selected].platform) || 'hub';
    const uniqueId = parts[1];
    const hit = findByKey(parts[0], platform, uniqueId);
    if (hit >= 0 && hit !== selected) {
      setStatus('unique_id 已注册');
      return;
    }
    if (!slots[selected]) {
      getOrCreate(parts[0], platform, uniqueId, {
        suggested_object_id: uniqueId
      });
    } else {
      const prev = slots[selected];
      if (prev.domain !== parts[0] || prev.unique_id !== uniqueId) {
        pushDeleted(prev);
      }
      slots[selected].entity_id = entityId;
      slots[selected].domain = parts[0];
      slots[selected].unique_id = uniqueId;
      slots[selected].platform = platform;
      if (!isUuidHex(slots[selected].id)) slots[selected].id = randomUuidHex();
    }
    saveSlots();
    if (catalog.length) applySlots(catalog);
    else cur.entity_id = entityId;
    setStatus('已注册 ' + entityId);
    render();
  });
}

for (let i = 0; i < PAGE_SIZE; i++) {
  const col = i % COLS;
  const row = Math.floor(i / COLS);
  const x = PAD + col * (CARD_W + GAP);
  const y = GRID_Y + row * (CARD_H + GAP);
  rows.push(ui.button('—', x, y, CARD_W, CARD_H, function () {
    const index = page * PAGE_SIZE + i;
    if (index >= entities.length) return;
    selected = index;
    const cardParts = splitEntityId(entities[index].entity_id);
    const domain = cardParts ? cardParts[0] : '';
    render();
    if (domain === 'light' || domain === 'switch') {
      contextual(2);
      return;
    }
    requestState();
  }, HUB_CARD));
}

function rowY(which) {
  return H - (which === 0 ? 100 : 52);
}

function rowX(index, count) {
  const total = count * BTN_W + (count - 1) * 10;
  return Math.floor((W - total) / 2) + index * (BTN_W + 10);
}

ui.button('同步', rowX(0, 6), rowY(0), BTN_W, BTN_H, discover, HUB_CARD);
ui.button('上页', rowX(1, 6), rowY(0), BTN_W, BTN_H, previousPage, HUB_CARD);
ui.button('下页', rowX(2, 6), rowY(0), BTN_W, BTN_H, nextPage, HUB_CARD);
actionButtons = [
  ui.button('打开', rowX(3, 6), rowY(0), BTN_W, BTN_H, () => contextual(0),
            HUB_ACCENT),
  ui.button('关闭', rowX(4, 6), rowY(0), BTN_W, BTN_H, () => contextual(1),
            HUB_CARD),
  ui.button('注册', rowX(5, 6), rowY(0), BTN_W, BTN_H, registerId,
            HUB_CARD)
];
configButtons = [
  ui.button('地址', rowX(0, 4), rowY(1), BTN_W, BTN_H, configureUrl, HUB_CARD),
  ui.button('令牌', rowX(1, 4), rowY(1), BTN_W, BTN_H, configureToken, HUB_CARD)
];
moreButton = ui.button('更多', rowX(3, 4), rowY(1), BTN_W, BTN_H,
                       toggleAdvanced, HUB_CARD);
advancedButtons = [
  ui.button('服务名', rowX(0, 4), rowY(1), BTN_W, BTN_H,
            configureAdvancedService, HUB_CARD),
  ui.button('JSON', rowX(1, 4), rowY(1), BTN_W, BTN_H,
            configureAdvancedData, HUB_CARD),
  ui.button('执行', rowX(2, 4), rowY(1), BTN_W, BTN_H, runAdvanced, HUB_ACCENT)
];

ui.onSwipe(function (dir) {
  if (dir === 'left') nextPage();
  else if (dir === 'right') previousPage();
});

let streamAt = Date.now();

function subscribeStates() {
  if (!token || pending) return false;
  if (ha.get(base, token, 'states')) {
    pending = 'stream';
    streamAt = Date.now();
    return true;
  }
  return false;
}

setInterval(function () {
  paintClock();
  if (!pending && token && Date.now() - streamAt > 5000) {
    subscribeStates();
  }
  const result = ha.poll();
  if (!result.done) return;
  if (result.error) {
    if (pending !== 'stream') setStatus('网络错误 ' + result.error);
    pending = '';
    return;
  }
  if (result.status < 200 || result.status >= 300) {
    if (pending !== 'stream') setStatus('HTTP ' + result.status);
    pending = '';
    return;
  }
  try {
    if (pending === 'states' || pending === 'stream') {
      const values = JSON.parse(result.body);
      const keepPage = pending === 'stream';
      const keepSel = selected;
      applySlots(Array.isArray(values) ? values : []);
      if (!keepPage) {
        page = 0;
        selected = entities.length ? 0 : -1;
      } else if (keepSel >= entities.length) {
        selected = entities.length ? entities.length - 1 : -1;
      } else {
        selected = keepSel;
      }
      render();
      if (pending === 'states') {
        setStatus(entities.length ? '已注册 ' + entities.length + ' 个实体' :
                  '没有设备');
      }
    } else if (pending === 'state') {
      const value = JSON.parse(result.body);
      if (selected >= 0) entities[selected] = value;
      render();
      setStatus(clip(entityName(value), 12) + '  ·  ' +
                stateLabel(value.state));
    } else {
      setStatus('已执行');
      if (selected >= 0) requestState();
    }
  } catch (e) {
    if (pending !== 'stream') setStatus('响应解析失败或实体过多');
  }
  pending = '';
}, 250);

render();
if (token) discover();
else setStatus('配置地址和访问令牌');
