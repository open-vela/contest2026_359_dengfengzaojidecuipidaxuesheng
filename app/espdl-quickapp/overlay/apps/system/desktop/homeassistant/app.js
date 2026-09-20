/* SPDX-License-Identifier: Apache-2.0 */
'use strict';

(function () {
  const nativeService = system.homeAssistantService;
  const sharedReady = nativeService && nativeService.apiVersion === 1 &&
    ['configure', 'get', 'getState', 'control', 'poll', 'close', 'status']
      .every(name => typeof nativeService[name] === 'function');
  const bridge = nativeService ? (sharedReady ? sharedAdapter() : {}) : system.homeAssistant;
  const storage = system.storage;
  const bridgeReady = bridge && ['get', 'getState', 'callService', 'poll']
    .every(name => typeof bridge[name] === 'function');
  const MAX_ENTITIES = 128;
  const MAX_FAVORITES = 16;
  const RESPONSE_LIMIT = 65536;
  const REQUEST_TIMEOUT = 12000;
  const REFRESH_INTERVAL = 10000;
  const VERIFY_INTERVAL = 1000;
  const VERIFY_WINDOW = 8000;
  const VERIFY_ATTEMPTS = 4;
  const INITIAL_BUTTON_SLOTS = 32;
  const BUTTON_SLOTS = 36;
  const buttons = [];
  const buttonPool = [];
  const size = ui.getSize();
  const W = size.width, H = size.height;
  const dark = ui.primary === 0xffffff;
  const colors = {
    background: dark ? 0x101318 : 0xf3f5f8,
    surface: dark ? 0x1d222b : 0xffffff,
    active: dark ? 0x3c2d20 : 0xfff0e4,
    hero: dark ? 0x32271f : 0xffe4cf,
    heroMuted: dark ? 0xcab39d : 0x8b5b36,
    sensor: dark ? 0x1c2f33 : 0xe3f1ef,
    detail: dark ? 0x171c24 : 0xf7f8fa,
    text: dark ? 0xf7f8fa : 0x15171a,
    muted: dark ? 0x9da5b2 : 0x6f7782,
    accent: dark ? 0xff7a1a : 0xff6900,
    selected: dark ? 0x4b321f : 0xffe2cc,
    good: dark ? 0x68d39b : 0x148054,
    warning: dark ? 0xf2be63 : 0xa36100,
    border: dark ? 0x2b323d : 0xe7eaef,
    track: dark ? 0x2f3742 : 0xeaeef3,
    warm: dark ? 0xffc107 : 0xf5b21b,
    cool: dark ? 0x2196f3 : 0x1f8fe0,
    orange: dark ? 0xff9800 : 0xf07d1a,
    purple: dark ? 0x9a7ad0 : 0x8a63c4
  };
  const styles = {
    icon: { radius: 18, borderWidth: 0 },
    tab: { radius: 16, borderWidth: 0, fontSize: 15 },
    card: { radius: 18, borderWidth: 0 },
    control: { radius: 14, borderWidth: 0, fontSize: 16 },
    setting: { radius: 14, borderWidth: 0, fontSize: 15 },
    panel: { radius: 18, borderWidth: 0 }
  };
  const glyph = {
    search: '\uf002', refresh: '\uf021', settings: '\uf013',
    previous: '\uf053', next: '\uf054', favorite: '\uf067', followed: '\uf00c',
    light: '\uf0e7', switch: '\uf011', input_boolean: '\uf205', fan: '\uf2dc',
    sensor: '\uf2c9', binary_sensor: '\uf06a', home: '\uf015'
  };
  let url = 'http://homeassistant.local:8123';
  let token = '';
  let httpAllowed = false;
  let favorites = [];
  let scope = 'all';
  let entities = [];
  let nav = 'overview', roomPage = 0, layoutStyle = 'rooms';
  let services = Object.create(null);
  let serviceDiscoveryLimited = false;
  let selectedId = '';
  let filter = 'all';
  let search = '';
  let page = 0;
  let settingsOpen = false;
  let detailOpen = false;
  let connected = false;
  let phase = 'disconnected';
  let message = '未连接';
  let generation = 0;
  let pending = null;
  let queued = null;
  let nativeBusy = false;
  let autoRefresh = false;
  let failures = 0;
  let nextRefresh = Infinity;
  let lastSync = 0;
  let lastCached = false;
  let disposed = false;
  let limited = false;
  let watchQueue = [];
  let watchResults = [];
  let timer;
  let renderTimer = 0;
  let widgets;
  let nativeManaged = false;
  let entityRevision = 0;
  let dashboardCacheKey = '';
  let dashboardCache = [];
  let cardRenderKey = '';
  let cardRenderCursor = 0;
  let renderRunning = false;
  let renderAgain = false;
  let detailBuilding = false;

  function sharedAdapter() {
    let requestId = 0, detached = false;
    function prepare() {
      detached = false;
      if (token) {
        if (nativeService.configure(url, token, httpAllowed) !== 0) return false;
        token = '';
        httpAllowed = false;
        nativeManaged = true;
      }
      const status = nativeService.status();
      nativeManaged = nativeEndpoint(status) === url;
      return nativeManaged;
    }
    function submit(action) {
      if (!prepare()) return false;
      const id = action();
      if (!Number.isInteger(id) || id <= 0) return false;
      requestId = id;
      return true;
    }
    function close() {
      detached = true;
      requestId = 0;
      nativeService.close();
    }
    return {
      get: (_url, _token, resource) => submit(() => nativeService.get(resource)),
      getState: (_url, _token, entity) => submit(() => nativeService.getState(entity)),
      callService: (_url, _token, domain, service, data) => {
        const body = JSON.parse(data);
        if (!validEntity(body.entity_id) || body.entity_id.split('.')[0] !== domain ||
            !['turn_on', 'turn_off'].includes(service) ||
            Object.keys(body).some(key => !['entity_id', 'brightness_pct'].includes(key))) return false;
        return submit(() => nativeService.control(body.entity_id, service === 'turn_on',
          body.brightness_pct === undefined ? -1 : body.brightness_pct));
      },
      poll: () => {
        if (detached) return { busy: false, done: false };
        const result = nativeService.poll();
        if (result.done && result.requestId !== requestId)
          return { busy: false, done: true, error: -22, status: 0, body: '' };
        return result;
      },
      close, stop: close
    };
  }

  function sharedConnection() {
    if (!sharedReady) return false;
    try {
      nativeManaged = nativeEndpoint(nativeService.status()) === url;
      return nativeManaged;
    } catch (_) {
      nativeManaged = false;
      return false;
    }
  }

  /* The native service owns the saved endpoint and already validated it when it
   * was configured (portal or earlier session). Typed addresses keep the strict
   * local-network rule, but a configured native session is authoritative. */
  function nativeEndpoint(status) {
    if (!status || !status.configured) return '';
    const raw = typeof status.url === 'string' ? status.url.trim() : '';
    if (!raw) return '';
    return parseUrl(raw) || raw;
  }

  function clean(value, limit) {
    return typeof value === 'string' ?
      Array.from(value.replace(/[\u0000-\u001f\u007f]/g, ' '))
        .slice(0, limit).join('') : '';
  }

  function validEntity(value) {
    return typeof value === 'string' && value.length < 96 &&
      /^[a-z][a-z0-9_]*\.[a-z0-9_]+$/.test(value);
  }

  function parseUrl(value) {
    if (typeof value !== 'string' || value.length > 120) return '';
    const match = /^http:\/\/([a-zA-Z0-9.-]+)(?::([0-9]{1,5}))?\/?$/.exec(value.trim());
    if (!match) return '';
    const host = match[1].toLowerCase();
    const port = match[2] ? Number(match[2]) : 8123;
    if (port < 1 || port > 65535) return '';
    let local = host === 'localhost';
    if (/^[0-9.]+$/.test(host)) {
      const pieces = host.split('.');
      if (pieces.length !== 4 ||
          pieces.some(x => String(Number(x)) !== x || Number(x) > 255)) return '';
      const a = pieces.map(Number);
      local = a[0] === 10 || a[0] === 127 ||
        (a[0] === 192 && a[1] === 168) ||
        (a[0] === 172 && a[1] >= 16 && a[1] <= 31) ||
        (a[0] === 169 && a[1] === 254);
    } else if (host.endsWith('.local') || host.endsWith('.lan')) {
      local = host.split('.').every(x =>
        x.length > 0 && x.length < 64 && /^[a-z0-9](?:[a-z0-9-]*[a-z0-9])?$/.test(x));
    }
    return local ? 'http://' + host + ':' + port : '';
  }

  function save(key, value) {
    try {
      if (storage && typeof storage.set === 'function') storage.set(key, value);
      return true;
    } catch (_) {
      message = '本地保存失败，本次会话仍可使用';
      return false;
    }
  }

  try {
    if (storage && typeof storage.get === 'function') {
      url = parseUrl(storage.get('eh_url')) || url;
      const saved = JSON.parse(storage.get('eh_favs') || '[]');
      if (Array.isArray(saved)) {
        favorites = saved.filter(validEntity)
          .filter((id, index, list) => list.indexOf(id) === index)
          .slice(0, MAX_FAVORITES);
      }
      /* An empty watch list would sync nothing at all, so fall back to the
       * full state list instead of showing an empty dashboard. */
      scope = storage.get('eh_scope') === 'favorites' && favorites.length ?
        'favorites' : 'all';
    }
  } catch (_) {
    message = '本地设置无法读取';
  }

  if (sharedReady) {
    try {
      const status = nativeService.status();
      const endpoint = nativeEndpoint(status);
      if (endpoint) {
        url = endpoint;
        nativeManaged = true;
        message = '本地 HA 服务已配置，尚未同步';
      }
    } catch (_) { message = '本地 HA 服务暂不可用'; }
  }
  // A remembered native connection should open directly to the home view and
  // begin its first bounded sync. New connections start with the setup view.
  settingsOpen = !nativeManaged;
  autoRefresh = nativeManaged;

  try {
    if (typeof console !== 'undefined' && console && typeof console.log === 'function') {
      var probeStatus = sharedReady ? nativeService.status() : null;
      console.log('ha-probe service=' + (nativeService ? 'yes' : 'no') +
        ' api=' + (nativeService ? nativeService.apiVersion : '-') +
        ' ready=' + !!sharedReady +
        ' configured=' + (probeStatus ? String(probeStatus.configured) : '-') +
        ' nativeUrl=' + (probeStatus ? String(probeStatus.url) : '-') +
        ' canControl=' + (probeStatus ? String(probeStatus.canControl) : '-') +
        ' jsUrl=' + url + ' managed=' + nativeManaged +
        ' settings=' + settingsOpen);
    }
  } catch (probeError) {
    if (typeof console !== 'undefined' && console && typeof console.log === 'function') {
      console.log('ha-probe failed ' + probeError);
    }
  }

  function staleAll() {
    entities.forEach(item => { item.stale = true; });
  }

  function invalidate() {
    generation++;
    queued = null;
    if (pending) pending.abandoned = true;
    connected = false;
    autoRefresh = false;
    nextRefresh = Infinity;
    lastCached = false;
    phase = 'disconnected';
    services = Object.create(null);
    serviceDiscoveryLimited = false;
    staleAll();
  }

  function disconnect() {
    invalidate();
    token = '';
    httpAllowed = false;
    message = '已断开，令牌已从会话移除';
    if (sharedReady) {
      try { bridge.close(); } catch (_) { /* UI still invalidates its session. */ }
      nativeManaged = false;
      message = '界面已断开，本地服务配置保留';
    }
    render();
  }

  function credentialsReady() {
    if (!bridgeReady) message = '固件未提供 Home Assistant 接口';
    else if (sharedConnection()) return true;
    else if (!parseUrl(url)) message = '需要局域网 HTTP 地址';
    else if (!token) message = '尚未输入访问令牌';
    else if (!httpAllowed) message = 'HTTP 未加密，尚未授权连接';
    else return true;
    render();
    return false;
  }

  function schedule(kind, entityId, data) {
    queued = { kind, entityId: entityId || '', data, generation };
  }

  function startQueued() {
    if (!queued || pending || nativeBusy || disposed) return;
    if (queued.notBefore && Date.now() < queued.notBefore) return;
    const request = queued;
    queued = null;
    if (request.generation !== generation || !credentialsReady()) return;
    let accepted = false;
    try {
      if (request.kind === 'service') {
        accepted = bridge.callService(url, token, request.data.domain,
          request.data.service, JSON.stringify(request.data.body));
      } else if (request.kind === 'verify' || request.kind === 'watch') {
        accepted = bridge.getState(url, token, request.entityId);
      } else {
        accepted = bridge.get(url, token, request.kind);
      }
    } catch (_) {
      accepted = false;
    }
    if (!accepted) {
      fail(request, '接口忙或参数被拒绝', false);
      return;
    }
    pending = request;
    pending.started = Date.now();
    nativeBusy = true;
    render();
  }

  function connect() {
    if (!credentialsReady()) return;
    if (pending || nativeBusy || queued) {
      message = '仍有请求未结束';
      render();
      return;
    }
    generation++;
    connected = false;
    phase = 'connecting';
    settingsOpen = false;
    detailOpen = false;
    selectedId = '';
    autoRefresh = true;
    failures = 0;
    services = Object.create(null);
    serviceDiscoveryLimited = false;
    staleAll();
    message = '正在连接 Home Assistant';
    schedule('config');
    startQueued();
  }

  function refresh() {
    if (disposed || pending || nativeBusy || queued) return;
    if (!connected) { connect(); return; }
    if (!credentialsReady()) return;
    message = '正在同步状态';
    beginStates();
    startQueued();
  }

  function beginStates() {
    if (scope === 'favorites') {
      watchQueue = favorites.slice();
      watchResults = [];
      nextWatch();
    } else {
      schedule('states');
    }
  }

  function nextWatch() {
    if (watchQueue.length) schedule('watch', watchQueue.shift());
    else commitStates(watchResults, false, false);
  }

  function normalize(raw, expectedId) {
    if (!raw || typeof raw !== 'object' || !validEntity(raw.entity_id) ||
        typeof raw.state !== 'string' ||
        (expectedId && raw.entity_id !== expectedId)) return null;
    const attributes = raw.attributes && typeof raw.attributes === 'object' ?
      raw.attributes : {};
    const modes = Array.isArray(attributes.supported_color_modes) ?
      attributes.supported_color_modes : [];
    const supportedFeatures = typeof attributes.supported_features === 'number' &&
      Number.isFinite(attributes.supported_features) ? attributes.supported_features : 0;
    const brightness = typeof attributes.brightness === 'number' &&
      Number.isFinite(attributes.brightness) &&
      attributes.brightness >= 0 && attributes.brightness <= 255 ?
      Math.round(attributes.brightness * 100 / 255) : null;
    function numeric(name, minimum, maximum) {
      const value = attributes[name];
      if (typeof value !== 'number' || !Number.isFinite(value) ||
          value < minimum || value > maximum) return null;
      return value;
    }
    return {
      entity_id: raw.entity_id,
      domain: raw.entity_id.split('.')[0],
      name: clean(attributes.friendly_name, 64) || raw.entity_id,
      state: clean(raw.state, 64),
      unit: clean(attributes.unit_of_measurement, 16),
      device_class: clean(attributes.device_class, 32),
      icon: clean(attributes.icon, 48),
      brightness,
      current_position: numeric('current_position', 0, 100),
      temperature: numeric('temperature', -100, 200),
      current_temperature: numeric('current_temperature', -100, 200),
      hvac_action: clean(attributes.hvac_action, 24),
      volume_level: numeric('volume_level', 0, 1),
      media_title: clean(attributes.media_title, 80),
      battery_level: numeric('battery_level', 0, 100),
      dimmable: raw.entity_id.startsWith('light.') &&
        (brightness !== null || (supportedFeatures & 1) !== 0 || modes.some(mode =>
          ['brightness', 'color_temp', 'hs', 'xy', 'rgb', 'rgbw', 'rgbww', 'white'].includes(mode))),
      stale: false
    };
  }

  function commitStates(values, wasLimited, fromCache) {
    entities = values;
    entityRevision++;
    limited = wasLimited;
    lastCached = !!fromCache;
    if (!entities.some(item => item.entity_id === selectedId)) selectedId = '';
    connected = true;
    phase = 'ready';
    failures = 0;
    lastSync = Date.now();
    nextRefresh = lastSync + REFRESH_INTERVAL;
    message = limited ? '已达 128 个实体上限，可切换关注同步' :
      '已同步 ' + entities.length + ' 个实体' +
      (lastCached ? ' · 本地缓存' : '');
    if (serviceDiscoveryLimited) message = '只读 · 服务目录超过 64 KiB' +
      (lastCached ? ' · 本地缓存' : '');
    /* tick() renders once after complete(); a second render here can push a
     * large state synchronization past the callback deadline. */
  }

  function failureMessage(result, request) {
    if (result.error === -75 || result.error === -7) {
      if (request.kind === 'states') return '响应超过 64 KiB，请切换关注同步';
      if (request.kind === 'service' || request.kind === 'verify')
        return '控制结果未确认，请刷新；不会重试';
      return '该响应超过 64 KiB，当前固件无法读取';
    }
    if (result.error) return request.kind === 'service' ?
      '控制结果未确认，请刷新；不会重试' : '网络连接失败，状态已过期';
    if (result.status === 401 || result.status === 403) return '认证失败，请重新输入令牌';
    return request.kind === 'service' ? '控制被拒绝，HTTP ' + result.status :
      '服务返回 HTTP ' + result.status;
  }

  function fail(request, text, auth) {
    queued = null;
    connected = false;
    phase = 'error';
    staleAll();
    message = text;
    failures++;
    if (auth) {
      generation++;
      token = '';
      httpAllowed = false;
      nativeManaged = false;
      autoRefresh = false;
    }
    const uncertain = request.kind === 'service' || request.kind === 'verify';
    nextRefresh = auth || uncertain ? Infinity :
      Date.now() + Math.min(60000, REFRESH_INTERVAL * Math.pow(2, Math.min(failures - 1, 3)));
    if (request.kind === 'verify') message = '控制结果未确认，请刷新；不会重试';
    render();
  }

  function complete(request, result) {
    if (request.generation !== generation || request.abandoned || disposed) return;
    if (request.kind === 'services' && (result.error === -75 || result.error === -7 ||
        (!result.error && result.status >= 200 && result.status < 300 &&
         typeof result.body === 'string' && result.body.length > RESPONSE_LIMIT))) {
      services = Object.create(null);
      serviceDiscoveryLimited = true;
      message = '服务目录超限，正在只读同步';
      beginStates();
      return;
    }
    if (request.kind === 'watch' && !result.error && result.status === 404) {
      watchResults.push({
        entity_id: request.entityId, domain: request.entityId.split('.')[0],
        name: request.entityId, state: 'unavailable', unit: '',
        brightness: null, dimmable: false, stale: false
      });
      nextWatch();
      return;
    }
    if (result.error || result.status < 200 || result.status >= 300) {
      fail(request, failureMessage(result, request),
        result.status === 401 || result.status === 403);
      return;
    }
    if (request.kind === 'service') {
      message = '控制已受理，正在核对状态';
      phase = 'verifying';
      nextRefresh = Infinity;
      schedule('verify', request.entityId, {
        state: request.data.service === 'turn_on' ? 'on' : 'off',
        brightness: request.data.body.brightness_pct,
        attempt: 1,
        deadline: Date.now() + VERIFY_WINDOW
      });
      return;
    }
    try {
      if (typeof result.body !== 'string' || result.body.length > RESPONSE_LIMIT)
        throw new Error('response limit');
      const value = JSON.parse(result.body);
      if (request.kind === 'config') {
        if (!value || typeof value !== 'object' || Array.isArray(value))
          throw new Error('invalid config');
        schedule('services');
      } else if (request.kind === 'services') {
        if (!Array.isArray(value)) throw new Error('invalid services');
        const next = Object.create(null);
        value.forEach(group => {
          if (!group || typeof group.domain !== 'string' ||
              !/^[a-z_]+$/.test(group.domain) ||
              !group.services || typeof group.services !== 'object') return;
          const names = Array.isArray(group.services) ?
            group.services : Object.keys(group.services);
          names.forEach(name => {
            if (typeof name === 'string' && /^[a-z_]+$/.test(name))
              next[group.domain + '.' + name] = true;
          });
        });
        services = next;
        beginStates();
      } else if (request.kind === 'states') {
        if (!Array.isArray(value)) throw new Error('invalid states');
        const next = [];
        const seen = Object.create(null);
        let overflow = false;
        value.forEach(raw => {
          const item = normalize(raw);
          if (!item || seen[item.entity_id]) return;
          seen[item.entity_id] = true;
          if (next.length < MAX_ENTITIES) next.push(item);
          else overflow = true;
        });
        commitStates(next, overflow, !!result.cached);
      } else {
        const item = normalize(value, request.entityId);
        if (!item) throw new Error('entity mismatch');
        if (request.kind === 'watch') {
          watchResults.push(item);
          nextWatch();
        } else {
          const index = entities.findIndex(row => row.entity_id === request.entityId);
          if (index >= 0) {
            entities[index] = item;
            entityRevision++;
          }
          const target = request.data;
          const matched = item.state === target.state &&
            (target.brightness === undefined || (item.brightness !== null &&
             Math.abs(item.brightness - target.brightness) <= 1));
          if (!matched) {
            if (target.attempt >= VERIFY_ATTEMPTS || Date.now() >= target.deadline) {
              fail(request, '控制结果未确认，请刷新；不会重试', false);
              return;
            }
            message = '等待设备达到目标状态';
            schedule('verify', request.entityId, {
              state: target.state, brightness: target.brightness,
              attempt: target.attempt + 1, deadline: target.deadline
            });
            queued.notBefore = Date.now() + VERIFY_INTERVAL;
            render();
            return;
          }
          connected = true;
          phase = 'ready';
          message = '状态已达到目标：' + item.name;
          failures = 0;
          nextRefresh = Date.now() + REFRESH_INTERVAL;
          render();
        }
      }
    } catch (_) {
      fail(request, '响应格式错误，未应用数据', false);
    }
  }

  function tick() {
    if (disposed || !bridgeReady) return;
    if (pending && !pending.abandoned &&
        Date.now() - pending.started >= REQUEST_TIMEOUT) {
      pending.abandoned = true;
      fail(pending, pending.kind === 'service' ?
        '控制结果未确认，请刷新；不会重试' : '请求超时，等待旧请求结束', false);
    }
    // Enforce the whole confirmation window before consuming a late result.
    const verification = pending || queued;
    if (verification && verification.kind === 'verify' &&
        !verification.abandoned && Date.now() >= verification.data.deadline) {
      verification.abandoned = true;
      fail(verification, '控制结果未确认，请刷新；不会重试', false);
    }
    let result;
    try { result = bridge.poll(); }
    catch (_) {
      if (pending && !pending.abandoned) {
        pending.abandoned = true;
        fail(pending, '接口异常，等待旧请求结束', false);
      }
      return;
    }
    if (!result || typeof result !== 'object') return;
    nativeBusy = !!result.busy;
    if (pending && result.done) {
      const request = pending;
      pending = null;
      complete(request, result);
      render();
    } else if (pending && !nativeBusy && pending.abandoned) {
      pending = null;
      render();
    }
    startQueued();
    if (!pending && !queued && !nativeBusy && !settingsOpen &&
        autoRefresh && (token || sharedConnection()) && Date.now() >= nextRefresh) {
      if (connected) refresh();
      else {
        phase = 'connecting';
        message = '正在重新读取连接状态';
        schedule('config');
        startQueued();
      }
    }
  }

  function current() {
    return entities.find(item => item.entity_id === selectedId) || null;
  }

  function available(item) {
    return item && !item.stale && item.state !== 'unavailable' &&
      item.state !== 'unknown';
  }

  function supports(item, service) {
    if (sharedReady) {
      try { if (!nativeService.status().canControl) return false; }
      catch (_) { return false; }
    }
    return item && ['light', 'switch', 'input_boolean', 'fan'].includes(item.domain) &&
      !!services[item.domain + '.' + service];
  }

  function control(service, brightness) {
    const item = current();
    if (!connected || !available(item) || pending || queued || nativeBusy ||
        !supports(item, service)) return;
    const body = { entity_id: item.entity_id };
    if (brightness !== undefined) {
      if (!item.dimmable || !Number.isFinite(brightness)) return;
      body.brightness_pct = Math.max(1, Math.min(100, Math.round(brightness)));
    }
    message = '正在发送控制请求';
    schedule('service', item.entity_id, { domain: item.domain, service, body });
    startQueued();
  }

  function changeBrightness(delta) {
    const item = current();
    if (!item || !item.dimmable) return;
    const level = item.brightness === null ? 0 : item.brightness;
    if (level <= 0 && delta < 0) return;
    control('turn_on', level + delta);
  }

  function toggleFavorite() {
    const item = current();
    if (!item) return;
    const index = favorites.indexOf(item.entity_id);
    if (index >= 0) favorites.splice(index, 1);
    else if (favorites.length < MAX_FAVORITES) favorites.push(item.entity_id);
    else { message = '关注列表已达 16 个实体'; render(); return; }
    save('eh_favs', JSON.stringify(favorites));
    render();
  }

  function promptUrl() {
    const openedGeneration = generation;
    prompt.input({ title: 'Home Assistant 地址', value: url, maxLength: 120 }, value => {
      if (disposed || openedGeneration !== generation ||
          value === null || value === undefined || value === '') return;
      const next = parseUrl(value);
      if (!next) {
        message = /^https:/i.test(String(value)) ?
          '当前 HA 接口不支持 HTTPS，不会降级连接' : '仅支持局域网 HTTP 地址，不允许路径或账号';
      } else if (next !== url) {
        invalidate();
        token = '';
        httpAllowed = false;
        url = next;
        favorites = [];
        entities = [];
        selectedId = '';
        message = '服务端已变更，请重新输入令牌';
        save('eh_url', url);
        save('eh_favs', '[]');
      }
      render();
    });
  }

  function promptToken() {
    const openedGeneration = generation;
    prompt.input({ title: '访问令牌（仅本次会话）', value: '',
      password: true, maxLength: 511 }, value => {
      if (disposed || openedGeneration !== generation ||
          value === null || value === undefined || value === '') return;
      if (typeof value !== 'string' || value.length > 511 || !/^[\x21-\x7e]+$/.test(value)) {
        message = '令牌含空白、控制字符或超过长度限制';
      } else {
        invalidate();
        token = value;
        message = '令牌已输入，仅保留在本次会话';
      }
      render();
    });
  }

  function addFavorite() {
    const openedGeneration = generation;
    prompt.input({ title: '关注实体 ID', value: '', maxLength: 95 }, value => {
      if (disposed || openedGeneration !== generation ||
          value === null || value === undefined || value === '') return;
      if (!validEntity(value)) message = '实体 ID 格式无效';
      else if (favorites.includes(value)) message = '该实体已在关注列表';
      else if (favorites.length >= MAX_FAVORITES) message = '关注列表已达 16 个实体';
      else {
        favorites.push(value);
        message = '已关注 ' + value;
        save('eh_favs', JSON.stringify(favorites));
      }
      render();
    });
  }

  function promptSearch() {
    prompt.input({ title: '搜索名称或实体 ID', value: search, maxLength: 64 }, value => {
      if (disposed || value === null || value === undefined) return;
      search = clean(value, 64).trim().toLowerCase();
      selectedId = '';
      page = 0;
      settingsOpen = false;
      detailOpen = false;
      render();
    });
  }

  /* Legacy dashboard helpers retained as source reference only.
  function visibleEntities() {
    return entities.filter(item => {
      if (filter === 'favorites' && !favorites.includes(item.entity_id)) return false;
      if (filter === 'controls' &&
          !['light', 'switch', 'input_boolean', 'fan'].includes(item.domain)) return false;
      if (filter === 'sensors' && !['sensor', 'binary_sensor'].includes(item.domain)) return false;
      return !search || (item.name + ' ' + item.entity_id).toLowerCase().includes(search);
    });
  }

  function stateText(item) {
    const translated = { on: '开启', off: '关闭', unavailable: '离线', unknown: '未知' };
    return (item.stale ? '上次：' : '') +
      (translated[item.state] || item.state) + (item.unit ? ' ' + item.unit : '');
  }

  function activeDevices() {
    return entities.filter(item => available(item) && item.state === 'on' &&
      ['light', 'switch', 'input_boolean', 'fan'].includes(item.domain)).length;
  }

  function controlDevices() {
    return entities.filter(item => ['light', 'switch', 'input_boolean', 'fan']
      .includes(item.domain)).length;
  }

  function cardStatus(item) {
    if (!available(item)) return item.state === 'unavailable' ? '离线' : '未知';
    if (item.state === 'on') return item.brightness === null ? '开启' : item.brightness + '%';
    if (item.state === 'off') return '关闭';
    return fit(stateText(item), 80, 14);
  }

  function fit(value, width, font) {
    const text = clean(value, 160);
    let used = 0, output = '';
    for (const char of Array.from(text)) {
      const step = char.charCodeAt(0) > 127 ? font : font * 0.66;
      if (used + step > width - font) return output + '…';
      used += step;
      output += char;
    }
    return output;
  }

  function hide(id, hidden) {
    if (id === null || id === undefined) return;
    if (typeof id === 'object') id.hidden = hidden;
    else pushHidden(id, hidden);
  }

  function pushStyle(id, tag, style) {
    const key = 'y' + tag + id;
    const value = JSON.stringify(style);
    if (paint[key] === value) return;
    paint[key] = value;
    if (typeof ui.setStyle === 'function') ui.setStyle(id, style);
  }

  function buttonColor(button, color) { button.color = color; }
  function buttonText(button, text) { button.text = text; }

  function createButton(text, x, y, w, h, action, color, style) {
    const button = { text, x, y, w, h, action, color, style: style || styles.control,
      hidden: false, section: 'common' };
    buttons.push(button);
    return button;
  }

  function initializeButtonPool() {
    for (let i = 0; i < BUTTON_SLOTS; i++) {
      const slot = { id: 0, target: null, signature: '' };
      slot.id = ui.button('', 0, 0, 1, 1, () => {
        const target = slot.target;
        if (!disposed && target && !target.hidden) target.action();
      }, colors.surface);
      pushHidden(slot.id, true);
      buttonPool.push(slot);
    }
  }

  function renderButtons() {
    const view = settingsOpen ? 'settings' : detailOpen ? 'detail' : 'list';
    const active = buttons.filter(button => {
      if (button.section === 'common') return true;
      if (button.section === 'navigation') return view !== 'settings';
      if (button.section === 'room') return button._interactive && button._roomVisible;
      return button.section === view;
    });
    if (active.length > BUTTON_SLOTS) throw new Error('Button pool capacity exceeded');
    buttonPool.forEach(slot => {
      if (slot.target && !active.includes(slot.target)) {
        slot.target = null;
        pushHidden(slot.id, true);
      }
    });
    active.forEach(button => {
      let slot = buttonPool.find(entry => entry.target === button);
      if (!slot) {
        slot = buttonPool.find(entry => !entry.target);
        if (!slot) throw new Error('Button pool exhausted');
        slot.target = button;
      }
      pushPos(slot.id, button.x, button.y);
      pushSize(slot.id, button.w, button.h);
      pushText(slot.id, button.text);
      pushColor(slot.id, button.color);
      pushStyle(slot.id, 'B', button.style);
      pushHidden(slot.id, button.hidden);
    });
  }
  function label(text, x, y, w, h, font, color, icon) {
    const id = ui.text(text, x, y, font, color, icon ? 1 : 0);
    ui.setSize(id, w, h);
    return id;
  }

  function panel(x, y, w, h, color, radius) {
    const id = ui.panel(x, y, w, h, color);
    if (typeof ui.setStyle === 'function') ui.setStyle(id, { radius: radius || 18, borderWidth: 0 });
    return id;
  }

  function iconButton(symbol, x, y, action) {
    const button = createButton('', x, y, 40, 36, action, colors.surface, styles.icon);
    const icon = label(symbol, x + 8, y + 5, 26, 26, 24, colors.accent, true);
    return { button, icon };
  }

  function placeIconButton(pair, x, y) {
    pair.button.x = x;
    pair.button.y = y;
    ui.setPos(pair.icon, x + 8, y + 5);
  }

  function pairHidden(pair, value) {
    hide(pair.button, value);
    hide(pair.icon, value);
  }
  */

  function activeDevices() {
    return entities.filter(item => available(item) && item.state === 'on' &&
      ['light', 'switch', 'input_boolean', 'fan'].includes(item.domain)).length;
  }


  // ------------------------------------------------------------------
  // 轻量总览：蓝色顶栏 + 左侧导航 + 扁平卡片网格 + 底部状态栏。
  // 只用面板、文本和按钮，不做逐点/逐柱的装饰图形，控件数与原生版持平。
  // ------------------------------------------------------------------
  const SIDEBAR_W = 164, HEADER_H = 64, FOOTER_H = 54;
  const GRID_X = SIDEBAR_W, GRID_Y = HEADER_H;
  const GRID_W = W - SIDEBAR_W;
  const MAIN_X = SIDEBAR_W + 14, MAIN_Y = HEADER_H + 16;
  const MAIN_W = W - MAIN_X - 14;
  const CARD_COLS = 4, CARD_ROWS = 2, CARD_GAP = 10, CARD_MARGIN = 12;
  const CARD_W = Math.floor((GRID_W - CARD_MARGIN * 2 - CARD_GAP * (CARD_COLS - 1)) /
    CARD_COLS);
  const CARD_TOP = HEADER_H + 12;
  const CARD_H = Math.floor((H - FOOTER_H - CARD_TOP - 12 -
    CARD_GAP * (CARD_ROWS - 1)) / CARD_ROWS);
  const SLOTS = CARD_COLS * CARD_ROWS;

  const pal = {
    page: 0xf2f5f8, card: 0xffffff, line: 0xdce4eb, ink: 0x26313b,
    muted: 0x7d8995, blue: 0x08a6df, blueSoft: 0xe1f3fb, soft: 0xf5f8fa,
    track: 0xe0e7ed, good: 0x1fa78a, warn: 0xd97a1f, yellow: 0xf5b91b,
    teal: 0x26ad93, orange: 0xf08a24, purple: 0x8a68c7, red: 0xd95757,
    yellowSoft: 0xfff5d6, tealSoft: 0xe4f5f1, orangeSoft: 0xffeee0,
    purpleSoft: 0xf0eafb
  };

  /* One cache keeps a re-render cheap: only changed widgets reach the runtime. */
  const paint = Object.create(null);
  let loggedEntities = -1;

  function pushText(id, text) {
    const key = 't' + id;
    if (paint[key] === text) return;
    paint[key] = text;
    ui.setText(id, text);
  }

  function pushColor(id, color) {
    const key = 'c' + id;
    if (paint[key] === color) return;
    paint[key] = color;
    ui.setColor(id, color);
  }

  function pushHidden(id, hidden) {
    const key = 'h' + id;
    const value = hidden ? 1 : 0;
    if (paint[key] === value) return;
    paint[key] = value;
    ui.setHidden(id, !!hidden);
  }

  function pushSize(id, w, h) {
    const key = 's' + id;
    const value = w + ',' + h;
    if (paint[key] === value) return;
    paint[key] = value;
    ui.setSize(id, w, h);
  }

  function pushPos(id, x, y) {
    const key = 'p' + id;
    const value = x + ',' + y;
    if (paint[key] === value) return;
    paint[key] = value;
    ui.setPos(id, x, y);
  }

  function pushStyle(id, tag, style) {
    const key = 'y' + tag + id;
    const value = JSON.stringify(style);
    if (paint[key] === value) return;
    paint[key] = value;
    if (typeof ui.setStyle === 'function') ui.setStyle(id, style);
  }

  function hide(id, hidden) {
    if (id === null || id === undefined) return;
    if (typeof id === 'object') id.hidden = hidden;
    else pushHidden(id, hidden);
  }

  function buttonColor(button, color) { button.color = color; }
  function buttonText(button, text) { button.text = text; }

  function createButton(text, x, y, w, h, action, color, style) {
    const button = { text, x, y, w, h, action, color,
      style: style || styles.control, hidden: false, section: 'common' };
    buttons.push(button);
    return button;
  }

  function appendButtonSlot() {
    const slot = { id: 0, target: null, signature: '' };
    slot.id = ui.button('', 0, 0, 4, 4, () => {
      const target = slot.target;
      if (!disposed && target && !target.hidden) target.action();
    }, pal.card);
    pushHidden(slot.id, true);
    buttonPool.push(slot);
  }

  function initializeButtonPool() {
    for (let i = 0; i < INITIAL_BUTTON_SLOTS; i++) appendButtonSlot();
  }

  function expandButtonPool() {
    while (buttonPool.length < BUTTON_SLOTS) appendButtonSlot();
  }

  function renderButtons() {
    const view = settingsOpen ? 'settings' : detailOpen ? 'detail' : 'list';
    if (buttons.length > buttonPool.length) throw new Error('Button pool capacity exceeded');
    for (let index = 0; index < buttonPool.length; index++) {
      const slot = buttonPool[index];
      const button = index < buttons.length ? buttons[index] : null;
      const active = !!button && (button.section === 'common' ||
        (button.section === 'card' || button.section === 'cardIcon' ?
          button._cardVisible : button.section === view));
      slot.target = active ? button : null;
      const style = active ? button.style : null;
      const signature = active ? [button.x, button.y, button.w, button.h, button.text,
        button.color, button.hidden ? 1 : 0, style.radius, style.borderWidth,
        style.borderColor || 0, style.fontSize || 0].join('|') : 'off';
      if (slot.signature === signature) continue;
      slot.signature = signature;
      if (!active) {
        pushHidden(slot.id, true);
        continue;
      }
      pushPos(slot.id, button.x, button.y);
      pushSize(slot.id, button.w, button.h);
      pushText(slot.id, button.text);
      pushColor(slot.id, button.color);
      pushStyle(slot.id, 'B', button.style);
      pushHidden(slot.id, button.hidden);
    }
  }

  function label(text, x, y, w, h, font, color, icon) {
    const id = ui.text(text, x, y, font, color, icon ? 1 : 0);
    ui.setSize(id, w, h);
    return id;
  }

  function panel(x, y, w, h, color, radius) {
    const id = ui.panel(x, y, w, h, color);
    if (typeof ui.setStyle === 'function') {
      ui.setStyle(id, { radius: Math.max(0, Math.min(64, radius || 10)), borderWidth: 0 });
    }
    return id;
  }

  function borderedPanel(x, y, w, h, color, radius) {
    const id = ui.panel(x, y, w, h, color);
    if (typeof ui.setStyle === 'function') {
      ui.setStyle(id, { radius: radius || 10, borderWidth: 1, borderColor: pal.line });
    }
    return id;
  }

  function iconButton(symbol, x, y, w, h, action, color) {
    const button = createButton('', x, y, w, h, action, color || pal.card,
      { radius: 8, borderWidth: 0 });
    const icon = label(symbol, x + Math.floor((w - 24) / 2), y + Math.floor((h - 24) / 2),
      24, 24, 20, pal.muted, true);
    return { button, icon };
  }

  function pairHidden(pair, value) {
    hide(pair.button, value);
    hide(pair.icon, value);
  }

  function put(id, x, y, w, h, font, color, text) {
    ui.setPos(id, x, y);
    pushSize(id, w, h);
    pushStyle(id, 'L', { fontSize: font, textColor: color });
    pushText(id, text);
  }

  /* ------------------------------ entity model ---------------------------- */

  function isNumeric(item) {
    return !!item && item.state !== '' && Number.isFinite(Number(item.state));
  }

  function isTemperature(item) {
    if (!item) return false;
    if (/(?:\u00b0[CF]|\u2103|\u2109)/i.test(String(item.unit || '')) ||
        /temperature|temp|\u6e29\u5ea6/i.test(item.name + ' ' + item.entity_id)) return true;
    return !!item && (item.unit.indexOf('°') >= 0 ||
      /temperature|temp|温度/i.test(item.name + ' ' + item.entity_id));
  }

  function isHumidity(item) {
    if (item && /humidity|moisture|\u6e7f\u5ea6/i.test(item.name + ' ' + item.entity_id)) return true;
    return !!item && /humidity|湿度|moisture/i.test(item.name + ' ' + item.entity_id);
  }

  function controllable(item) {
    return !!item && ['light', 'switch', 'input_boolean', 'fan'].includes(item.domain);
  }

  const STATE_WORDS = {
    on: '已开启', off: '已关闭', unavailable: '不可用', unknown: '未知',
    open: '已打开', opening: '正在打开', closing: '正在关闭', closed: '已关闭',
    playing: '播放中', paused: '已暂停', idle: '空闲', standby: '待机',
    home: '在家', not_home: '离家', away: '外出',
    above_horizon: '日间', below_horizon: '夜间',
    heat: '制热', cool: '制冷', heat_cool: '自动', auto: '自动',
    dry: '除湿', fan_only: '送风', heating: '正在制热', cooling: '正在制冷',
    locked: '已上锁', unlocked: '已解锁', detected: '已触发', clear: '正常',
    sunny: '晴', cloudy: '多云', partlycloudy: '局部多云', rainy: '下雨',
    pouring: '大雨', snowy: '下雪', fog: '雾', windy: '大风', lightning: '雷电',
    charging: '充电中', discharging: '放电中', full: '已充满'
  };

  function stateLabel(item) {
    const key = String(item.state);
    return STATE_WORDS[key] || STATE_WORDS[key.toLowerCase()] || key;
  }

  function numberText(value) {
    const number = Number(value);
    if (!Number.isFinite(number)) return String(value);
    return Math.abs(number) >= 100 ? number.toFixed(0) : number.toFixed(1);
  }

  function metricText(item) {
    if (!item) return '';
    if (item.state === 'unknown' || item.state === 'unavailable') return '--';
    const unit = item.unit || '';
    if (!unit) return numberText(item.state);
    if (unit === '%') return numberText(item.state) + '%';
    return numberText(item.state) + ' ' + unit;
  }

  function sensorLabel(item) {
    const text = item.name + ' ' + item.entity_id;
    if (isTemperature(item)) return '温度';
    if (isHumidity(item)) return '湿度';
    if (/illuminance|lux|光照/i.test(text)) return '光照';
    if (/voltage|电压/i.test(text)) return '电压';
    if (/power|功率/i.test(text)) return '功率';
    if (/current|电流/i.test(text)) return '电流';
    if (/battery|电量/i.test(text)) return '电量';
    if (/rain|降水|雨/i.test(text)) return '降水量';
    if (/co2|carbon/i.test(text)) return '二氧化碳';
    if (/pressure|气压/i.test(text)) return '气压';
    return '传感器';
  }

  function cardValue(item) {
    if (controllable(item)) {
      if (item.brightness !== null && item.state === 'on') return item.brightness + '%';
      return stateLabel(item);
    }
    if (isNumeric(item)) return metricText(item);
    return stateLabel(item);
  }

  function cardSubtitle(item) {
    if (!available(item)) return item.state === 'unavailable' ? '不可用' : '未知';
    if (controllable(item)) {
      if (item.state !== 'on') return '已关闭';
      return item.brightness !== null ? '已开启 · ' + item.brightness + '%' : '已开启';
    }
    if (isNumeric(item)) return sensorLabel(item);
    return stateLabel(item);
  }

  function cardIcon(item) {
    if (isTemperature(item)) return '°C';
    if (isHumidity(item)) return '\uf043';
    if (/illuminance|lux|光照/i.test(item.name + ' ' + item.entity_id)) return '\uf06e';
    if (/co2|carbon/i.test(item.name + ' ' + item.entity_id)) return 'CO';
    if (item.domain === 'binary_sensor' &&
        /smoke|moisture|gas|carbon|door|window|garage|烟雾|燃气|漏水|门窗/i
          .test(item.name)) return '\uf071';
    return {
      light: '\uf0e7', switch: '\uf011', input_boolean: '\uf011', fan: '\uf028',
      cover: '\uf0c9', media_player: '\uf04b', climate: '\uf0e7',
      sun: '\uf043', weather: '\uf043', vacuum: '\uf021', lock: '\uf06e',
      camera: '\uf03e', alarm_control_panel: '\uf071', binary_sensor: '\uf06e',
      update: '\uf019', person: '\uf124', device_tracker: '\uf124',
      sensor: '\uf06e', automation: '\uf011', script: '\uf011',
      scene: '\uf0c9', button: '\uf067', group: '\uf00b'
    }[item.domain] || '\uf015';
  }

  function fit(value, width, font) {
    const text = clean(value, 160);
    let used = 0, output = '';
    for (const char of Array.from(text)) {
      const step = char.charCodeAt(0) > 127 ? font : font * 0.66;
      if (used + step > width - font) return output + '…';
      used += step;
      output += char;
    }
    return output;
  }

  /* Legacy layouts retained as source reference only.
  // ------------------------------- build ----------------------------------

  function buildUi() {
    ui.background(pal.page);
    const out = { cards: [], settings: [] };
    initializeButtonPool();

    out.sidebar = panel(0, 0, SIDEBAR_W, H, pal.card, 0);
    out.brand = label('Home Assistant', 12, 14, SIDEBAR_W - 24, 24, 15, pal.ink);
    out.menu = [];
    ['概述', '设备', '环境', '设置'].forEach((name, index) => {
      const y = 48 + index * 40;
      out.menu.push({
        panel: index === 0 ? panel(8, y, SIDEBAR_W - 16, 34, pal.blueSoft, 8) : null,
        text: label(name, 20, y + 7, SIDEBAR_W - 32, 20, 14,
          index === 0 ? pal.blue : pal.ink)
      });
    });
    out.stats = [];
    for (let i = 0; i < 3; i++) {
      out.stats.push({
        dot: panel(12, 232 + i * 26, 10, 10, i === 0 ? pal.blue : pal.good, 5),
        text: label('', 30, 228 + i * 26, SIDEBAR_W - 44, 20, 12, pal.muted)
      });
    }
    out.note = label('本地数据 · 非模拟', 12, H - 26, SIDEBAR_W - 24, 18, 11, pal.muted);

    out.header = panel(GRID_X, 0, GRID_W, HEADER_H, pal.blue, 0);
    out.back = iconButton('\uf053', GRID_X + 8, 10, 28, 26, () => {
      if (settingsOpen) { settingsOpen = false; render(); }
    }, pal.blue);
    out.title = label('概览', GRID_X + 44, 11, 120, 24, 19, 0xffffff);
    out.status = label('', GRID_W - 250, 13, 190, 20, 13, 0xffffff);
    pushStyle(out.status, 'L', { fontSize: 13, textColor: 0xffffff, center: 1 });
    out.refresh = iconButton('\uf021', W - 44, 9, 32, 28, refresh, pal.blue);
    out.settingsButton = iconButton('\uf013', W - 82, 9, 32, 28, () => {
      settingsOpen = !settingsOpen;
      render();
    }, pal.blue);
    const topW = Math.floor((GRID_W - 3 * M) / 2);
    out.syncButton = createButton('同步状态', GRID_X + M, HEADER_H + 8, topW,
      TOPROW_H - 16, refresh, pal.card,
      { radius: 8, borderWidth: 1, fontSize: 14 });
    out.linkButton = createButton('连接设置', GRID_X + 2 * M + topW, HEADER_H + 8,
      topW, TOPROW_H - 16, () => { settingsOpen = !settingsOpen; render(); },
      pal.card, { radius: 8, borderWidth: 1, fontSize: 14 });
    out.syncButton.section = 'list';
    out.linkButton.section = 'list';

    for (let index = 0; index < SLOTS; index++) {
      const cx = GRID_X + M + (index % COLS) * (CARD_W + GAP);
      const cy = GRID_Y + M + Math.floor(index / COLS) * (CARD_H + GAP);
      const card = {
        item: null,
        panel: borderedPanel(cx, cy, CARD_W, CARD_H, pal.card, 10),
        icon: label('', cx + 14, cy + 12, 24, 24, 20, pal.muted, true),
        name: label('', cx + 46, cy + 14, CARD_W - 58, 22, 14, pal.ink),
        value: label('', cx + 16, cy + 48, CARD_W - 32, 34, 26, pal.ink),
        state: label('', cx + 16, cy + 88, CARD_W - 32, 18, 12, pal.muted),
        track: panel(cx + 16, cy + CARD_H - 24, CARD_W - 32, 6, pal.track, 3),
        fill: panel(cx + 16, cy + CARD_H - 24, 2, 6, pal.blue, 3)
      };
      const button = createButton('', cx, cy, CARD_W, CARD_H, () => {
        if (!card.item) return;
        selectOrToggle(card.item);
      }, pal.card, { radius: 10, borderWidth: 0 });
      button.section = 'card';
      button._cardVisible = false;
      card.button = button;
      out.cards.push(card);
    }

    out.footer = panel(GRID_X, H - FOOTER_H, GRID_W, FOOTER_H, pal.card, 0);
    out.footerLine = panel(GRID_X, H - FOOTER_H, GRID_W, 1, pal.line, 0);
    out.selectedName = label('', GRID_X + 12, H - FOOTER_H + 8, 300, 22, 15, pal.ink);
    out.selectedState = label('', GRID_X + 12, H - FOOTER_H + 30, 300, 16, 12, pal.muted);
    out.on = createButton('开启', GRID_X + 320, H - FOOTER_H + 10, 72, 32,
      () => control('turn_on'), pal.blueSoft, { radius: 8, borderWidth: 0, fontSize: 14 });
    out.off = createButton('关闭', GRID_X + 400, H - FOOTER_H + 10, 72, 32,
      () => control('turn_off'), pal.card, { radius: 8, borderWidth: 1, fontSize: 14 });
    out.minus = createButton('\uf068', GRID_X + 486, H - FOOTER_H + 10, 34, 32,
      () => changeBrightness(-10), pal.card, { radius: 8, borderWidth: 0, fontSize: 14 });
    out.level = label('', GRID_X + 522, H - FOOTER_H + 15, 56, 22, 14, pal.ink);
    out.plus = createButton('\uf067', GRID_X + 580, H - FOOTER_H + 10, 34, 32,
      () => changeBrightness(10), pal.card, { radius: 8, borderWidth: 0, fontSize: 14 });
    out.pages = label('', GRID_X + GRID_W - 300, H - FOOTER_H + 16, 200, 20, 13, pal.muted);
    pushStyle(out.pages, 'L', { fontSize: 13, textColor: pal.muted, center: 1 });
    out.previous = iconButton('\uf053', W - 100, H - FOOTER_H + 11, 36, 30, () => {
      if (roomPage > 0) roomPage--;
      render();
    }, pal.card);
    out.next = iconButton('\uf054', W - 58, H - FOOTER_H + 11, 36, 30, () => {
      if (roomPage + 1 < pageCount()) roomPage++;
      render();
    }, pal.card);
    [out.on, out.off, out.minus, out.plus].forEach(button => {
      button.section = 'common';
    });

    out.settingsPanel = borderedPanel(GRID_X + M, GRID_Y + M, GRID_W - 2 * M,
      GRID_H - 2 * M, pal.card, 10);
    out.url = label('', GRID_X + 24, GRID_Y + 24, GRID_W - 48, 24, 16, pal.ink);
    out.secret = label('', GRID_X + 24, GRID_Y + 54, GRID_W - 48, 24, 14, pal.muted);
    out.transport = label('', GRID_X + 24, GRID_Y + 84, GRID_W - 48, 24, 14, pal.warn);
    const settingsWidth = Math.floor((GRID_W - 60) / 2);
    [
      ['服务端', promptUrl], ['访问令牌', promptToken],
      ['允许 HTTP', () => {
        if (httpAllowed) { invalidate(); httpAllowed = false; }
        else httpAllowed = true;
        message = httpAllowed ? '已允许本次局域网明文连接' : '已撤销 HTTP 授权';
        render();
      }],
      ['全部实体', () => {
        if (pending || nativeBusy || queued) { message = '请求结束后才能切换范围'; render(); return; }
        scope = scope === 'all' ? 'favorites' : 'all';
        save('eh_scope', scope); selectedId = '';
        if (connected) { beginStates(); startQueued(); }
        render();
      }],
      ['添加关注', addFavorite], ['连接', connect], ['断开', disconnect]
    ].forEach((entry, index) => {
      const button = createButton(entry[0], GRID_X + 24 + (index % 2) * (settingsWidth + 12),
        GRID_Y + 130 + Math.floor(index / 2) * 46, settingsWidth, 36, entry[1],
        index === 5 ? pal.blueSoft : pal.card,
        { radius: 8, borderWidth: 1, fontSize: 14 });
      button.section = 'settings';
      out.settings.push(button);
    });
    return out;
  }

  // ------------------------------- render ---------------------------------

  function pageCount() {
    return Math.max(1, Math.ceil(Math.max(0, entities.length) / SLOTS));
  }

  function rankOf(item) {
    if (controllable(item)) return 0;
    if (isNumeric(item) && isHumidity(item)) return 1;
    if (isNumeric(item) && isTemperature(item)) return 2;
    if (isNumeric(item)) return 3;
    return 4;
  }

  function renderCard(card, item) {
    const x = card.button.x, y = card.button.y;
    card.item = item;
    card.button._cardVisible = false;
    hide(card.panel, !item);
    hide(card.icon, !item);
    hide(card.name, !item);
    hide(card.value, !item);
    hide(card.state, !item);
    hide(card.track, true);
    hide(card.fill, true);
    if (!item) return;
    card.button._cardVisible = true;
    const tint = available(item) ? (controllable(item) && item.state !== 'on' ?
      pal.muted : pal.blue) : pal.warn;
    pushText(card.icon, cardIcon(item));
    pushColor(card.icon, tint);
    pushText(card.name, fit(item.name, CARD_W - 58, 14));
    pushText(card.value, fit(cardValue(item), CARD_W - 32, 26));
    pushColor(card.value, available(item) ? pal.ink : pal.muted);
    pushText(card.state, fit(cardSubtitle(item), CARD_W - 32, 12));
    pushColor(card.state, available(item) ? pal.muted : pal.warn);
    if (item.dimmable && item.brightness !== null) {
      const pct = Math.max(0, Math.min(100, item.brightness || 0));
      pushSize(card.fill, Math.max(2, Math.round((CARD_W - 32) * pct / 100)), 6);
      pushColor(card.fill, item.state === 'on' ? pal.blue : pal.muted);
      hide(card.track, false);
      hide(card.fill, false);
    }
  }

  function render() {
    if (!widgets || disposed) return;
    if (entities.length !== loggedEntities) {
      loggedEntities = entities.length;
      try {
        if (typeof console !== 'undefined' && console && typeof console.log === 'function') {
          console.log('ha-data entities=' + entities.length + ' scope=' + scope +
            ' cached=' + lastCached);
        }
      } catch (logError) { }
    }
    const showDashboard = !settingsOpen;
    const selected = current();
    const pages = pageCount();
    if (roomPage > pages - 1) roomPage = pages - 1;

    hide(widgets.sidebar, !showDashboard);
    hide(widgets.brand, !showDashboard);
    hide(widgets.note, !showDashboard);
    hide(widgets.header, !showDashboard);
    hide(widgets.title, !showDashboard);
    hide(widgets.status, !showDashboard);
    hide(widgets.footer, !showDashboard);
    hide(widgets.footerLine, !showDashboard);
    hide(widgets.selectedName, !showDashboard);
    hide(widgets.selectedState, !showDashboard);
    hide(widgets.pages, !showDashboard);
    widgets.menu.forEach((entry, index) => {
      hide(entry.text, !showDashboard);
      if (entry.panel) hide(entry.panel, !showDashboard);
      pushColor(entry.text, index === 0 ? pal.blue : pal.ink);
    });
    const controls = entities.filter(controllable);
    const online = entities.filter(available).length;
    const running = entities.filter(item => available(item) && item.state === 'on').length;
    widgets.stats.forEach((entry, index) => {
      hide(entry.dot, !showDashboard);
      hide(entry.text, !showDashboard);
      pushText(entry.text, index === 0 ? '设备 ' + controls.length :
        index === 1 ? '在线 ' + online : '运行 ' + running);
    });
    pairHidden(widgets.refresh, !showDashboard);
    pairHidden(widgets.settingsButton, settingsOpen);
    pairHidden(widgets.back, !settingsOpen);
    pushText(widgets.status, connected ? (lastCached ? '缓存状态' : '已同步') : '连接中…');
    pushText(widgets.pages, '第 ' + (roomPage + 1) + ' / ' + pages + ' 页 · ' +
      entities.length + ' 个实体');
    pushText(widgets.selectedName, selected ? selected.name : '未选择设备');
    pushText(widgets.selectedState, selected ?
      (selected.entity_id + ' · ' + cardSubtitle(selected)) : message);
    pairHidden(widgets.previous, !showDashboard || roomPage === 0);
    pairHidden(widgets.next, !showDashboard || roomPage + 1 >= pages);

    const usable = showDashboard && selected && connected && available(selected) &&
      controllable(selected) && !pending && !queued && !nativeBusy;
    hide(widgets.on, !usable || !supports(selected, 'turn_on'));
    hide(widgets.off, !usable || !supports(selected, 'turn_off'));
    const dimmer = usable && selected.dimmable && selected.brightness !== null &&
      supports(selected, 'turn_on');
    [widgets.minus, widgets.level, widgets.plus].forEach(id => hide(id, !dimmer));
    if (dimmer) pushText(widgets.level, selected.brightness + '%');

    const ranked = entities.slice().sort((a, b) =>
      (a._rank === undefined ? (a._rank = rankOf(a)) : a._rank) -
      (b._rank === undefined ? (b._rank = rankOf(b)) : b._rank));
    const offset = roomPage * SLOTS;
    widgets.cards.forEach((card, index) =>
      renderCard(card, ranked[offset + index] || null));

    hide(widgets.settingsPanel, !settingsOpen);
    [widgets.url, widgets.secret, widgets.transport].forEach(id => hide(id, !settingsOpen));
    widgets.settings.forEach(button => hide(button, !settingsOpen));
    if (settingsOpen) {
      pushText(widgets.url, fit('家庭中枢 · ' + url, GRID_W - 48, 16));
      pushText(widgets.secret, token ? '令牌：已输入（仅本次会话）' :
        nativeManaged ? '令牌：由本地服务管理' : '令牌：未输入');
      pushText(widgets.transport, httpAllowed || nativeManaged ?
        '本地网络 · HTTP 明文连接' : '连接前需要确认本地 HTTP 风险');
      buttonText(widgets.settings[2], httpAllowed ? 'HTTP 已允许' : '允许 HTTP');
      buttonText(widgets.settings[3], scope === 'favorites' ? '仅关注' : '全部实体');
      buttonText(widgets.settings[5], connected ? '重新同步' : '连接家庭');
    }
    renderButtons();
  }

  // Reference dashboard layout. It leaves the transport/state machine untouched.
  function buildUiReferenceLegacy() {
    ui.background(pal.page);
    const out = { cards: [], settings: [], deviceRows: [], summary: [] };
    initializeButtonPool();
    out.sidebar = panel(0, 0, SIDEBAR_W, H, pal.card, 0);
    out.brand = label('Home Assistant', 16, 22, SIDEBAR_W - 26, 26, 16, pal.ink);
    out.menu = [];
    ['\u603b\u89c8', '\u8bbe\u5907', '\u73af\u5883', '\u8bbe\u7f6e'].forEach((name, index) => {
      const y = 78 + index * 44;
      out.menu.push({ panel: index === 0 ? panel(7, y, SIDEBAR_W - 14, 36, pal.blueSoft, 10) : null,
        text: label(name, 24, y + 8, SIDEBAR_W - 38, 20, 15, index === 0 ? pal.blue : pal.ink) });
    });
    out.stats = [];
    for (let i = 0; i < 3; i++) out.stats.push({
      dot: panel(18, 332 + i * 34, 10, 10, i === 0 ? pal.blue : pal.good, 5),
      text: label('', 36, 328 + i * 34, SIDEBAR_W - 48, 20, 13, pal.muted)
    });
    out.note = label('\u672c\u6b21\u8fd0\u884c  ·  \u672c\u5730\u91c7\u6837', 16, H - 52, SIDEBAR_W - 26, 18, 11, pal.muted);

    out.header = panel(GRID_X, 0, GRID_W, HEADER_H, pal.blue, 0);
    out.back = iconButton('\uf053', GRID_X + 12, 16, 30, 30, () => { if (settingsOpen) { settingsOpen = false; render(); } }, pal.blue);
    out.title = label('\u6982\u89c8', GRID_X + 54, 17, 108, 28, 20, 0xffffff);
    out.tabs = [label('\u6807\u51c6', GRID_X + 228, 21, 54, 22, 14, 0xffffff),
      label('\u7d27\u51d1', GRID_X + 294, 21, 54, 22, 14, 0xffffff),
      label('\u8212\u9002', GRID_X + 360, 21, 54, 22, 14, 0xffffff),
      panel(GRID_X + 430, 11, 62, 42, 0xffffff, 22),
      label('\u5206\u7ec4', GRID_X + 506, 21, 54, 22, 14, 0xffffff)];
    for (const id of [out.tabs[0], out.tabs[1], out.tabs[2], out.tabs[4]]) pushStyle(id, 'L', { center: 1 });
    out.classic = label('\u7ecf\u5178', GRID_X + 434, 21, 54, 22, 14, pal.blue);
    pushStyle(out.classic, 'L', { center: 1 });
    out.status = label('', W - 220, 21, 110, 22, 13, 0xffffff); pushStyle(out.status, 'L', { center: 1 });
    out.refresh = iconButton('\uf021', W - 44, 16, 32, 28, refresh, pal.blue);
    out.settingsButton = iconButton('\uf013', W - 82, 16, 32, 28, () => { settingsOpen = !settingsOpen; render(); }, pal.blue);

    const topY = HEADER_H + 16, leftX = SIDEBAR_W + 14;
    out.syncButton = createButton('\u540c\u6b65\u72b6\u6001', leftX, topY, 126, 46, refresh, pal.card,
      { radius: 12, borderWidth: 1, borderColor: pal.line, fontSize: 14 });
    out.linkButton = createButton('\u8fde\u63a5\u8bbe\u7f6e', leftX + 138, topY, 126, 46,
      () => { settingsOpen = !settingsOpen; render(); }, pal.card,
      { radius: 12, borderWidth: 1, borderColor: pal.line, fontSize: 14 });
    out.syncButton.section = 'list'; out.linkButton.section = 'list';
    const deviceY = topY + 60;
    out.devicePanel = borderedPanel(leftX, deviceY, 264, 244, pal.card, 10);
    out.deviceTitle = label('\u8bbe\u5907', leftX + 16, deviceY + 16, 180, 24, 17, pal.ink);
    ['\u7a7a\u6c14\u51c0\u5316\u5668', '\u4e66\u623f\u98ce\u6247', '\u5496\u5561\u673a\u63d2\u5ea7', '\u4e66\u684c\u7535\u6e90'].forEach((name, index) => {
      const y = deviceY + 52 + index * 44;
      const row = { item: null, name: label(name, leftX + 16, y, 160, 18, 14, pal.ink),
        state: label('', leftX + 16, y + 20, 150, 16, 12, pal.muted), button: null, knob: null };
      pushStyle(row.name, 'L', { ellipsis: 1 });
      row.button = createButton('', leftX + 190, y + 2, 54, 28, () => { if (row.item) selectOrToggle(row.item); }, pal.blue, { radius: 14, borderWidth: 0 });
      row.knob = panel(leftX + 194, y + 6, 20, 20, pal.card, 10);
      row.button.section = 'card'; row.button._cardVisible = true;
      out.deviceRows.push(row);
    });

    function lamp(x, y, title) {
      const c = { kind: 'lamp', item: null, panel: borderedPanel(x, y, LAMP_W, LAMP_H, pal.card, 10),
        arc: ui.arc(x + 17, y + 7, 92, 92, 0, pal.track, pal.blue),
        icon: label('\uf0e7', x + 47, y + 31, 32, 32, 28, pal.yellow, true),
        name: label(title, x + 8, y + 105, LAMP_W - 16, 20, 14, pal.ink),
        state: label('', x + 8, y + 125, LAMP_W - 16, 18, 12, pal.muted), button: null };
      pushStyle(c.name, 'L', { center: 1 }); pushStyle(c.state, 'L', { center: 1 });
      c.button = createButton('', x, y, LAMP_W, LAMP_H, () => { if (c.item) selectOrToggle(c.item); }, pal.card, { radius: 10, borderWidth: 0 });
      c.button.section = 'card'; c.button._cardVisible = true; out.cards.push(c);
    }
    const lampX = leftX + 282;
    lamp(lampX, topY, '\u5ba2\u5385\u4e3b\u706f'); lamp(lampX + LAMP_W + LAMP_GAP, topY, '\u5367\u5ba4\u706f');
    lamp(lampX, topY + LAMP_H + LAMP_GAP, '\u9910\u5385\u540a\u706f'); lamp(lampX + LAMP_W + LAMP_GAP, topY + LAMP_H + LAMP_GAP, '\u7384\u5173\u706f\u5e26');
    const sensorX = leftX + 560, sensorW = W - sensorX - 14;
    const temp = { kind: 'temp', item: null, panel: borderedPanel(sensorX, topY, sensorW, 145, pal.card, 10),
      name: label('\u5ba2\u5385\u6e29\u5ea6', sensorX + 16, topY + 16, sensorW - 32, 22, 16, pal.ink),
      value: label('--', sensorX + 16, topY + 48, sensorW - 32, 36, 27, pal.ink),
      line: ui.line([[0, 22], [20, 31], [40, 16], [60, 24], [80, 32], [100, 12], [120, 21], [140, 30], [160, 6], [180, 16], [200, 8], [220, 20]], sensorX + 16, topY + 87, sensorW - 32, 34, 0, pal.blue),
      range: label('\u672c\u6b21 23.0 - 24.8', sensorX + 16, topY + 124, sensorW - 32, 16, 11, pal.muted) };
    temp.button = createButton('', sensorX, topY, sensorW, 145, () => { if (temp.item) selectedId = temp.item.entity_id; render(); }, pal.card, { radius: 10, borderWidth: 0 }); temp.button.section = 'card'; temp.button._cardVisible = true; out.cards.push(temp);
    const hy = topY + 155;
    const humidity = { kind: 'humidity', item: null, panel: borderedPanel(sensorX, hy, sensorW, 145, pal.card, 10),
      name: label('\u5ba4\u5185\u6e7f\u5ea6', sensorX + 16, hy + 16, sensorW - 32, 22, 16, pal.ink), value: label('--', sensorX + 16, hy + 48, 120, 36, 27, pal.ink),
      arc: ui.arc(sensorX + sensorW - 145, hy + 25, 112, 84, 48, pal.track, pal.teal, 180, 180), range: label('0 - 100%', sensorX + sensorW - 145, hy + 116, 112, 16, 11, pal.muted) };
    pushStyle(humidity.range, 'L', { center: 1 }); humidity.button = createButton('', sensorX, hy, sensorW, 145, () => { if (humidity.item) selectedId = humidity.item.entity_id; render(); }, pal.card, { radius: 10, borderWidth: 0 }); humidity.button.section = 'card'; humidity.button._cardVisible = true; out.cards.push(humidity);

    const summaryY = H - FOOTER_H - 84;
    ['\u5bb6\u5ead\u6982\u51b5', '\u5bb6\u5ead\u4e2d\u67a2', '\u5165\u6237\u95e8\u7a97'].forEach((title, index) => {
      const x = leftX + index * 278;
      const value = label('', x + 16, summaryY + 36, 238, 24, index === 1 ? 12 : 20, pal.ink);
      pushStyle(value, 'L', { ellipsis: 1 });
      out.summary.push({ panel: borderedPanel(x, summaryY, 264, 72, pal.card, 10), title: label(title, x + 16, summaryY + 13, 230, 18, 13, pal.muted), value });
    });

    out.footer = panel(GRID_X, H - FOOTER_H, GRID_W, FOOTER_H, pal.card, 0); out.footerLine = panel(GRID_X, H - FOOTER_H, GRID_W, 1, pal.line, 0);
    out.selectedName = label('', GRID_X + 16, H - FOOTER_H + 8, 260, 22, 16, pal.ink); out.selectedState = label('', GRID_X + 16, H - FOOTER_H + 31, 260, 16, 12, pal.muted);
    out.on = createButton('\u5f00\u542f', GRID_X + 320, H - FOOTER_H + 10, 72, 32, () => control('turn_on'), pal.blueSoft, { radius: 8, borderWidth: 0, fontSize: 14 });
    out.off = createButton('\u5173\u95ed', GRID_X + 400, H - FOOTER_H + 10, 72, 32, () => control('turn_off'), pal.card, { radius: 8, borderWidth: 1, fontSize: 14 });
    out.minus = createButton('\uf068', GRID_X + 486, H - FOOTER_H + 10, 34, 32, () => changeBrightness(-10), pal.card, { radius: 8, borderWidth: 0, fontSize: 14 });
    out.level = label('', GRID_X + 522, H - FOOTER_H + 15, 56, 22, 14, pal.ink); out.plus = createButton('\uf067', GRID_X + 580, H - FOOTER_H + 10, 34, 32, () => changeBrightness(10), pal.card, { radius: 8, borderWidth: 0, fontSize: 14 });
    out.pages = label('', GRID_X + GRID_W - 300, H - FOOTER_H + 16, 200, 20, 13, pal.muted); pushStyle(out.pages, 'L', { center: 1 });
    out.previous = iconButton('\uf053', W - 100, H - FOOTER_H + 11, 36, 30, () => {}, pal.card); out.next = iconButton('\uf054', W - 58, H - FOOTER_H + 11, 36, 30, () => {}, pal.card);
    [out.on, out.off, out.minus, out.plus].forEach(button => { button.section = 'common'; });
    out.settingsPanel = borderedPanel(GRID_X + 18, GRID_Y + 18, GRID_W - 36, H - GRID_Y - FOOTER_H - 36, pal.card, 10);
    out.url = label('', GRID_X + 42, GRID_Y + 42, GRID_W - 84, 24, 16, pal.ink); out.secret = label('', GRID_X + 42, GRID_Y + 72, GRID_W - 84, 24, 14, pal.muted); out.transport = label('', GRID_X + 42, GRID_Y + 102, GRID_W - 84, 24, 14, pal.warn);
    const settingsWidth = Math.floor((GRID_W - 84) / 2);
    [['\u670d\u52a1\u7aef', promptUrl], ['\u8bbf\u95ee\u4ee4\u724c', promptToken], ['\u5141\u8bb8 HTTP', () => { httpAllowed = !httpAllowed; render(); }], ['\u5168\u90e8\u5b9e\u4f53', () => { scope = scope === 'all' ? 'favorites' : 'all'; save('eh_scope', scope); if (connected) { beginStates(); startQueued(); } render(); }], ['\u6dfb\u52a0\u5173\u6ce8', addFavorite], ['\u8fde\u63a5', connect], ['\u65ad\u5f00', disconnect]].forEach((entry, index) => { const b = createButton(entry[0], GRID_X + 42 + (index % 2) * (settingsWidth + 12), GRID_Y + 142 + Math.floor(index / 2) * 46, settingsWidth, 36, entry[1], index === 5 ? pal.blueSoft : pal.card, { radius: 8, borderWidth: 1, fontSize: 14 }); b.section = 'settings'; out.settings.push(b); });
    return out;
  }

  function referenceEntity(list, predicate, offset) {
    const matches = list.filter(predicate || (() => true));
    return matches[offset || 0] || list[offset || 0] || null;
  }

  function renderReferenceLegacy() {
    if (!widgets || disposed) return;
    const show = !settingsOpen;
    const controls = entities.filter(controllable);
    const sensors = entities.filter(item => isNumeric(item));
    const lights = controls.filter(item => item.domain === 'light');
    const temp = referenceEntity(sensors, isTemperature, 0);
    const humidity = referenceEntity(sensors, isHumidity, 0);
    const selected = current();
    const hideAll = [widgets.sidebar, widgets.brand, widgets.footer, widgets.footerLine,
      widgets.selectedName, widgets.selectedState, widgets.pages, widgets.devicePanel, widgets.deviceTitle,
      widgets.note, widgets.syncButton, widgets.linkButton];
    hideAll.forEach(id => hide(id, !show));
    widgets.menu.forEach(entry => { hide(entry.text, !show); if (entry.panel) hide(entry.panel, !show); });
    widgets.stats.forEach((entry, index) => {
      hide(entry.dot, !show); hide(entry.text, !show);
      pushText(entry.text, index === 0 ? '\u8bbe\u5907 ' + controls.length : index === 1 ? '\u5728\u7ebf ' + entities.filter(available).length : '\u8fd0\u884c ' + activeDevices());
    });
    widgets.tabs.forEach(id => hide(id, !show)); hide(widgets.classic, !show);
    hide(widgets.header, false); hide(widgets.title, false);
    pairHidden(widgets.back, !settingsOpen);
    pairHidden(widgets.refresh, !show);
    pairHidden(widgets.settingsButton, !show);
    widgets.deviceRows.forEach((row, index) => {
      const item = controls[index] || null; row.item = item; hide(row.name, !show); hide(row.state, !show); hide(row.button, !show); hide(row.knob, !show);
      pushText(row.state, item ? (available(item) ? cardSubtitle(item) : '\u4e0d\u53ef\u7528') : '\u6682\u65e0\u5b9e\u4f53');
      pushColor(row.state, item && item.state === 'on' ? pal.blue : pal.muted);
      buttonColor(row.button, item && item.state === 'on' ? pal.blue : pal.track);
      pushPos(row.knob, row.button.x + (item && item.state === 'on' ? 30 : 4), row.button.y + 4);
      row.button._cardVisible = show;
    });
    widgets.cards.forEach(card => {
      hide(card.panel, !show); hide(card.button, !show); card.button._cardVisible = show;
      let item = card.kind === 'temp' ? temp : card.kind === 'humidity' ? humidity : null;
      if (card.kind === 'lamp') item = lights[widgets.cards.filter(x => x.kind === 'lamp').indexOf(card)] || controls[widgets.cards.indexOf(card)] || null;
      card.item = item;
      if (card.kind === 'lamp') {
        const pct = item && item.brightness !== null ? item.brightness : item && item.state === 'on' ? 100 : 0;
        if (card._value !== pct) { card._value = pct; ui.arcSet(card.arc, pct); }
        pushColor(card.icon, item && item.state === 'on' ? pal.yellow : pal.muted);
        pushText(card.state, item ? (item.state === 'on' ? '\u5df2\u5f00\u542f  ·  ' + pct + '%' : '\u5df2\u5173\u95ed') : '\u6682\u65e0\u5b9e\u4f53');
        pushColor(card.state, item && item.state === 'on' ? pal.muted : pal.muted);
      } else if (card.kind === 'temp') {
        pushText(card.value, temp ? metricText(temp) : '--');
      } else if (card.kind === 'humidity') {
        const value = humidity && isNumeric(humidity) ? Math.max(0, Math.min(100, Number(humidity.state))) : 0;
        pushText(card.value, humidity ? metricText(humidity) : '--');
        if (card._value !== value) { card._value = value; ui.arcSet(card.arc, value); }
      }
    });
    widgets.summary.forEach((card, index) => {
      hide(card.panel, !show); hide(card.title, !show); hide(card.value, !show);
      pushText(card.value, index === 0 ? entities.filter(available).length + ' / ' + entities.length + ' \u5728\u7ebf' : index === 1 ? url : (entities.some(item => /door|window|\u95e8|\u7a97/i.test(item.name)) ? '\u6b63\u5e38' : '\u65e0\u4f20\u611f\u5668'));
      pushColor(card.value, index === 1 && !nativeManaged ? pal.warn : pal.ink);
    });
    pushText(widgets.status, connected ? (lastCached ? '\u5df2\u540c\u6b65' : '\u5df2\u540c\u6b65') : '\u672a\u8fde\u63a5');
    pushText(widgets.selectedName, selected ? selected.name : '\u672a\u9009\u62e9\u8bbe\u5907');
    pushText(widgets.selectedState, selected ? selected.entity_id + '  ·  ' + cardSubtitle(selected) : message);
    pushText(widgets.pages, '\u7b2c 1 / 1 \u9875  ·  ' + entities.length + ' \u4e2a\u5b9e\u4f53');
    const usable = show && selected && connected && available(selected) && controllable(selected) && !pending && !queued && !nativeBusy;
    hide(widgets.on, !usable || !supports(selected, 'turn_on')); hide(widgets.off, !usable || !supports(selected, 'turn_off'));
    const dimmer = usable && selected.dimmable && selected.brightness !== null && supports(selected, 'turn_on');
    [widgets.minus, widgets.level, widgets.plus].forEach(id => hide(id, !dimmer)); if (dimmer) pushText(widgets.level, selected.brightness + '%');
    pairHidden(widgets.previous, true); pairHidden(widgets.next, true);
    hide(widgets.settingsPanel, !settingsOpen); [widgets.url, widgets.secret, widgets.transport].forEach(id => hide(id, !settingsOpen)); widgets.settings.forEach(button => hide(button, !settingsOpen));
    if (settingsOpen) { pushText(widgets.url, '\u5bb6\u5ead\u4e2d\u67a2  ·  ' + url); pushText(widgets.secret, nativeManaged ? '\u4ee4\u724c\uff1a\u7531\u672c\u5730\u670d\u52a1\u7ba1\u7406' : '\u4ee4\u724c\uff1a\u672a\u8f93\u5165'); pushText(widgets.transport, nativeManaged ? '\u672c\u5730\u7f51\u7edc  ·  HTTP \u8fde\u63a5' : '\u8fde\u63a5\u524d\u9700\u786e\u8ba4 HTTP \u98ce\u9669'); }
    renderButtons();
  }

  */

  function friendlyState(item) {
    const states = {
      on: '\u5df2\u5f00\u542f', off: '\u5df2\u5173\u95ed', unavailable: '\u4e0d\u53ef\u7528',
      unknown: '\u672a\u77e5', open: '\u5df2\u6253\u5f00', closed: '\u5df2\u5173\u95ed',
      opening: '\u6b63\u5728\u6253\u5f00', closing: '\u6b63\u5728\u5173\u95ed', playing: '\u6b63\u5728\u64ad\u653e',
      paused: '\u5df2\u6682\u505c', idle: '\u7a7a\u95f2', standby: '\u5f85\u673a', home: '\u5728\u5bb6',
      not_home: '\u79bb\u5bb6', away: '\u79bb\u5bb6', above_horizon: '\u5730\u5e73\u7ebf\u4e0a',
      below_horizon: '\u5730\u5e73\u7ebf\u4e0b', locked: '\u5df2\u4e0a\u9501', unlocked: '\u5df2\u89e3\u9501',
      heat: '\u5236\u70ed', cool: '\u5236\u51b7', auto: '\u81ea\u52a8', dry: '\u9664\u6e7f',
      fan_only: '\u9001\u98ce', heating: '\u6b63\u5728\u5236\u70ed', cooling: '\u6b63\u5728\u5236\u51b7',
      sunny: '\u6674', cloudy: '\u591a\u4e91', partlycloudy: '\u5c40\u90e8\u591a\u4e91', rainy: '\u4e0b\u96e8',
      charging: '\u5145\u7535\u4e2d', discharging: '\u653e\u7535\u4e2d', full: '\u5df2\u5145\u6ee1'
    };
    const value = String(item && item.state || '');
    return states[value.toLowerCase()] || value;
  }

  function binaryState(item) {
    const active = item.state === 'on';
    const words = {
      door: ['\u5df2\u5173\u95ed', '\u5df2\u6253\u5f00'], window: ['\u5df2\u5173\u95ed', '\u5df2\u6253\u5f00'],
      garage_door: ['\u5df2\u5173\u95ed', '\u5df2\u6253\u5f00'], opening: ['\u5df2\u5173\u95ed', '\u5df2\u6253\u5f00'],
      motion: ['\u65e0\u6d3b\u52a8', '\u68c0\u6d4b\u5230\u6d3b\u52a8'], occupancy: ['\u65e0\u4eba', '\u6709\u4eba'],
      presence: ['\u65e0\u4eba', '\u6709\u4eba'], smoke: ['\u6b63\u5e38', '\u68c0\u6d4b\u5230\u70df\u96fe'],
      gas: ['\u6b63\u5e38', '\u68c0\u6d4b\u5230\u71c3\u6c14'], moisture: ['\u5e72\u71e5', '\u68c0\u6d4b\u5230\u6f0f\u6c34'],
      problem: ['\u6b63\u5e38', '\u9700\u8981\u6ce8\u610f'], connectivity: ['\u79bb\u7ebf', '\u5728\u7ebf']
    };
    const pair = words[item.device_class] || ['\u6b63\u5e38', '\u5df2\u89e6\u53d1'];
    return pair[active ? 1 : 0];
  }

  function dashboardValue(item) {
    if (!available(item)) return '--';
    if (item.domain === 'binary_sensor') return binaryState(item);
    if (item.domain === 'climate') {
      const value = item.current_temperature === null ? item.temperature : item.current_temperature;
      return value === null ? friendlyState(item) : numberText(value) + ' \u00b0C';
    }
    if (item.domain === 'cover' && item.current_position !== null)
      return item.current_position + '%';
    if (item.domain === 'media_player' && item.media_title) return item.media_title;
    if (controllable(item) && item.brightness !== null && item.state === 'on')
      return item.brightness + '%';
    if (isNumeric(item)) return metricText(item);
    return friendlyState(item);
  }

  function dashboardSubtitle(item) {
    if (!available(item)) return item.state === 'unavailable' ? '\u5b9e\u4f53\u79bb\u7ebf' : '\u72b6\u6001\u672a\u77e5';
    if (item.domain === 'climate') {
      const target = item.temperature === null ? '' : '\u76ee\u6807 ' + numberText(item.temperature) + ' \u00b0C';
      const action = item.hvac_action ? friendlyState({ state: item.hvac_action }) : friendlyState(item);
      return target ? action + '  \u00b7  ' + target : action;
    }
    if (item.domain === 'media_player') {
      if (item.volume_level !== null) return friendlyState(item) + '  \u00b7  \u97f3\u91cf ' +
        Math.round(item.volume_level * 100) + '%';
      return friendlyState(item);
    }
    if (item.domain === 'cover') return friendlyState(item) +
      (item.current_position === null ? '' : '  \u00b7  ' + item.current_position + '%');
    if (controllable(item)) return friendlyState(item) +
      (item.brightness !== null && item.state === 'on' ? '  \u00b7  \u4eae\u5ea6' : '');
    if (item.domain === 'sensor') return ({
      temperature: '\u6e29\u5ea6', humidity: '\u6e7f\u5ea6', battery: '\u7535\u91cf',
      power: '\u529f\u7387', energy: '\u80fd\u8017', voltage: '\u7535\u538b', current: '\u7535\u6d41',
      illuminance: '\u5149\u7167', pressure: '\u6c14\u538b', carbon_dioxide: 'CO2'
    })[item.device_class] || '\u4f20\u611f\u5668';
    return friendlyState(item);
  }

  function dashboardIcon(item) {
    const byClass = {
      temperature: '\uf0e7', humidity: '\uf043', battery: '\uf240', power: '\uf0e7',
      energy: '\uf0e7', voltage: '\uf0e7', current: '\uf0e7', illuminance: '\uf06e',
      door: '\uf0c9', window: '\uf0c9', motion: '\uf06e', occupancy: '\uf06e',
      smoke: '\uf071', gas: '\uf071', moisture: '\uf043', connectivity: '\uf1eb'
    };
    if (byClass[item.device_class]) return byClass[item.device_class];
    return {
      light: '\uf0e7', switch: '\uf011', input_boolean: '\uf011', fan: '\uf079',
      sensor: '\uf06e', binary_sensor: '\uf071', cover: '\uf0c9', climate: '\uf0e7',
      media_player: '\uf04b', person: '\uf124', device_tracker: '\uf124', weather: '\uf043',
      sun: '\uf06e', update: '\uf019', lock: '\uf00c', vacuum: '\uf079', camera: '\uf03e',
      alarm_control_panel: '\uf071', automation: '\uf074', script: '\uf158', scene: '\uf03e',
      button: '\uf011', number: '\uf00b', select: '\uf00b', group: '\uf00b'
    }[item.domain] || '\uf015';
  }

  function dashboardTint(item) {
    if (!available(item)) return pal.muted;
    if (isTemperature(item) || item.domain === 'climate') return pal.orange;
    if (isHumidity(item) || item.device_class === 'moisture') return pal.teal;
    if (['weather', 'cover'].includes(item.domain)) return pal.blue;
    if (['update', 'media_player'].includes(item.domain)) return pal.purple;
    if (item.domain === 'binary_sensor' && item.state === 'on') return pal.red;
    if (item.domain === 'light' && item.state === 'on') return pal.yellow;
    if (controllable(item) && item.state === 'on') return pal.blue;
    return pal.muted;
  }

  function progressValue(item) {
    if (!available(item)) return null;
    if (item.brightness !== null) return item.state === 'on' ? item.brightness : 0;
    if (item.current_position !== null) return item.current_position;
    if (item.volume_level !== null) return Math.round(item.volume_level * 100);
    if (item.battery_level !== null) return item.battery_level;
    if (item.device_class === 'humidity' && isNumeric(item)) return Number(item.state);
    if ((item.device_class === 'battery' || item.unit === '%') && isNumeric(item)) return Number(item.state);
    return null;
  }

  function domainTitle(item) {
    return {
      light: '\u706f\u5149', switch: '\u5f00\u5173', input_boolean: '\u6a21\u5f0f', fan: '\u98ce\u6247',
      sensor: '\u4f20\u611f\u5668', binary_sensor: '\u72b6\u6001', cover: '\u906e\u9633', climate: '\u6e29\u63a7',
      media_player: '\u5a92\u4f53', person: '\u6210\u5458', device_tracker: '\u4f4d\u7f6e', weather: '\u5929\u6c14',
      sun: '\u65e5\u7167', update: '\u66f4\u65b0', lock: '\u95e8\u9501', vacuum: '\u6e05\u6d01', camera: '\u6444\u50cf\u5934',
      alarm_control_panel: '\u5b89\u9632', automation: '\u81ea\u52a8\u5316', script: '\u811a\u672c', scene: '\u573a\u666f'
    }[item.domain] || item.domain.replace(/_/g, ' ');
  }

  function entitySection(item) {
    if (['light', 'switch', 'input_boolean', 'fan', 'cover', 'climate', 'media_player',
      'vacuum', 'lock', 'camera', 'alarm_control_panel'].includes(item.domain)) return 'devices';
    if (['sensor', 'binary_sensor', 'weather', 'sun'].includes(item.domain)) return 'environment';
    return 'system';
  }

  function dashboardEntities() {
    const cacheKey = entityRevision + '|' + nav + '|' + favorites.join(',');
    if (dashboardCacheKey === cacheKey) return dashboardCache;
    const list = nav === 'overview' ? entities.slice() :
      entities.filter(item => entitySection(item) === nav);
    dashboardCache = list.map((item, index) => {
      let score = favorites.includes(item.entity_id) ? -100 : 0;
      if (!available(item)) score += 80;
      if (controllable(item)) score -= 40;
      else if (isTemperature(item) || isHumidity(item)) score -= 30;
      else if (item.domain === 'binary_sensor') score -= 20;
      else if (['climate', 'cover', 'media_player', 'weather'].includes(item.domain)) score -= 10;
      return { item, index, score };
    }).sort((a, b) => a.score - b.score || a.index - b.index).map(row => row.item);
    dashboardCacheKey = cacheKey;
    return dashboardCache;
  }

  function pageCount() {
    return Math.max(1, Math.ceil(dashboardEntities().length / SLOTS));
  }

  function selectSection(section) {
    nav = section;
    roomPage = 0;
    selectedId = '';
    settingsOpen = false;
    detailOpen = false;
    render();
  }

  function buildUiReference() {
    ui.background(pal.page);
    const out = { cards: [], settings: [], menu: [], stats: [] };
    initializeButtonPool();
    out.sidebar = panel(0, 0, SIDEBAR_W, H, pal.card, 0);
    out.brand = label('Home Assistant', 16, 20, SIDEBAR_W - 28, 26, 16, pal.ink);
    const sections = [
      ['overview', '\u603b\u89c8', '\uf015'], ['devices', '\u8bbe\u5907', '\uf00b'],
      ['environment', '\u73af\u5883', '\uf043'], ['system', '\u7cfb\u7edf', '\uf013']
    ];
    sections.forEach((entry, index) => {
      const y = 70 + index * 46;
      const row = {
        key: entry[0], panel: panel(8, y, SIDEBAR_W - 16, 38, pal.card, 8),
        icon: label(entry[2], 22, y + 8, 24, 22, 17, pal.muted, true),
        text: label(entry[1], 50, y + 8, SIDEBAR_W - 62, 22, 14, pal.ink)
      };
      row.button = createButton('', 8, y, SIDEBAR_W - 16, 38,
        () => selectSection(entry[0]), pal.card, { radius: 8, borderWidth: 0 });
      row.button.section = 'list';
      out.menu.push(row);
    });
    for (let i = 0; i < 3; i++) out.stats.push({
      dot: panel(18, 298 + i * 32, 9, 9, i === 0 ? pal.blue : pal.good, 5),
      text: label('', 36, 293 + i * 32, SIDEBAR_W - 48, 20, 12, pal.muted)
    });
    out.note = label('\u5b9e\u65f6\u6765\u81ea Home Assistant', 16, H - 36,
      SIDEBAR_W - 28, 18, 11, pal.muted);

    out.header = panel(GRID_X, 0, GRID_W, HEADER_H, pal.blue, 0);
    out.back = iconButton('\uf053', GRID_X + 12, 16, 32, 30,
      () => {
        if (detailOpen) detailOpen = false;
        else settingsOpen = false;
        render();
      }, pal.blue);
    out.title = label('', GRID_X + 56, 17, 280, 28, 20, 0xffffff);
    out.status = label('', W - 238, 21, 126, 20, 13, 0xffffff);
    pushStyle(out.status, 'L', { center: 1 });
    out.refresh = iconButton('\uf021', W - 44, 16, 32, 28, refresh, pal.blue);
    out.settingsButton = iconButton('\uf013', W - 82, 16, 32, 28,
      () => { detailOpen = false; settingsOpen = true; render(); }, pal.blue);

    for (let index = 0; index < SLOTS; index++) {
      const x = GRID_X + CARD_MARGIN + (index % CARD_COLS) * (CARD_W + CARD_GAP);
      const y = CARD_TOP + Math.floor(index / CARD_COLS) * (CARD_H + CARD_GAP);
      const card = {
        item: null,
        panel: borderedPanel(x, y, CARD_W, CARD_H, pal.card, 10),
        icon: label('', x + 18, y + 18, 29, 29, 23, pal.blue, true),
        type: label('', x + 18, y + 48, CARD_W - 36, 18, 11, pal.muted),
        name: label('', x + 53, y + 20, CARD_W - 71, 22, 14, pal.ink),
        value: label('', x + 18, y + 72, CARD_W - 36, 36, 26, pal.ink),
        state: label('', x + 18, y + 116, CARD_W - 36, 20, 12, pal.muted),
        track: panel(x + 16, y + CARD_H - 18, CARD_W - 32, 5, pal.track, 3),
        fill: panel(x + 16, y + CARD_H - 18, 2, 5, pal.blue, 3)
      };
      pushStyle(card.type, 'L', { ellipsis: 1 });
      pushStyle(card.name, 'L', { ellipsis: 1 });
      pushStyle(card.value, 'L', { ellipsis: 1 });
      pushStyle(card.state, 'L', { ellipsis: 1 });
      card.button = createButton('', x, y, CARD_W, CARD_H,
        () => { if (card.item) openDetail(card.item); }, pal.card,
        { radius: 10, borderWidth: 0 });
      card.button.section = 'card';
      card.button._cardVisible = false;
      card.iconButton = createButton('', x + 6, y + 6, 48, 48,
        () => { if (card.item) toggleItem(card.item); }, pal.card,
        { radius: 24, borderWidth: 0 });
      card.iconButton.section = 'cardIcon';
      card.iconButton._cardVisible = false;
      [card.panel, card.icon, card.type, card.name, card.value, card.state,
        card.track, card.fill].forEach(id => hide(id, true));
      out.cards.push(card);
    }

    out.footer = panel(GRID_X, H - FOOTER_H, GRID_W, FOOTER_H, pal.card, 0);
    out.footerLine = panel(GRID_X, H - FOOTER_H, GRID_W, 1, pal.line, 0);
    out.selectedName = label('', GRID_X + 16, H - FOOTER_H + 7, 278, 22, 15, pal.ink);
    out.selectedState = label('', GRID_X + 16, H - FOOTER_H + 29, 278, 17, 11, pal.muted);
    out.pages = label('', W - 292, H - FOOTER_H + 16, 180, 20, 12, pal.muted);
    pushStyle(out.pages, 'L', { center: 1 });
    out.previous = iconButton('\uf053', W - 100, H - FOOTER_H + 11, 36, 30,
      () => { if (roomPage > 0) { roomPage--; render(); } }, pal.card);
    out.next = iconButton('\uf054', W - 58, H - FOOTER_H + 11, 36, 30,
      () => { if (roomPage + 1 < pageCount()) { roomPage++; render(); } }, pal.card);
    out.settingsPanel = borderedPanel(GRID_X + 18, GRID_Y + 18, GRID_W - 36,
      H - GRID_Y - FOOTER_H - 36, pal.card, 10);
    out.url = label('', GRID_X + 42, GRID_Y + 42, GRID_W - 84, 24, 16, pal.ink);
    out.secret = label('', GRID_X + 42, GRID_Y + 72, GRID_W - 84, 24, 14, pal.muted);
    out.transport = label('', GRID_X + 42, GRID_Y + 102, GRID_W - 84, 24, 14, pal.warn);
    const settingsWidth = Math.floor((GRID_W - 96) / 2);
    [
      ['\u670d\u52a1\u7aef', promptUrl], ['\u8bbf\u95ee\u4ee4\u724c', promptToken],
      ['\u5141\u8bb8 HTTP', () => { httpAllowed = !httpAllowed; render(); }],
      ['\u5168\u90e8\u5b9e\u4f53', () => {
        scope = scope === 'all' ? 'favorites' : 'all';
        save('eh_scope', scope);
        if (connected) { beginStates(); startQueued(); }
        render();
      }],
      ['\u6dfb\u52a0\u5173\u6ce8', addFavorite], ['\u8fde\u63a5', connect], ['\u65ad\u5f00', disconnect]
    ].forEach((entry, index) => {
      const button = createButton(entry[0], GRID_X + 42 + (index % 2) * (settingsWidth + 12),
        GRID_Y + 142 + Math.floor(index / 2) * 46, settingsWidth, 36, entry[1],
        index === 5 ? pal.blueSoft : pal.card,
        { radius: 8, borderWidth: 1, borderColor: pal.line, fontSize: 14 });
      button.section = 'settings';
      out.settings.push(button);
    });

    return out;
  }

  function buildDetailUi(out) {
    const dx = GRID_X + 18, dy = HEADER_H + 18;
    const dw = GRID_W - 36, dh = H - HEADER_H - 36;
    const controlX = dx + 364, controlW = dw - 388;
    expandButtonPool();
    out.detailPanel = borderedPanel(dx, dy, dw, dh, pal.card, 10);
    out.detailCrumb = label('', dx + 24, dy + 18, 320, 20, 12, pal.muted);
    out.detailName = label('', dx + 24, dy + 48, 316, 36, 25, pal.ink);
    out.detailState = label('', dx + 24, dy + 90, 316, 28, 18, pal.muted);
    pushStyle(out.detailName, 'L', { center: 1, ellipsis: 1 });
    pushStyle(out.detailState, 'L', { center: 1, ellipsis: 1 });
    out.detailHalo = panel(dx + 106, dy + 142, 116, 116, pal.blueSoft, 58);
    out.detailGlyph = label('\uf011', dx + 106, dy + 186, 116, 28, 24, pal.blue, true);
    pushStyle(out.detailGlyph, 'L', { center: 1 });
    out.detailPower = createButton('', dx + 106, dy + 142, 116, 116,
      () => { const item = current(); if (item) toggleItem(item); }, pal.card,
      { radius: 58, borderWidth: 0 });
    out.detailPower.section = 'detail';
    out.detailHint = label('', dx + 48, dy + 276, 232, 22, 13, pal.muted);
    pushStyle(out.detailHint, 'L', { center: 1 });
    out.detailControl = borderedPanel(controlX, dy + 28, controlW, 184, pal.soft, 10);
    out.detailControlTitle = label('', controlX + 22, dy + 48, controlW - 44, 22, 14, pal.muted);
    out.detailPercent = label('', controlX + 22, dy + 76, controlW - 44, 38, 28, pal.ink);
    pushStyle(out.detailControlTitle, 'L', { center: 1 });
    pushStyle(out.detailPercent, 'L', { center: 1, ellipsis: 1 });
    out.detailTrack = panel(controlX + 22, dy + 126, controlW - 44, 8, pal.track, 4);
    out.detailFill = panel(controlX + 22, dy + 126, 2, 8, pal.yellow, 4);
    out.detailTrackWidth = controlW - 44;
    out.detailMinus = createButton('', controlX + 22, dy + 150, 58, 42,
      () => changeBrightness(-10), pal.card,
      { radius: 8, borderWidth: 0 });
    out.detailPlus = createButton('', controlX + controlW - 80, dy + 150, 58, 42,
      () => changeBrightness(10), pal.card,
      { radius: 8, borderWidth: 0 });
    out.detailMinus.section = 'detail';
    out.detailPlus.section = 'detail';
    out.detailMinusFace = borderedPanel(controlX + 22, dy + 150, 58, 42, pal.card, 8);
    out.detailPlusFace = borderedPanel(controlX + controlW - 80, dy + 150, 58, 42, pal.card, 8);
    out.detailMinusGlyph = label('\uf068', controlX + 22, dy + 157, 58, 28, 20, pal.ink, true);
    out.detailPlusGlyph = label('\uf067', controlX + controlW - 80, dy + 157,
      58, 28, 20, pal.ink, true);
    pushStyle(out.detailMinusGlyph, 'L', { center: 1 });
    pushStyle(out.detailPlusGlyph, 'L', { center: 1 });
    out.detailMetaTitle = label('\u5b9e\u4f53\u4fe1\u606f', controlX + 2, dy + 238,
      controlW - 4, 22, 14, pal.ink);
    out.detailEntity = label('', controlX + 2, dy + 270, controlW - 4, 22, 13, pal.muted);
    out.detailAvailability = label('', controlX + 2, dy + 300, controlW - 4, 22, 13, pal.muted);
    out.detailReadOnly = label('', controlX + 2, dy + 334, controlW - 4, 24, 13, pal.blue);
    out.detailWidgets = [out.detailPanel, out.detailCrumb, out.detailName, out.detailState,
      out.detailHalo, out.detailGlyph, out.detailHint, out.detailControl,
      out.detailControlTitle, out.detailPercent, out.detailTrack, out.detailFill,
      out.detailMinusFace, out.detailPlusFace, out.detailMinusGlyph, out.detailPlusGlyph,
      out.detailMetaTitle, out.detailEntity, out.detailAvailability, out.detailReadOnly];
    out.detailWidgets.forEach(id => hide(id, true));
    hide(out.detailPower, true);
    hide(out.detailMinus, true);
    hide(out.detailPlus, true);
  }

  function ensureDetailUi() {
    if (!widgets || widgets.detailWidgets || detailBuilding || disposed) return;
    detailBuilding = true;
    setTimeout(() => {
      if (disposed) return;
      buildDetailUi(widgets);
      detailBuilding = false;
      render();
    }, 20);
  }

  function renderNow() {
    if (!widgets || disposed) return;
    let selected = current();
    if (detailOpen && !selected) detailOpen = false;
    const listView = !settingsOpen && !detailOpen;
    const detailRequested = !settingsOpen && detailOpen && !!selected;
    const detailView = detailRequested && !!widgets.detailWidgets;
    const list = dashboardEntities();
    const pages = Math.max(1, Math.ceil(list.length / SLOTS));
    if (roomPage >= pages) roomPage = pages - 1;
    const sectionNames = {
      overview: '\u603b\u89c8', devices: '\u8bbe\u5907', environment: '\u73af\u5883', system: '\u7cfb\u7edf'
    };
    const nextCardKey = entityRevision + '|' + nav + '|' + roomPage + '|' +
      selectedId + '|' + (settingsOpen ? 'settings' : detailRequested ? 'detail' : 'cards');
    if (nextCardKey !== cardRenderKey) {
      cardRenderKey = nextCardKey;
      cardRenderCursor = 0;
      widgets.cards.forEach(card => {
        card.button._cardVisible = false;
        card.iconButton._cardVisible = false;
      });
    }

    hide(widgets.sidebar, false); hide(widgets.brand, false); hide(widgets.note, false);
    hide(widgets.header, false); hide(widgets.title, false);
    widgets.menu.forEach(row => {
      const active = row.key === nav;
      hide(row.panel, settingsOpen); hide(row.icon, settingsOpen); hide(row.text, settingsOpen);
      hide(row.button, settingsOpen);
      pushColor(row.panel, active ? pal.blueSoft : pal.card);
      pushColor(row.icon, active ? pal.blue : pal.muted);
      pushColor(row.text, active ? pal.blue : pal.ink);
    });
    widgets.stats.forEach((entry, index) => {
      hide(entry.dot, settingsOpen); hide(entry.text, settingsOpen);
      pushText(entry.text, index === 0 ? '\u5b9e\u4f53 ' + entities.length :
        index === 1 ? '\u5728\u7ebf ' + entities.filter(available).length :
          '\u8fd0\u884c ' + activeDevices());
    });
    pushText(widgets.title, settingsOpen ? '\u8fde\u63a5\u8bbe\u7f6e' :
      detailView ? fit(selected.name, 280, 20) : sectionNames[nav]);
    pushText(widgets.status, connected ? '\u5df2\u540c\u6b65' : '\u672a\u8fde\u63a5');
    pairHidden(widgets.back, !settingsOpen && !detailRequested);
    pairHidden(widgets.refresh, !listView);
    pairHidden(widgets.settingsButton, !listView);

    const offset = roomPage * SLOTS;
    const cardsNeedUpdate = cardRenderCursor < SLOTS;
    const cardStart = cardRenderCursor;
    const cardEnd = Math.min(SLOTS, cardStart + 1);
    widgets.cards.forEach((card, index) => {
      if (index < cardStart || index >= cardEnd) return;
      const item = listView ? list[offset + index] || null : null;
      card.item = item;
      const hidden = !item;
      [card.panel, card.icon, card.type, card.name, card.value,
        card.state, card.track, card.fill].forEach(id => hide(id, hidden));
      card.button._cardVisible = !!item;
      card.iconButton._cardVisible = !!item && controllable(item);
      hide(card.button, hidden);
      hide(card.iconButton, hidden || !controllable(item));
      if (!item) return;
      const tint = dashboardTint(item);
      const progress = progressValue(item);
      pushColor(card.panel, item.entity_id === selectedId ? pal.blueSoft : pal.card);
      pushColor(card.icon, tint);
      pushText(card.icon, dashboardIcon(item));
      pushText(card.type, fit(domainTitle(item), CARD_W - 36, 11));
      pushText(card.name, fit(item.name, CARD_W - 71, 14));
      pushText(card.value, fit(dashboardValue(item), CARD_W - 36, 26));
      pushText(card.state, fit(dashboardSubtitle(item), CARD_W - 36, 12));
      pushColor(card.value, available(item) ? pal.ink : pal.muted);
      pushColor(card.state, available(item) ? pal.muted : pal.red);
      hide(card.track, progress === null);
      hide(card.fill, progress === null);
      if (progress !== null) {
        const percent = Math.max(0, Math.min(100, progress));
        pushSize(card.fill, Math.max(2, Math.round((CARD_W - 32) * percent / 100)), 5);
        pushColor(card.fill, tint);
      }
    });
    cardRenderCursor = cardEnd;
    if (cardsNeedUpdate && cardRenderCursor < SLOTS) renderAgain = true;

    hide(widgets.footer, !listView); hide(widgets.footerLine, !listView);
    hide(widgets.selectedName, !listView); hide(widgets.selectedState, !listView);
    hide(widgets.pages, !listView);
    pushText(widgets.selectedName, selected ? fit(selected.name, 278, 15) : '\u9009\u62e9\u5361\u7247\u67e5\u770b\u8be6\u60c5');
    pushText(widgets.selectedState, selected ?
      fit(selected.entity_id + '  \u00b7  ' + dashboardSubtitle(selected), 278, 11) : message);
    pushText(widgets.pages, '\u7b2c ' + (roomPage + 1) + ' / ' + pages + '\u9875  \u00b7  ' + list.length + '\u4e2a');
    pairHidden(widgets.previous, !listView || roomPage === 0);
    pairHidden(widgets.next, !listView || roomPage + 1 >= pages);

    if (widgets.detailWidgets) {
      widgets.detailWidgets.forEach(id => hide(id, !detailView));
      const usable = detailView && connected && available(selected) && controllable(selected) &&
        !pending && !queued && !nativeBusy;
      const toggleService = selected && selected.state === 'on' ? 'turn_off' : 'turn_on';
      const canToggle = usable && supports(selected, toggleService);
      const dimmer = usable && selected.dimmable && supports(selected, 'turn_on');
      hide(widgets.detailPower, !canToggle);
      hide(widgets.detailMinus, !dimmer);
      hide(widgets.detailPlus, !dimmer);
      hide(widgets.detailMinusFace, !detailView || !dimmer);
      hide(widgets.detailPlusFace, !detailView || !dimmer);
      hide(widgets.detailMinusGlyph, !detailView || !dimmer);
      hide(widgets.detailPlusGlyph, !detailView || !dimmer);
      if (detailView) {
      const tint = dashboardTint(selected);
      const rawProgress = selected.dimmable ? (selected.brightness === null ? 0 : selected.brightness) :
        progressValue(selected);
      const hasProgress = rawProgress !== null;
      const percent = hasProgress ? Math.max(0, Math.min(100, Number(rawProgress))) : 0;
      pushText(widgets.detailCrumb, sectionNames[nav] + '  /  ' + domainTitle(selected));
      pushText(widgets.detailName, fit(selected.name, 316, 25));
      pushText(widgets.detailState, fit(dashboardSubtitle(selected), 316, 18));
      pushText(widgets.detailGlyph, '\uf011');
      pushText(widgets.detailMinusGlyph, '\uf068');
      pushText(widgets.detailPlusGlyph, '\uf067');
      pushColor(widgets.detailGlyph, selected.state === 'on' ?
        (selected.domain === 'light' ? pal.yellow : pal.blue) : pal.ink);
      pushColor(widgets.detailHalo, selected.state === 'on' ?
        (selected.domain === 'light' ? pal.yellowSoft : pal.blueSoft) : pal.soft);
      pushText(widgets.detailHint, canToggle ? '\u70b9\u51fb\u7535\u6e90\u56fe\u6807\u5207\u6362\u72b6\u6001' :
        '\u5f53\u524d\u5b9e\u4f53\u4e3a\u53ea\u8bfb\u72b6\u6001');
      pushText(widgets.detailControlTitle, selected.dimmable ? '\u4eae\u5ea6' : '\u5f53\u524d\u72b6\u6001');
      pushText(widgets.detailPercent, selected.dimmable ? Math.round(percent) + '%' :
        fit(dashboardValue(selected), widgets.detailTrackWidth, 28));
      hide(widgets.detailTrack, !hasProgress);
      hide(widgets.detailFill, !hasProgress || percent <= 0);
      if (hasProgress) {
        pushSize(widgets.detailFill,
          Math.max(2, Math.round(widgets.detailTrackWidth * percent / 100)), 8);
        pushColor(widgets.detailFill, tint);
      }
      pushText(widgets.detailEntity, 'Entity ID  ' + selected.entity_id);
      pushText(widgets.detailAvailability, '\u72b6\u6001  ' +
        (available(selected) ? '\u53ef\u7528  \u00b7  ' + friendlyState(selected) : '\u4e0d\u53ef\u7528'));
      pushText(widgets.detailReadOnly, dimmer ? '\u4f7f\u7528 - / + \u8c03\u6574\u4eae\u5ea6' :
        canToggle ? '\u7535\u6e90\u63a7\u5236\u5df2\u53ef\u7528' : '\u6b64\u5b9e\u4f53\u4e0d\u4f1a\u53d1\u9001\u63a7\u5236\u8bf7\u6c42');
      }
    }

    hide(widgets.settingsPanel, !settingsOpen);
    [widgets.url, widgets.secret, widgets.transport].forEach(id => hide(id, !settingsOpen));
    widgets.settings.forEach(button => hide(button, !settingsOpen));
    if (settingsOpen) {
      pushText(widgets.url, '\u5bb6\u5ead\u4e2d\u67a2  \u00b7  ' + url);
      pushText(widgets.secret, nativeManaged ? '\u4ee4\u724c\uff1a\u7531\u672c\u5730\u670d\u52a1\u7ba1\u7406' :
        '\u4ee4\u724c\uff1a\u672a\u8f93\u5165');
      pushText(widgets.transport, nativeManaged ? '\u5df2\u4f7f\u7528\u5df2\u9a8c\u8bc1\u7684\u672c\u5730 Home Assistant \u670d\u52a1' :
        '\u8fde\u63a5\u524d\u9700\u786e\u8ba4 HTTP \u98ce\u9669');
      buttonText(widgets.settings[2], httpAllowed ? 'HTTP \u5df2\u5141\u8bb8' : '\u5141\u8bb8 HTTP');
      buttonText(widgets.settings[3], scope === 'favorites' ? '\u4ec5\u5173\u6ce8' : '\u5168\u90e8\u5b9e\u4f53');
      buttonText(widgets.settings[5], connected ? '\u91cd\u65b0\u540c\u6b65' : '\u8fde\u63a5');
    }
    renderButtons();
  }

  function continueRender() {
    renderTimer = 0;
    if (disposed) { renderRunning = false; return; }
    renderAgain = false;
    renderNow();
    if (renderAgain) {
      system.yield().then(continueRender, () => { renderRunning = false; });
    } else {
      renderRunning = false;
    }
  }

  function render() {
    if (!widgets || disposed) return;
    renderAgain = true;
    if (renderRunning) return;
    renderRunning = true;
    renderTimer = setTimeout(continueRender, 20);
  }

  function openDetail(item) {
    selectedId = item.entity_id;
    settingsOpen = false;
    detailOpen = true;
    ensureDetailUi();
    render();
  }

  function toggleItem(item) {
    selectedId = item.entity_id;
    if (!controllable(item) || !connected || !available(item) ||
        pending || queued || nativeBusy) {
      render();
      return;
    }
    const service = item.state === 'on' ? 'turn_off' : 'turn_on';
    if (!supports(item, service)) { render(); return; }
    message = '正在发送控制请求';
    schedule('service', item.entity_id,
      { domain: item.domain, service, body: { entity_id: item.entity_id } });
    startQueued();
  }

  function snapshot() {
    return {
      phase, message, url, connected, hasToken: !!token, httpAllowed, scope,
      backend: sharedReady ? 'native-service' :
        nativeService ? 'unsupported-service' : 'legacy',
      nativeManaged,
      favorites: favorites.slice(), filter, search, page, nav, roomPage, layoutStyle,
      settingsOpen, detailOpen, selectedId, limited, serviceDiscoveryLimited, lastSync, lastCached,
      nextRefresh, nativeBusy, disposed,
      view: settingsOpen ? 'settings' : detailOpen ? 'detail' : 'list',
      pending: pending ? { kind: pending.kind, entityId: pending.entityId,
        abandoned: !!pending.abandoned } : null,
      entities: entities.map(row => Object.assign({}, row))
    };
  }

  if (W < 640 || H < 400) {
    ui.background(pal.page);
    label('米家 HA', 12, 12, Math.max(1, W - 24), 28, 20, pal.ink);
    label('显示区域至少需要 640 × 400', 12, 56, Math.max(1, W - 24), 64, 16, pal.muted);
    return;
  }
  widgets = buildUiReference();
  if (!bridgeReady) message = '固件未提供 Home Assistant 接口';
  renderNow();
  if (nativeManaged && bridgeReady) connect();
  ui.onSwipe(direction => {
    if (disposed || settingsOpen || detailOpen) return;
    const pages = pageCount();
    if (direction === 'left' && roomPage + 1 < pages) roomPage++;
    else if (direction === 'right' && roomPage > 0) roomPage--;
    render();
  });
  timer = setInterval(tick, 250);
  globalThis.ESPHomeHA = {
    refresh, disconnect,
    settings: () => { detailOpen = false; settingsOpen = true; render(); },
    snapshot,
    dispose: () => {
      disconnect();
      disposed = true;
      buttonPool.forEach(slot => {
        slot.target = null;
        ui.setHidden(slot.id, true);
      });
      clearInterval(timer);
      if (renderTimer) clearTimeout(renderTimer);
      if (bridge && typeof bridge.stop === 'function') bridge.stop();
    }
  };
})();
