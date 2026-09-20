'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname,
  '../overlay/apps/system/desktop/homeassistant/app.js'), 'utf8');

assert.equal(source.includes('\\u2212'), false,
  'frontend text must not use the unsupported Unicode minus sign');

function boot(width = 1024, height = 600, options = {}) {
  const serviceUrl = options.url || 'http://192.168.1.20:8123';
  let handle = 1;
  let requestId = 1;
  let timer;
  let touch;
  let swipe;
  let nextTimeout = 2;
  const deferred = [];
  const pending = [];
  const buttons = [];
  const controls = [];
  const geometry = new Map();
  const textValues = new Map();
  const responses = {
    config: { location_name: 'Lab' },
    services: [{ domain: 'light', services: { turn_on: {}, turn_off: {} } }],
    states: options.states || [
      { entity_id: 'light.desk', state: 'on', attributes: {
        friendly_name: 'Desk', brightness: 128,
        supported_color_modes: ['brightness'] } },
      { entity_id: 'light.spotlights', state: 'on', attributes: {
        friendly_name: 'Spotlights', brightness: 200,
        supported_color_modes: ['brightness'] } },
      { entity_id: 'light.a', state: 'off', attributes: {
        friendly_name: 'Light A', supported_color_modes: ['brightness'] } },
      { entity_id: 'light.b', state: 'off', attributes: {
        friendly_name: 'Light B', supported_color_modes: ['brightness'] } },
      { entity_id: 'light.c', state: 'on', attributes: {
        friendly_name: 'Light C', brightness: 102,
        supported_color_modes: ['brightness'] } },
      { entity_id: 'light.d', state: 'off', attributes: {
        friendly_name: 'Light D', supported_color_modes: ['brightness'] } },
      { entity_id: 'sensor.temperature', state: '24.5', attributes: {
        friendly_name: 'Temperature', unit_of_measurement: 'C' } }
    ]
  };
  const service = {
    apiVersion: 1,
    configure: () => 0,
    status: () => ({ configured: true, busy: false,
      url: serviceUrl, canControl: true, canConfigure: true }),
    get(resource) {
      const id = requestId++;
      pending.push({ requestId: id, busy: false, done: true,
        cached: resource === 'states', status: 200, error: 0,
        body: JSON.stringify(responses[resource]) });
      return id;
    },
    getState: () => -16,
    control(entity, on, brightness) {
      controls.push({ entity, on, brightness });
      return -16;
    },
    poll: () => pending.shift() || { busy: false, done: false },
    close: () => {}
  };
  function create(x, y, w, h) {
    const id = handle++;
    geometry.set(id, { x, y, w, h });
    return id;
  }
  function requireHandle(id, api) {
    if (!geometry.has(id)) {
      throw new TypeError(`${api} received a non-native handle: ${String(id)}`);
    }
  }
  const ui = {
    primary: 0x252d46,
    getSize: () => ({ width, height }),
    background: () => {},
    button(text, x, y, w, h, callback) {
      const id = create(x, y, w, h);
      buttons.push({ id, callback });
      return id;
    },
    panel: (x, y, w, h) => create(x, y, w, h),
    rect: (x, y, w, h) => create(x, y, w, h),
    arc: (x, y, w, h) => create(x, y, w, h),
    arcSet: id => requireHandle(id, 'arcSet'),
    line: (points, x, y, w, h) => create(x, y, w, h),
    lineSet: id => requireHandle(id, 'lineSet'),
    text: (text, x, y, w = 1, h = 1) => create(x, y, w, h),
    setColor: id => requireHandle(id, 'setColor'),
    setHidden: id => requireHandle(id, 'setHidden'),
    setStyle: id => requireHandle(id, 'setStyle'),
    setText(id, value) {
      requireHandle(id, 'setText');
      textValues.set(id, value);
    },
    setOpacity: id => requireHandle(id, 'setOpacity'),
    onSwipe: handler => { swipe = handler; },
    onTouch: handler => { touch = handler; },
    setPos(id, x, y) {
      requireHandle(id, 'setPos');
      Object.assign(geometry.get(id), { x, y });
    },
    setSize(id, w, h) {
      requireHandle(id, 'setSize');
      Object.assign(geometry.get(id), { w, h });
    }
  };
  const context = vm.createContext({
    system: { homeAssistantService: service, homeAssistant: {},
      yield: () => ({ then: resolve => { deferred.push(resolve); } }),
      storage: { get: () => '', set: () => {} } },
    ui, prompt: { input: () => {} }, console,
    setInterval(callback) { timer = callback; return 1; },
    clearInterval: () => {},
    setTimeout(callback) { deferred.push(callback); return nextTimeout++; },
    clearTimeout: () => {}
  });
  vm.runInContext(source, context, { filename: 'homeassistant/app.js' });
  return { context, buttons, controls, geometry, textValues, tick: () => {
    timer();
    const callback = deferred.shift();
    if (callback) callback();
  },
    touch: (state, nx, ny) => touch(state, nx, ny),
    swipe: direction => swipe(direction) };
}

test('frontend uses the reviewed native service and completes a state sync', () => {
  const runtime = boot();
  assert.equal(runtime.context.ESPHomeHA.snapshot().backend, 'native-service');
  runtime.context.ESPHomeHA.refresh();
  for (let i = 0; i < 12; i++) runtime.tick();
  const state = runtime.context.ESPHomeHA.snapshot();
  assert.equal(state.connected, true);
  assert.equal(state.phase, 'ready');
  assert.equal(state.lastCached, true);
  assert.equal(state.entities.length, 7);
  assert.equal(state.entities[0].brightness, 50);
});

test('frontend keeps its fixed controls inside supported displays', () => {
  for (const [width, height] of [[1024, 536], [1024, 600]]) {
    const runtime = boot(width, height);
    assert.equal(runtime.buttons.length, 32);
    assert.ok(runtime.geometry.size <= 140,
      `${width}x${height}: ${runtime.geometry.size} native widgets exceeds the UI budget`);
    for (const box of runtime.geometry.values()) {
      assert.ok(box.x >= 0 && box.y >= 0 && box.w > 0 && box.h > 0);
      assert.ok(box.x + box.w <= width, `${width} clips ${JSON.stringify(box)}`);
      assert.ok(box.y + box.h <= height, `${height} clips ${JSON.stringify(box)}`);
    }
  }
});

test('card opens details while only its icon toggles the entity', () => {
  for (const [width, height] of [[1024, 536], [1024, 600]]) {
    const runtime = boot(width, height);
    runtime.context.ESPHomeHA.refresh();
    for (let i = 0; i < 12; i++) runtime.tick();
    const card = runtime.buttons.find(entry => {
      const box = runtime.geometry.get(entry.id);
      return box && box.w >= 120 && box.h >= 50;
    });
    assert.ok(card, `${width}x${height}: a card sized button exists`);
    card.callback();
    assert.equal(runtime.context.ESPHomeHA.snapshot().view, 'detail');
    assert.equal(runtime.context.ESPHomeHA.snapshot().detailOpen, true);
    assert.equal(runtime.controls.length, 0, `${width}x${height}: card does not toggle`);
    for (let i = 0; i < 12; i++) runtime.tick();
    assert.equal(runtime.buttons.length, 36, `${width}x${height}: detail pool expands lazily`);
    for (const box of runtime.geometry.values()) {
      assert.ok(box.x + box.w <= width, `${width} detail clips ${JSON.stringify(box)}`);
      assert.ok(box.y + box.h <= height, `${height} detail clips ${JSON.stringify(box)}`);
    }

    const iconRuntime = boot(width, height);
    iconRuntime.context.ESPHomeHA.refresh();
    for (let i = 0; i < 12; i++) iconRuntime.tick();
    const icon = iconRuntime.buttons.find(entry => {
      const box = iconRuntime.geometry.get(entry.id);
      return box && box.w === 48 && box.h === 48;
    });
    assert.ok(icon, `${width}x${height}: an icon toggle exists`);
    icon.callback();
    assert.equal(iconRuntime.controls.length, 1, `${width}x${height}: one control call`);
    assert.equal(iconRuntime.controls[0].entity, 'light.desk');
    assert.equal(iconRuntime.controls[0].on, false);
  }
});

test('detail power control toggles while read-only details never send control', () => {
  const runtime = boot();
  runtime.context.ESPHomeHA.refresh();
  for (let i = 0; i < 12; i++) runtime.tick();
  const card = runtime.buttons.find(entry => {
    const box = runtime.geometry.get(entry.id);
    return box && box.w >= 120 && box.h >= 50;
  });
  card.callback();
  for (let i = 0; i < 12; i++) runtime.tick();
  const power = runtime.buttons.find(entry => {
    const box = runtime.geometry.get(entry.id);
    return box && box.w === 116 && box.h === 116;
  });
  assert.ok(power, 'detail power control exists');
  assert.ok([...runtime.textValues.values()].includes('\uf011'),
    'detail uses the supported centered power glyph');
  assert.ok([...runtime.textValues.values()].includes('\uf068'),
    'detail uses the supported minus glyph');
  assert.ok([...runtime.textValues.values()].includes('\uf067'),
    'detail uses the supported plus glyph');
  power.callback();
  assert.equal(runtime.controls.length, 1, 'detail power sends one control call');
  assert.equal(runtime.controls[0].entity, 'light.desk');
  assert.equal(runtime.controls[0].on, false);

  const sensorRuntime = boot(1024, 600, { states: [{
    entity_id: 'sensor.temperature', state: '24.5', attributes: {
      friendly_name: 'Temperature', unit_of_measurement: 'C', device_class: 'temperature'
    }
  }] });
  sensorRuntime.context.ESPHomeHA.refresh();
  for (let i = 0; i < 12; i++) sensorRuntime.tick();
  const sensorCard = sensorRuntime.buttons.find(entry => {
    const box = sensorRuntime.geometry.get(entry.id);
    return box && box.w >= 120 && box.h >= 50;
  });
  sensorCard.callback();
  for (let i = 0; i < 12; i++) sensorRuntime.tick();
  assert.equal(sensorRuntime.context.ESPHomeHA.snapshot().view, 'detail');
  assert.equal(sensorRuntime.controls.length, 0);
});

test('detail brightness controls keep using brightness_pct through the native service', () => {
  const runtime = boot();
  runtime.context.ESPHomeHA.refresh();
  for (let i = 0; i < 12; i++) runtime.tick();
  const card = runtime.buttons.find(entry => {
    const box = runtime.geometry.get(entry.id);
    return box && box.w >= 120 && box.h >= 50;
  });
  card.callback();
  for (let i = 0; i < 12; i++) runtime.tick();
  const brightnessButtons = runtime.buttons.filter(entry => {
    const box = runtime.geometry.get(entry.id);
    return box && box.w === 58 && box.h === 42;
  }).sort((a, b) => runtime.geometry.get(a.id).x - runtime.geometry.get(b.id).x);
  assert.equal(brightnessButtons.length, 2);
  brightnessButtons[1].callback();
  assert.equal(runtime.controls.length, 1);
  assert.equal(runtime.controls[0].entity, 'light.desk');
  assert.equal(runtime.controls[0].on, true);
  assert.equal(runtime.controls[0].brightness, 60);
});

test('a configured native session is adopted even on a public endpoint', () => {
  const runtime = boot(1024, 600, { url: 'http://124.223.30.246:8123' });
  const state = runtime.context.ESPHomeHA.snapshot();
  assert.equal(state.nativeManaged, true);
  assert.equal(state.backend, 'native-service');
  assert.equal(state.settingsOpen, false);
  assert.equal(state.url, 'http://124.223.30.246:8123');
  runtime.context.ESPHomeHA.refresh();
  for (let i = 0; i < 12; i++) runtime.tick();
  assert.equal(runtime.context.ESPHomeHA.snapshot().connected, true);
});

test('dashboard preserves domain metadata and pages through every synced entity', () => {
  const states = [
    { entity_id: 'cover.study', state: 'open', attributes: {
      friendly_name: 'Study blind', device_class: 'blind', current_position: 72 } },
    { entity_id: 'climate.downstairs', state: 'heat', attributes: {
      friendly_name: 'Downstairs', current_temperature: 20.5, temperature: 22,
      hvac_action: 'heating' } },
    { entity_id: 'media_player.living_room', state: 'playing', attributes: {
      friendly_name: 'Living room speaker', media_title: 'Evening radio', volume_level: 0.35 } },
    { entity_id: 'binary_sensor.front_door', state: 'on', attributes: {
      friendly_name: 'Front door', device_class: 'door' } },
    { entity_id: 'sensor.phone_battery', state: '68', attributes: {
      friendly_name: 'Phone battery', device_class: 'battery', unit_of_measurement: '%' } }
  ];
  for (let index = 0; index < 10; index++) states.push({
    entity_id: `sensor.metric_${index}`, state: String(index), attributes: {
      friendly_name: `Metric ${index}`, unit_of_measurement: 'W', device_class: 'power'
    }
  });
  const runtime = boot(1024, 600, { states });
  runtime.context.ESPHomeHA.refresh();
  for (let index = 0; index < 12; index++) runtime.tick();
  let state = runtime.context.ESPHomeHA.snapshot();
  assert.equal(state.entities.length, 15);
  assert.equal(state.entities[0].current_position, 72);
  assert.equal(state.entities[1].current_temperature, 20.5);
  assert.equal(state.entities[1].temperature, 22);
  assert.equal(state.entities[2].media_title, 'Evening radio');
  assert.equal(state.entities[2].volume_level, 0.35);
  assert.equal(state.entities[3].device_class, 'door');
  assert.ok([...runtime.textValues.values()].some(value => String(value).startsWith('Evening')));
  runtime.swipe('left');
  state = runtime.context.ESPHomeHA.snapshot();
  assert.equal(state.roomPage, 1);
  runtime.swipe('right');
  assert.equal(runtime.context.ESPHomeHA.snapshot().roomPage, 0);
});
