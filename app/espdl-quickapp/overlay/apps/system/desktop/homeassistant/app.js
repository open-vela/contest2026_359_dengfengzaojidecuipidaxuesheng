/* SPDX-License-Identifier: Apache-2.0 */
'use strict';

(function () {
  const tr = value => typeof system.translate === 'function' ? system.translate(value) : value;

  const nativeService = system.homeAssistantService;
  const sharedReady = nativeService && nativeService.apiVersion === 1 &&
    ['configure', 'get', 'getState', 'control', 'poll', 'close', 'status']
      .every(name => typeof nativeService[name] === 'function');
  const bridge = nativeService ? (sharedReady ? sharedAdapter() : {}) : system.homeAssistant;
  const storage = system.storage;
  const bridgeReady = bridge && ['get', 'getState', 'callService', 'poll']
    .every(name => typeof bridge[name] === 'function');
  const MAX_ENTITIES = 512;
  const MAX_SCENES = 128;
  const MAX_CARD_PREFS = 128;
  const MAX_FAVORITES = 16;
  const RESPONSE_LIMIT = 256 * 1024;
  // Elapsed-time deadlines must survive NTP or user wall-clock changes.
  const steadyNow = () => typeof ui.nowMs === 'function' ? ui.nowMs() : Date.now();
  const REQUEST_TIMEOUT = 12000;
  const REFRESH_INTERVAL = 10000;
  const VERIFY_INTERVAL = 1000;
  const VERIFY_WINDOW = 8000;
  const VERIFY_ATTEMPTS = 4;
  const INITIAL_BUTTON_SLOTS = 32;
  const BUTTON_SLOTS = 64;
  const buttons = [];
  const buttonPool = [];
  const size = ui.getSize();
  const W = size.width, H = size.height;
  const dark = ui.primary > 0x888888;
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
  if(ui.theme)Object.assign(colors,{background:ui.theme.background,surface:ui.theme.card,
    detail:ui.theme.surface,text:ui.theme.primary,muted:ui.theme.secondary,border:ui.theme.border,
    track:ui.theme.surface,good:ui.theme.success,warning:ui.theme.warning});
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
  let sceneEntities=[],sceneSyncReady=false,sceneTotal=0,sceneSyncError='',sceneSyncAt=0;
  let lastSceneAcceptance=null;
  let nav = 'overview', roomPage = 0, layoutStyle = 'rooms';
  let services = Object.create(null);
  let serviceDiscoveryLimited = false;
  let selectedId = '';
  let filter = 'all';
  let search = '';
  let selectedArea='',favoritesOnly=false,discoveryRevision=0,browseOpen=false,browsePage=0,browseUi=null;
  let detailExpanded=false;
  let menuDrag=null,menuClickUntil=0;
  let page = 0;
  let settingsOpen = false;
  let detailOpen = false;
  let functionPage=0,controlPage=0,detailRenderKey='',controlGrant=null;
  let optionPicker=null,optionPickerUi=null;
  let lampWheelDrag=null;
  let lampAuxEntity='';
  let quickDrag = null, quickGestureOwned = false;
  let brightnessIntent = null;
  let controlQueue=[],controlTargets=Object.create(null),controlSequence=0,controlRevision=0;
  const controlFeedback=Object.create(null);
  function markControlFeedback(entity,kind,text){
    if(!entity)return;
    delete controlFeedback[entity];
    controlFeedback[entity]={kind,text,expires:steadyNow()+(kind==='error'?5000:1800)};
    const keys=Object.keys(controlFeedback);while(keys.length>32)delete controlFeedback[keys.shift()];
    controlRevision++;
  }
  let usageServer='',usageCounts=Object.create(null),homeWeights=Object.create(null),homeOrderRevision=0;
  let usageDirty=false,usageSaveAt=Infinity;
  let desktopPins=[],desktopServer='',desktopPublished='',desktopInputKey='',desktopRevision=0,pendingDesktop=null;
  let entityDevices=Object.create(null),deviceRegistryReady=false,deviceRegistryAt=0;
  let deviceCardsKey=-1,deviceCards=[];
  let dashboardStatsKey='',dashboardStatsValue=null;
  const activityLog=[];
  let logSequence=0;
  function appendActivity(name,detail){
    const date=new Date(),time=[date.getHours(),date.getMinutes(),date.getSeconds()].map(value=>String(value).padStart(2,'0')).join(':');
    activityLog.unshift({entity_id:'local.log_'+(++logSequence),name:clean(name,40),detail:clean(detail,120),time});
    if(activityLog.length>32)activityLog.length=32;
  }
  let layoutEditing = false, sizeRevision = 0, sizesUrl = '';
  let cardSizePrefs = Object.create(null), boardCacheKey = '', boardCache = [],boardCacheNav='',boardCacheGeneration=-1;
  let quickGroupBuilding = false, quickGroupTarget = 0, layoutSaveError = false;
  let connected = false;
  let phase = 'disconnected';
  let message = tr('未连接');
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
  let widgets, uiReady = false;
  let nativeManaged = false;
  let observedNativeEndpoint = '', observedNativeRevision;
  let entityRevision = 0;
  let dashboardCacheKey = '';
  let dashboardCache = [];
  let cardRenderKey = '';
  let cardRenderCursor = 0,cardRenderOrder=[];
  let renderRunning = false;
  let renderAgain = false;
  let paintDeferred = false, creationDeferred = false;
  function canPaint() { return !ui.isVisible || ui.isVisible(); }
  let windowRows=null,windowColumns=[];
  let detailBuilding = false;

  function sharedAdapter() {
    let requestId = 0, detached = false, submitError = 0;
    function prepare() {
      detached = false;
      if (token) {
        if (nativeService.configure(url, token, httpAllowed) !== 0) return false;
        token = '';
        httpAllowed = false;
        nativeManaged = true;
      }
      const status = nativeService.status();
      observeNativeConfiguration(status);
      nativeManaged = nativeEndpoint(status) === url;
      return nativeManaged;
    }
    function submit(action) {
      submitError=0;
      if (!prepare()) return false;
      const id = action();
      if (!Number.isInteger(id) || id <= 0) {submitError=Number.isInteger(id)?id:-22;return false;}
      requestId = id;
      return true;
    }
    function close() {
      detached = true;
      requestId = 0;
      nativeService.close();
    }
    return {
      submitError:()=>submitError,
      get: (_url, _token, resource) => submit(() => nativeService.get(resource)),
      getState: (_url, _token, entity) => submit(() => nativeService.getState(entity)),
      callService: (_url, _token, domain, service, data) => {
        const body = JSON.parse(data);
        if (!validEntity(body.entity_id) || body.entity_id.split('.')[0] !== domain ||
            !service || typeof service!=='string') return false;
        if(typeof nativeService.callService==='function')
          return submit(()=>nativeService.callService(body.entity_id,service,data));
        if(!['turn_on','turn_off'].includes(service)||Object.keys(body).some(key=>!['entity_id','brightness_pct'].includes(key)))return false;
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

  function observeNativeConfiguration(status) {
    observedNativeEndpoint = nativeEndpoint(status);
    observedNativeRevision = status && status.configurationRevision;
  }

  function reloadNativeConfiguration() {
    if (!sharedReady) return false;
    const status = nativeService.status();
    const endpoint = nativeEndpoint(status);
    if (endpoint === observedNativeEndpoint &&
        status.configurationRevision === observedNativeRevision) return false;
    observeNativeConfiguration(status);
    // Portal saves replace the shared connection, including requests in flight.
    // Drop the old UI generation before reading from the new home.
    invalidate();
    bridge.close();
    pending = null;
    nativeBusy = false;
    token = '';
    httpAllowed = false;
    nativeManaged = !!endpoint;
    entities = [];
    entityRevision++;
    selectedId = '';
    detailOpen = false;
    roomPage = page = 0;
    settingsOpen = !endpoint;
    if (endpoint) {
      url = endpoint;
      connect();
    } else {
      message = tr('家庭连接配置已清除');
      render();
    }
    return true;
  }

  function clean(value, limit) {
    if(typeof value!=='string')return '';
    const text=value.replace(/[\u0000-\u001f\u007f]/g,' ');
    return text.length<=limit?text:Array.from(text).slice(0,limit).join('');
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
      message = tr('本地保存失败，本次会话仍可使用');
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
    message = tr('本地设置无法读取');
  }

  if (sharedReady) {
    try {
      const status = nativeService.status();
      observeNativeConfiguration(status);
      const endpoint = nativeEndpoint(status);
      if (endpoint) {
        url = endpoint;
        nativeManaged = true;
        message = tr('本地 HA 服务已配置，尚未同步');
      }
    } catch (_) { message = tr('本地 HA 服务暂不可用'); }
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
    let changed=false;
    entities.forEach(item => {if(!item.stale){item.stale=true;changed=true;}});
    sceneEntities.forEach(item=>{if(!item.stale){item.stale=true;changed=true;}});
    if(changed)entityRevision++;
  }

  function invalidate() {
    cancelInputs();
    search='';selectedArea='';favoritesOnly=false;browseOpen=false;discoveryRevision++;
    Object.keys(controlFeedback).forEach(id=>delete controlFeedback[id]);
    sceneEntities=[];sceneSyncReady=false;sceneTotal=0;sceneSyncError='';sceneSyncAt=0;
    lastSceneAcceptance=null;
    entityRevision++;
    optionPicker=null;
    quickDrag=null;
    brightnessIntent=null;
    controlQueue=[];controlTargets=Object.create(null);controlRevision++;
    entityDevices=Object.create(null);deviceRegistryReady=false;deviceRegistryAt=0;
    deviceCardsKey=-1;
    generation++;
    queued = null;
    if (pending) pending.abandoned = true;
    connected = false;
    autoRefresh = false;
    nextRefresh = Infinity;
    lastCached = false;
    pendingDesktop=null;
    phase = 'disconnected';
    services = Object.create(null);
    serviceDiscoveryLimited = false;
    staleAll();
  }

  function disconnect() {
    invalidate();
    token = '';
    httpAllowed = false;
    message = tr('已断开，令牌已从会话移除');
    if (sharedReady) {
      try { bridge.close(); } catch (_) { /* UI still invalidates its session. */ }
      nativeManaged = false;
      message = tr('界面已断开，本地服务配置保留');
    }
    render();
  }

  function credentialsReady() {
    if (!bridgeReady) message = tr('固件未提供 Home Assistant 接口');
    else if (sharedConnection()) return true;
    else if (!parseUrl(url)) message = tr('需要局域网 HTTP 地址');
    else if (!token) message = tr('尚未输入访问令牌');
    else if (!httpAllowed) message = tr('HTTP 未加密，尚未授权连接');
    else return true;
    render();
    return false;
  }

  function schedule(kind, entityId, data) {
    queued = { kind, entityId: entityId || '', data, generation };
  }

  function startQueued() {
    if (!queued || pending || nativeBusy || disposed) return;
    if (queued.notBefore && steadyNow() < queued.notBefore) return;
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
      if(bridge.submitError&&bridge.submitError()===-16){
        // The native API explicitly declined admission, so this request has
        // not been sent. Keep it until the shared worker becomes available.
        queued=request;nativeBusy=true;message=tr('等待接口空闲');render();return;
      }
      fail(request, tr('接口忙或参数被拒绝'), false);
      return;
    }
    if(request.kind==='service'){
      const target=controlTargets[request.entityId];
      if(target&&target.sequence===request.data.sequence)target.sent=true;
      if(brightnessIntent&&brightnessIntent.sequence===request.data.sequence)brightnessIntent.sent=true;
      controlRevision++;
    }
    pending = request;
    pending.started = steadyNow();
    nativeBusy = true;
    render();
  }

  function connect() {
    if (!credentialsReady()) return;
    if (pending || nativeBusy || queued) {
      message = tr('仍有请求未结束');
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
    message = tr('正在连接 Home Assistant');
    schedule('config');
    startQueued();
  }

  function refresh() {
    if (disposed || pending || nativeBusy || queued) return;
    if (!connected) { connect(); return; }
    if (!credentialsReady()) return;
    if(nav==='scenes'&&supportsSceneSync()){
      sceneSyncError='';message=tr('正在同步 HA 场景');schedule('scenes');startQueued();render();return;
    }
    message = tr('正在同步状态');
    beginStates();
    startQueued();
  }

  function beginStates() {
    let groupingSupported=false;
    try{groupingSupported=sharedReady&&nativeService.status().deviceGroups===true;}catch(_){}
    if(groupingSupported&&(!deviceRegistryReady||steadyNow()-deviceRegistryAt>=300000)) {
      schedule('devices');return;
    }
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

  function strings(value){return Array.isArray(value)?value.filter(v=>typeof v==='string'&&v.length>0&&v.length<=128).slice(0,64):[];}

  function supportsSceneSync(){
    try{return sharedReady&&nativeService.status().sceneSync===true;}catch(_){return false;}
  }

  function isSceneCommand(item){return !!item&&['scene','script'].includes(item.domain);}
  function sceneStatusText(count){
    if(sceneSyncError)return tr('同步失败 · 点刷新重试');
    if(lastSceneAcceptance&&steadyNow()-lastSceneAcceptance.at<5000)return tr('HA 已接受：')+fit(lastSceneAcceptance.name,180,11);
    if(pending&&pending.kind==='scenes')return tr('正在同步 HA 场景');
    if(sceneSyncReady)return sceneTotal>MAX_SCENES?tr('显示前 128 / ')+sceneTotal+tr(' 个'):count?tr('已同步 ')+count+tr(' 个 · HA 联动'):tr('HA 中暂无场景或脚本');
    return supportsSceneSync()?tr('等待同步 HA 场景'):count?tr('功能按原生服务能力展示'):tr('暂无已同步的场景或脚本');
  }
  // Rebuild at most once per committed revision; controls always resolve
  // the newest entity rather than retaining a row captured by an old card.
  let entityIndexRevision=-1,entityIndexRows=null,sceneIndexRows=null;
  let entityIndex=Object.create(null),sceneIndex=Object.create(null);
  function indexedEntity(id,scene=false){
    if(entityIndexRevision!==entityRevision||entityIndexRows!==entities||sceneIndexRows!==sceneEntities){
      entityIndex=Object.create(null);sceneIndex=Object.create(null);
      entities.forEach(row=>{if(!entityIndex[row.entity_id])entityIndex[row.entity_id]=row;});
      sceneEntities.forEach(row=>{if(!sceneIndex[row.entity_id])sceneIndex[row.entity_id]=row;});
      entityIndexRows=entities;sceneIndexRows=sceneEntities;entityIndexRevision=entityRevision;
    }
    return (scene?sceneIndex:entityIndex)[id]||null;
  }
  function lookupEntity(id){return indexedEntity(id,sceneSyncReady&&/^(scene|script)\./.test(id));}
  function sceneTimestamp(item){
    const stamp=item.domain==='scene'?item.state:item.last_triggered;
    if(!stamp||!/^\d{4}-\d\d-\d\dT\d\d:\d\d/.test(stamp)||!Number.isFinite(Date.parse(stamp)))return tr('暂无触发记录');
    const zone=stamp.endsWith('Z')||stamp.endsWith('+00:00')?' UTC':/[+-]\d\d:\d\d$/.test(stamp)?' '+stamp.slice(-6):'';
    return tr('上次触发 ')+stamp.slice(5,10)+' '+stamp.slice(11,16)+zone;
  }
  function activateScene(item){
    item=lookupEntity(item.entity_id)||item;
    if(!isSceneCommand(item)||controlTargets[item.entity_id])return;
    requestControl(item,'turn_on');
  }

  function commitScenes(value){
    if(!value||!Array.isArray(value.items)||!Number.isInteger(value.total)||value.total<value.items.length)throw Error('invalid scenes');
    const next=[],seen=new Set();
    value.items.slice(0,MAX_SCENES).forEach(raw=>{
      const item=normalize(raw);
      if(!item||!['scene','script'].includes(item.domain)||seen.has(item.entity_id))return;
      seen.add(item.entity_id);next.push(item);
    });
    const changed=!sameState(sceneEntities,next);
    if(changed){sceneEntities=next;entityRevision++;}
    sceneSyncReady=true;sceneTotal=value.total;sceneSyncError='';sceneSyncAt=Date.now();
    phase='ready';failures=0;nextRefresh=steadyNow()+REFRESH_INTERVAL;
    message=sceneTotal>MAX_SCENES?tr('已同步前 128 个场景与脚本，共 ')+sceneTotal+tr(' 个'):tr('已同步 ')+next.length+tr(' 个 HA 场景与脚本');
    if(changed)appendActivity(tr('场景同步'),next.length+tr(' 个场景与脚本'));
    if(nav==='scenes'&&selectedId&&!next.some(item=>item.entity_id===selectedId)){selectedId='';detailOpen=false;}
  }

  function brightnessPercent(value) {
    if(typeof value!=='number'||!Number.isFinite(value)||value<0||value>255)return null;
    return value>0?Math.max(1,Math.round(value*100/255)):0;
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
    const brightness = brightnessPercent(attributes.brightness);
    function numeric(name, minimum, maximum) {
      const value = attributes[name];
      if (typeof value !== 'number' || !Number.isFinite(value) ||
          value < minimum || value > maximum) return null;
      return value;
    }
    return {
      entity_id: raw.entity_id,
      device_id: clean(attributes.device_id,64),
      device_name: clean(attributes.device_name,64),
      domain: raw.entity_id.split('.')[0],
      name: clean(attributes.friendly_name, 64) || raw.entity_id,
      state: clean(raw.state, 128),
      last_triggered: clean(attributes.last_triggered,128),
      unit: clean(attributes.unit_of_measurement, 16),
      device_class: clean(attributes.device_class, 32),
      icon: clean(attributes.icon, 48),
      card_type: clean(attributes.miot_type || attributes.device_type, 120),
      quick_values: quickPropertyValues(raw, attributes),
      members: Array.isArray(attributes.entity_id) ? attributes.entity_id.filter(validEntity).slice(0,4) : [],
      brightness,
      color_modes:modes.filter(mode=>typeof mode==='string').slice(0,10),
      color_mode:clean(attributes.color_mode,24),
      color_capable:modes.some(mode=>['hs','xy','rgb','rgbw','rgbww'].includes(mode)) ||
        (!modes.length&&(supportedFeatures&16)!==0),
      rgb_color:validRgb(attributes.rgb_color)?attributes.rgb_color.slice():null,
      color_temp_kelvin: numeric('color_temp_kelvin',1000,20000) ||
        (numeric('color_temp',50,1000)!==null?Math.round(1000000/attributes.color_temp):null),
      color_temperature: modes.includes('color_temp') || (supportedFeatures&2)!==0 || numeric('color_temp_kelvin',1000,20000)!==null,
      min_color_temp_kelvin: numeric('min_color_temp_kelvin',1000,20000) ||
        (numeric('max_mireds',50,1000)!==null?Math.ceil(1000000/attributes.max_mireds):2000),
      max_color_temp_kelvin: numeric('max_color_temp_kelvin',1000,20000) ||
        (numeric('min_mireds',50,1000)!==null?Math.floor(1000000/attributes.min_mireds):6500),
      minimum: numeric('min',-1e9,1e9), maximum: numeric('max',-1e9,1e9), step: numeric('step',0.000001,1e9),
      min_temp: numeric('min_temp',-100,200), max_temp: numeric('max_temp',-100,200),
      target_temp_step: numeric('target_temp_step',0.01,100),
      min_humidity: numeric('min_humidity',0,100), max_humidity: numeric('max_humidity',0,100),
      humidity: numeric('humidity',0,100), supported_features:supportedFeatures,
      has_supported_features:typeof attributes.supported_features==='number'&&Number.isFinite(attributes.supported_features),
      options: strings(attributes.options), effects: strings(attributes.effect_list), effect:clean(attributes.effect,128),
      hvac_modes:strings(attributes.hvac_modes),fan_modes:strings(attributes.fan_modes),fan_mode:clean(attributes.fan_mode,128),
      direction:clean(attributes.direction,128),
      swing_horizontal_modes:strings(attributes.swing_horizontal_modes),swing_horizontal_mode:clean(attributes.swing_horizontal_mode,128),
      target_temp_low:numeric('target_temp_low',-100,200),target_temp_high:numeric('target_temp_high',-100,200),
      away_mode:attributes.away_mode===true,
      preset_modes:strings(attributes.preset_modes),swing_modes:strings(attributes.swing_modes),swing_mode:clean(attributes.swing_mode,128),
      modes:strings(attributes.available_modes),mode:clean(attributes.mode,128),
      sources:strings(attributes.source_list),source:clean(attributes.source,128),
      fan_speeds:strings(attributes.fan_speed_list),fan_speed:clean(attributes.fan_speed,128),
      operation_modes:strings(attributes.operation_list),operation_mode:clean(attributes.operation_mode,128),
      is_volume_muted:attributes.is_volume_muted===true,
      percentage: numeric('percentage', 0, 100),
      percentage_step:numeric('percentage_step',0.000001,100),
      preset_mode: clean(attributes.preset_mode, 128),
      quick_modes: (Array.isArray(attributes.preset_modes)?attributes.preset_modes:Array.isArray(attributes.hvac_modes)?attributes.hvac_modes:[]).filter(v=>typeof v==='string').slice(0,3).map(v=>clean(v,24)),
      oscillating: attributes.oscillating === true,
      current_humidity: numeric('current_humidity', 0, 100),
      pm25: numeric('pm25', 0, 5000),
      current_position: numeric('current_position', 0, 100),
      current_tilt_position:numeric('current_tilt_position',0,100),
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

  function sameState(a,b) {
    if(a===b)return true;
    if(!a||!b||typeof a!=='object'||typeof b!=='object')return false;
    const keys=Object.keys(b);
    if(keys.length!==Object.keys(a).length)return false;
    for(const key of keys)if(!sameState(a[key],b[key]))return false;
    return true;
  }

  function commitStates(values, wasLimited, fromCache) {
    const wasConnected=connected;
    const previous=Object.create(null);entities.forEach(item=>previous[item.entity_id]=item);
    let changed=entities.length!==values.length;
    values.forEach((item,index)=>{
      const old=previous[item.entity_id];
      if(old&&sameState(old,item))values[index]=old;
      else changed=true;
      if(!entities[index]||entities[index].entity_id!==item.entity_id)changed=true;
    });
    entities = values;
    if(changed)entityRevision++;
    limited = wasLimited;
    lastCached = !!fromCache;
    if (!entities.some(item => item.entity_id === selectedId)&&!sceneEntities.some(item=>item.entity_id===selectedId)) selectedId = '';
    connected = true;
    phase = 'ready';
    failures = 0;
    lastSync = Date.now();
    nextRefresh = steadyNow() + REFRESH_INTERVAL;
    message = limited ? tr('部分设备未同步，可切换关注同步') :
      tr('已同步 ') + groupedDeviceCards().length + tr(' 台设备');
    if (serviceDiscoveryLimited) message = tr('只读 · 服务目录超过 256 KiB') +
      (lastCached ? tr(' · 本地缓存') : '');
    if(changed||!wasConnected)appendActivity(tr('状态同步'),entities.length+tr(' 个实体 · ')+groupedDeviceCards().length+tr(' 台设备')+(lastCached?tr(' · 本地缓存'):'')+(limited?tr(' · 已达实体上限'):'')+(changed?tr(' · 数据已更新'):tr(' · 连接已恢复')));
    ensureDesktopPins();
    if(scope==='all'&&!limited&&deviceRegistryReady){
      const keys=groupedDeviceCards().map(deviceCardKey),pins=desktopPins.filter(key=>keys.includes(key));
      if(pins.length!==desktopPins.length){desktopPins=pins;desktopRevision++;
        save('eh_desktop_pins',JSON.stringify({server:url,pins:desktopPins}));}
    }
    publishDesktop();resolveDesktopDevice();
    if(nav==='scenes'&&supportsSceneSync())schedule('scenes');
    /* tick() renders once after complete(); a second render here can push a
     * large state synchronization past the callback deadline. */
  }

  function failureMessage(result, request) {
    if (result.error === -75 || result.error === -7) {
      if (request.kind === 'states') return tr('响应超过 256 KiB，请切换关注同步');
      if (request.kind === 'service' || request.kind === 'verify')
        return tr('控制结果未确认，请刷新；不会重试');
      return tr('该响应超过 256 KiB，当前固件无法读取');
    }
    if (result.error) return request.kind === 'service' ?
      tr('控制结果未确认，请刷新；不会重试') : tr('网络连接失败，状态已过期');
    if (result.status === 401 || result.status === 403) return tr('认证失败，请重新输入令牌');
    return request.kind === 'service' ? tr('控制被拒绝，HTTP ') + result.status :
      tr('服务返回 HTTP ') + result.status;
  }

  function fail(request, text, auth) {
    if(request.kind==='service'||request.kind==='verify')markControlFeedback(request.entityId,'error',tr('结果未确认'));
    if(request.kind==='scenes')sceneSyncError=text;
    if(request.kind==='scenes'&&!auth){
      sceneSyncError=text;sceneEntities.forEach(item=>item.stale=true);entityRevision++;
      message=text;appendActivity(tr('场景同步失败'),text);nextRefresh=steadyNow()+30000;render();return;
    }
    cancelInputs();
    brightnessIntent=null;
    controlQueue=[];controlTargets=Object.create(null);controlRevision++;
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
    appendActivity(uncertain?tr('操作失败'):tr('同步失败'),text);
    nextRefresh = auth || uncertain ? Infinity :
      steadyNow() + Math.min(60000, REFRESH_INTERVAL * Math.pow(2, Math.min(failures - 1, 3)));
    if (request.kind === 'verify') message = tr('控制结果未确认，请刷新；不会重试');
    render();
  }

  function complete(request, result) {
    if (request.generation !== generation || request.abandoned || disposed) return;
    if(request.kind==='devices'&&(result.error||result.status<200||result.status>=300)) {
      if(result.status===401||result.status===403){fail(request,tr('设备关联读取未授权'),true);return;}
      deviceRegistryReady=true;deviceRegistryAt=steadyNow();beginStates();return;
    }
    if (request.kind === 'services' && (result.error === -75 || result.error === -7 ||
        (!result.error && result.status >= 200 && result.status < 300 &&
         typeof result.body === 'string' && result.body.length > RESPONSE_LIMIT))) {
      services = Object.create(null);
      serviceDiscoveryLimited = true;
      message = tr('服务目录超限，正在只读同步');
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
      if(['scene','script','button','input_button'].includes(request.data.domain)||['media_next_track','media_previous_track','stop_cover_tilt'].includes(request.data.service)){
        markControlFeedback(request.entityId,'accepted',tr('已受理'));
        const item=lookupEntity(request.entityId),intent=controlTargets[request.entityId];
        if(intent&&intent.sequence===request.data.sequence){delete controlTargets[request.entityId];controlRevision++;}
        phase='ready';message=tr('HA 已接受执行：')+(item?item.name:request.entityId);
        lastSceneAcceptance={name:item?item.name:request.entityId,at:steadyNow()};
        appendActivity(tr('HA 已接受场景'),item?item.name:request.entityId);
        failures=0;nextRefresh=0;
        if(nav==='scenes'&&supportsSceneSync())schedule('scenes');
        return;
      }
      message = tr('控制已受理，正在核对状态');
      phase = 'verifying';
      nextRefresh = Infinity;
      schedule('verify', request.entityId, {
        state: undefined,
        service:request.data.service,
        expected:request.data.expected,
        brightness: request.data.body.brightness_pct,
        sequence: request.data.sequence,
        attempt: 1,
        deadline: steadyNow() + VERIFY_WINDOW
      });
      return;
    }
    try {
      if (typeof result.body !== 'string' || result.body.length > RESPONSE_LIMIT)
        throw new Error('response limit');
      const value = JSON.parse(result.body);
      if(request.kind==='scenes') {
        commitScenes(value);
      } else if(request.kind==='devices') {
        if(!Array.isArray(value))throw new Error('invalid devices');
        const next=Object.create(null);let count=0;
        value.slice(0,MAX_ENTITIES).forEach(row=>{
          if(!Array.isArray(row)||row.length<4||typeof row[0]!=='string'||
             !/^[a-zA-Z0-9_-]{1,64}$/.test(row[0])||!Array.isArray(row[3]))return;
          const meta={id:row[0],name:clean(row[1],64),area:clean(row[2],48),
            model:clean(row[4],120),manufacturer:clean(row[5],64)};
          row[3].forEach(id=>{if(count>=MAX_ENTITIES*2||!validEntity(id)||next[id])return;
            next[id]=meta;count++;});
        });
        if(!sameState(entityDevices,next)){entityDevices=next;entityRevision++;deviceCardsKey=-1;}
        deviceRegistryReady=true;deviceRegistryAt=steadyNow();beginStates();
      } else if (request.kind === 'config') {
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
          if (index >= 0 && !sameState(entities[index],item)) {
            entities[index] = item;
            entityRevision++;
          }
          const target = request.data;
          const matched = (target.state===undefined || item.state===target.state) && serviceReached(item,target.service) && matchesExpected(item,target.expected) &&
            (target.brightness === undefined || (item.brightness !== null &&
             Math.abs(item.brightness - target.brightness) <= 1));
          if (!matched) {
            if (target.attempt >= VERIFY_ATTEMPTS || steadyNow() >= target.deadline) {
              fail(request, tr('控制结果未确认，请刷新；不会重试'), false);
              return;
            }
            message = tr('等待设备达到目标状态');
            schedule('verify', request.entityId, {
              state: target.state, service:target.service, brightness: target.brightness, expected:target.expected,
              sequence: target.sequence,
              attempt: target.attempt + 1, deadline: target.deadline
            });
            queued.notBefore = steadyNow() + VERIFY_INTERVAL;
            render();
            return;
          }
          connected = true;
          phase = 'ready';
          message = tr('状态已达到目标：') + item.name;
          markControlFeedback(item.entity_id,'success',tr('已确认'));
          appendActivity(tr('操作已确认'),item.name);
          const intent=controlTargets[item.entity_id];
          if(intent&&intent.sequence===target.sequence){delete controlTargets[item.entity_id];controlRevision++;}
          if(brightnessIntent&&brightnessIntent.entity===item.entity_id&&brightnessIntent.sequence===target.sequence)brightnessIntent=null;
          failures = 0;
          nextRefresh = steadyNow() + REFRESH_INTERVAL;
          render();
        }
      }
    } catch (_) {
      if(request.kind==='devices'){deviceRegistryReady=true;deviceRegistryAt=steadyNow();beginStates();return;}
      fail(request, tr('响应格式错误，未应用数据'), false);
    }
  }

  function tick() {
    if (disposed) return;
    if (canPaint()) {
      if (creationDeferred) resumeUiCreation();
      if (paintDeferred) render();
    }
    if (disposed || !bridgeReady) return;
    let feedbackExpired=false;
    Object.keys(controlTargets).forEach(id=>{const target=controlTargets[id];if(!target.slow&&steadyNow()-target.createdAt>=4000){target.slow=true;feedbackExpired=true;}});
    Object.keys(controlFeedback).forEach(id=>{if(steadyNow()>=controlFeedback[id].expires){delete controlFeedback[id];feedbackExpired=true;}});
    if(feedbackExpired){controlRevision++;render();}
    try { if (reloadNativeConfiguration()) return; }
    catch (_) { /* Retry the shared configuration check on the next tick. */ }
    if (pending && !pending.abandoned &&
        steadyNow() - pending.started >= REQUEST_TIMEOUT) {
      pending.abandoned = true;
      fail(pending, pending.kind === 'service' ?
        tr('控制结果未确认，请刷新；不会重试') : tr('请求超时，等待旧请求结束'), false);
    }
    // Enforce the whole confirmation window before consuming a late result.
    const verification = pending || queued;
    if (verification && verification.kind === 'verify' &&
        !verification.abandoned && steadyNow() >= verification.data.deadline) {
      verification.abandoned = true;
      fail(verification, tr('控制结果未确认，请刷新；不会重试'), false);
    }
    let result;
    try { result = bridge.poll(); }
    catch (_) {
      if (pending && !pending.abandoned) {
        pending.abandoned = true;
        fail(pending, tr('接口异常，等待旧请求结束'), false);
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
    drainControls();
    if(usageDirty&&!pending&&!queued&&!nativeBusy&&!quickDrag&&!layoutEditing&&steadyNow()>=usageSaveAt)flushUsage();
    if (!pending && !queued && !nativeBusy && !settingsOpen && !layoutEditing && !quickDrag &&
        !brightnessIntent && !controlQueue.length &&
        autoRefresh && (token || sharedConnection()) && steadyNow() >= nextRefresh) {
      if (connected) refresh();
      else {
        phase = 'connecting';
        message = tr('正在重新读取连接状态');
        schedule('config');
        startQueued();
      }
    }
  }

  function current() {
    if(nav==='scenes'&&sceneSyncReady)return indexedEntity(selectedId,true);
    return indexedEntity(selectedId);
  }

  function available(item) {
    return item && !item.stale && item.state !== 'unavailable' &&
      (item.state !== 'unknown' || ['button','input_button','scene','script'].includes(item.domain));
  }

  function supports(item, service) {
    if(!item)return false;
    if(commonCardProfile(item)?.readonly)return false;
    if(['turn_on','turn_off'].includes(service)){
      if(sharedReady&&typeof nativeService.callService==='function'&&!['light','switch','input_boolean','fan','climate','humidifier','media_player','water_heater','scene','script'].includes(item.domain))return false;
      if(isSceneCommand(item)&&service!=='turn_on')return false;
      const flags={fan:[32,16],climate:[256,128],media_player:[128,256],water_heater:[8,8]}[item.domain];
      if(flags&&item.has_supported_features&&!(item.supported_features&flags[service==='turn_on'?0:1]))return false;
    }
    if (sharedReady) {
      try { if(controlGrant===null)controlGrant=!!nativeService.status().canControl;if(!controlGrant)return false; }
      catch (_) { return false; }
    }
    return item && controllable(item) && !!services[item.domain+'.'+service] &&
      ((!sharedReady || typeof nativeService.callService==='function') ||
       (['light','switch','input_boolean','fan'].includes(item.domain)&&['turn_on','turn_off'].includes(service)));
  }

  function control(service, brightness) {
    if (layoutEditing) return;
    const item = current();
    requestControl(item,service,brightness);
  }

  function requestControl(item,service,brightness,params,expected) {
    if(layoutEditing||!item||!connected||!available(item)||!controllable(item))return;
    if(isSceneCommand(item)&&controlTargets[item.entity_id])return;
    if (brightness !== undefined) {
      if (!item.dimmable || !Number.isFinite(brightness)) return;
      brightness=Math.max(0,Math.min(100,Math.round(brightness)));
      service=brightness===0?'turn_off':'turn_on';
    }
    if(!supports(item,service))return;
    const parameterKey=params?Object.keys(params).sort().join(','):'power';
    const commandKey=item.entity_id+'|'+(brightness!==undefined?'brightness':parameterKey);
    const index=controlQueue.findIndex(command=>command.key===commandKey);
    const previous=index>=0?controlQueue[index]:controlTargets[item.entity_id];
    const sameParams=(a,b)=>{const keys=Object.keys(a||{}).sort(),other=Object.keys(b||{}).sort();
      return keys.length===other.length&&keys.every((key,i)=>key===other[i]&&JSON.stringify(a[key])===JSON.stringify(b[key]));};
    if(previous&&previous.generation===generation&&previous.key===commandKey&&previous.service===service&&previous.brightness===brightness&&sameParams(previous.params,params)){
      render();return;
    }
    if(index<0&&controlQueue.length>=16){message=tr('待执行操作已达 16 项');render();return;}
    const command={entity:item.entity_id,service,brightness,params,expected,key:commandKey,generation,sequence:++controlSequence,createdAt:steadyNow(),slow:false};
    appendActivity(tr('提交操作'),item.name+' · '+service);
    recordUsage(item);
    if(queued&&queued.kind==='service'&&queued.entityId===item.entity_id&&queued.data.key===commandKey){
      const body=Object.assign({entity_id:item.entity_id},params||{});if(brightness>0)body.brightness_pct=brightness;
      queued.data={domain:item.domain,service,body,expected,key:commandKey,sequence:command.sequence};
    }else if(index<0)controlQueue.push(command);else controlQueue[index]=command;
    controlTargets[item.entity_id]=command;controlRevision++;
    delete controlFeedback[item.entity_id];
    if(brightness!==undefined)brightnessIntent={entity:item.entity_id,generation,percent:brightness,sent:false,sequence:command.sequence};
    else if(brightnessIntent&&brightnessIntent.entity===item.entity_id)brightnessIntent=null;
    message=pending||queued||nativeBusy?tr('待执行：')+item.name:tr('正在发送控制请求');
    drainControls();render();
  }

  function drainControls() {
    if(pending||queued||nativeBusy||disposed||!connected)return;
    while(controlQueue.length){
      const command=controlQueue.shift(),item=lookupEntity(command.entity);
      if(command.generation!==generation||!available(item)||!controllable(item)||!supports(item,command.service)||
         (command.brightness!==undefined&&!item.dimmable)){
        if(controlTargets[command.entity]===command)delete controlTargets[command.entity];
        if(brightnessIntent&&brightnessIntent.sequence===command.sequence)brightnessIntent=null;
        controlRevision++;continue;
      }
      const specs=functionControls(item);
      const valid=Object.entries(command.params||{}).every(([key,value])=>{
        if(key==='rgb_color')return lampCanColor(item)&&validRgb(value);
        const spec=specs.find(s=>s.service===command.service&&s.key===key);
        if(!spec)return false;
        if(spec.kind==='range')return Number.isFinite(value)&&value>=spec.min&&value<=spec.max;
        if(spec.kind==='options')return spec.options.includes(value);
        return spec.kind==='text'&&typeof value==='string'&&value.length>0&&value.length<=128;
      });
      if(!valid){
        if(controlTargets[command.entity]===command)delete controlTargets[command.entity];
        controlRevision++;markControlFeedback(command.entity,'failed',tr('设备功能或范围已变化，请重新设置'));continue;
      }
      const body=Object.assign({entity_id:command.entity},command.params||{});if(command.brightness>0)body.brightness_pct=command.brightness;
      controlRevision++;message=tr('正在发送：')+item.name;
      schedule('service',command.entity,{domain:item.domain,service:command.service,body,expected:command.expected,key:command.key,sequence:command.sequence});
      startQueued();return;
    }
  }

  function serviceReached(item,service){
    if(!item||['unknown','unavailable'].includes(item.state))return false;
    if(service==='turn_on')return ['climate','media_player','water_heater'].includes(item.domain)?item.state!=='off':item.state==='on';
    if(service==='turn_off')return item.state==='off';
    if(service==='open_cover')return item.state==='open'&&(item.current_position===null||item.current_position===100);
    if(service==='close_cover')return item.state==='closed';
    if(service==='stop_cover')return !['opening','closing'].includes(item.state);
    if(service==='open_cover_tilt')return item.current_tilt_position===100;
    if(service==='close_cover_tilt')return item.current_tilt_position===0;
    if(service==='media_play')return item.state==='playing';
    if(service==='media_pause'||service==='pause')return item.state==='paused';
    if(service==='media_stop')return ['idle','off'].includes(item.state);
    if(service==='start')return item.state==='cleaning';
    if(service==='stop')return ['idle','docked','paused'].includes(item.state);
    if(service==='return_to_base')return ['returning','docked'].includes(item.state);
    return true;
  }
  function matchesExpected(item,expected){
    if(!expected)return true;
    return Object.keys(expected).every(key=>{const value=expected[key];
      if(key==='rgb_color'){
        if(!validRgb(item.rgb_color)||item.color_mode==='color_temp')return false;
        const a=Math.max(...item.rgb_color),b=Math.max(...value);
        return a>0&&b>0&&value.every((v,i)=>Math.abs(item.rgb_color[i]*255/a-v*255/b)<=16);
      }
      return typeof value==='number'?
      Number.isFinite(Number(item[key]))&&Math.abs(Number(item[key])-value)<= (key==='color_temp_kelvin'?50:key==='volume_level'?0.02:0.01):item[key]===value;});
  }
  function deviceFunctions(){
    const card=currentDeviceCard(),ids=card&&card.device_entities||[selectedId];
    if(nav==='scenes'&&current())return [current()];
    const members=ids.map(id=>indexedEntity(id)).filter(Boolean);
    if(members.length<2)return members;
    // Stable partition: generate each member's descriptors once, with current
    // grants/capabilities/state, and retain order within both groups.
    const actions=[],readings=[];
    for(const item of members)(functionControls(item).length?actions:readings).push(item);
    return actions.concat(readings);
  }
  function functionControls(item){
    if(!item)return [];
    const out=[],nativeActions=!sharedReady||typeof nativeService.callService==='function';
    const add=(title,kind,service,key,value,min,max,step,field,options)=>{
      if(!supports(item,service)||(!nativeActions&&key&&key!=='brightness_pct'))return;
      if(kind==='range'&&(!Number.isFinite(min)||!Number.isFinite(max)||max<=min))return;
      if(kind==='options'&&(!options||!options.length))return;
      out.push({title,kind,service,key,value,min,max,step:step||1,field,options});
    };
    const range=(title,service,key,value,min,max,step,field)=>add(title,'range',service,key,value,min,max,step,field);
    const opts=(title,service,key,value,options,field)=>add(title,'options',service,key,value,0,0,1,field,options);
    const action=(title,service)=>add(title,'action',service);
    const feature=bit=>!item.has_supported_features||!!(item.supported_features&bit);
    if(item.domain==='light'){
      if(item.dimmable)range(tr('亮度'),'turn_on','brightness_pct',displayBrightness(item),0,100,1,'brightness');
      if(item.color_temperature)range(tr('色温'),'turn_on','color_temp_kelvin',item.color_temp_kelvin,item.min_color_temp_kelvin,item.max_color_temp_kelvin,1,'color_temp_kelvin');
      if(feature(4))opts(tr('灯光效果'),'turn_on','effect',item.effect,item.effects,'effect');
    }
    if(['number','input_number'].includes(item.domain))range('设定值','set_value','value',Number(item.state),item.minimum,item.maximum,item.step,'state');
    if(['select','input_select'].includes(item.domain))opts('选项','select_option','option',item.state,item.options,'state');
    if(['button','input_button'].includes(item.domain))action(tr('执行'),'press');
    if(isSceneCommand(item))action(item.domain==='scene'?'执行场景':'运行脚本','turn_on');
    if(['text','input_text'].includes(item.domain))add(tr('编辑内容'),'text','set_value','value',item.state,0,0,1,'state');
    if(item.domain==='fan'){
      if(feature(1)&&(item.percentage!==null||(item.supported_features&1)))range(tr('风速'),'set_percentage','percentage',item.percentage,0,100,item.percentage_step||1,'percentage');
      if(feature(8))opts(tr('模式'),'set_preset_mode','preset_mode',item.preset_mode,item.preset_modes,'preset_mode');
      if(item.supported_features&4)opts('风向','set_direction','direction',item.direction,['forward','reverse'],'direction');
      if(item.supported_features&2)opts(tr('摆风'),'oscillate','oscillating',item.oscillating,[false,true],'oscillating');
    }
    if(item.domain==='cover'){
      if(item.supported_features&4)range('开合位置','set_cover_position','position',item.current_position,0,100,1,'current_position');
      if(item.supported_features&128)range('叶片角度','set_cover_tilt_position','tilt_position',item.current_tilt_position,0,100,1,'current_tilt_position');
      if(item.supported_features&16)action('打开叶片','open_cover_tilt');if(item.supported_features&32)action('关闭叶片','close_cover_tilt');if(item.supported_features&64)action('停止叶片','stop_cover_tilt');
      if(feature(1))action(tr('打开'),'open_cover');if(feature(2))action(tr('关闭'),'close_cover');if(feature(8))action(tr('停止'),'stop_cover');
    }
    if(['climate','water_heater'].includes(item.domain)){
      if(feature(1)&&(item.temperature!==null||(item.supported_features&1)))range(tr('目标温度'),'set_temperature','temperature',item.temperature,item.min_temp===null?5:item.min_temp,item.max_temp===null?35:item.max_temp,item.target_temp_step||0.5,'temperature');
      if(item.domain==='climate'){
        if((item.supported_features&2)&&item.target_temp_low!==null&&item.target_temp_high!==null){
          range('温度下限','set_temperature','target_temp_low',item.target_temp_low,item.min_temp===null?7:item.min_temp,item.max_temp===null?35:item.max_temp,item.target_temp_step||0.5,'target_temp_low');
          range('温度上限','set_temperature','target_temp_high',item.target_temp_high,item.min_temp===null?7:item.min_temp,item.max_temp===null?35:item.max_temp,item.target_temp_step||0.5,'target_temp_high');
        }
        if(item.supported_features&4)range(tr('目标湿度'),'set_humidity','humidity',item.humidity,item.min_humidity===null?30:item.min_humidity,item.max_humidity===null?99:item.max_humidity,1,'humidity');
        if(item.supported_features&512)opts('水平摆风','set_swing_horizontal_mode','swing_horizontal_mode',item.swing_horizontal_mode,item.swing_horizontal_modes,'swing_horizontal_mode');
        opts(tr('运行模式'),'set_hvac_mode','hvac_mode',item.state,item.hvac_modes,'state');
        if(feature(8))opts('风速模式','set_fan_mode','fan_mode',item.fan_mode,item.fan_modes,'fan_mode');
        if(feature(16))opts('预设模式','set_preset_mode','preset_mode',item.preset_mode,item.preset_modes,'preset_mode');
        if(feature(32))opts('摆风模式','set_swing_mode','swing_mode',item.swing_mode,item.swing_modes,'swing_mode');
      }else{
        if(feature(2))opts(tr('运行模式'),'set_operation_mode','operation_mode',item.operation_mode,item.operation_modes,'operation_mode');
        if(item.supported_features&4)opts('外出模式','set_away_mode','away_mode',item.away_mode,[false,true],'away_mode');
      }
    }
    if(item.domain==='humidifier'){
      range(tr('目标湿度'),'set_humidity','humidity',item.humidity,item.min_humidity===null?0:item.min_humidity,item.max_humidity===null?100:item.max_humidity,1,'humidity');
      if(feature(1))opts(tr('运行模式'),'set_mode','mode',item.mode,item.modes,'mode');
    }
    if(item.domain==='media_player'){
      if(item.supported_features&4)range(tr('音量'),'volume_set','volume_level',item.volume_level,0,1,0.01,'volume_level');
      if(feature(2048))opts('输入源','select_source','source',item.source,item.sources,'source');
      if(item.supported_features&8)opts(tr('静音'),'volume_mute','is_volume_muted',item.is_volume_muted,[false,true],'is_volume_muted');
      if(feature(16384))action(tr('播放'),'media_play');if(feature(1))action(tr('暂停'),'media_pause');if(feature(32))action('下一首','media_next_track');if(feature(16))action('上一首','media_previous_track');if(feature(4096))action(tr('停止'),'media_stop');
    }
    if(item.domain==='vacuum'){
      if(feature(8192))action('开始清扫','start');if(feature(4))action(tr('暂停'),'pause');if(feature(8))action(tr('停止'),'stop');if(feature(16))action(tr('回充'),'return_to_base');
      if(feature(32))opts('清扫强度','set_fan_speed','fan_speed',item.fan_speed,item.fan_speeds,'fan_speed');
    }
    if(item.domain==='lock'){action('上锁','lock');action(tr('解锁'),'unlock');}
    const toggle=powerIsOn(item)?'turn_off':'turn_on';
    if(!isSceneCommand(item)&&supports(item,toggle))out.push({title:powerIsOn(item)?'关闭电源':'开启电源',kind:'action',service:toggle});
    return out;
  }
  function requestFunction(item,spec,value){
    if(!item||!spec)return;
    spec=functionControls(item).find(c=>c.service===spec.service&&c.key===spec.key);
    if(!spec)return;
    if(isSceneCommand(item)){if(spec.kind==='action'&&spec.service==='turn_on')activateScene(item);return;}
    if(spec.kind==='range'){
      if(!Number.isFinite(value))return;
      value=Math.max(spec.min,Math.min(spec.max,spec.min+Math.round((value-spec.min)/spec.step)*spec.step));
      value=Number(value.toFixed(6));
    }
    if(spec.kind==='options'&&!spec.options.includes(value))return;
    if(spec.kind==='text'&&(typeof value!=='string'||value.length===0||value.length>128))return;
    let params=spec.key?{[spec.key]:value}:undefined;
    let expected=spec.field?{[spec.field]:spec.field==='state'?String(value):value}:undefined;
    if(['target_temp_low','target_temp_high'].includes(spec.key)){
      params={target_temp_low:item.target_temp_low,target_temp_high:item.target_temp_high,[spec.key]:value};
      if(params.target_temp_low===null||params.target_temp_high===null||params.target_temp_low>params.target_temp_high){message='温度下限不能高于上限';render();return;}
      expected=Object.assign({},params);
    }
    if(spec.key==='brightness_pct')requestControl(item,spec.service,value);
    else requestControl(item,spec.service,undefined,params,expected);
  }
  function controlValue(item,spec){
    const target=controlTargets[item.entity_id];
    return target&&target.params&&target.params[spec.key]!==undefined?target.params[spec.key]:spec.value;
  }
  function formatControl(spec,value){
    if(value===null||value===undefined||!Number.isFinite(Number(value)))return '--';
    const v=Number.isInteger(Number(value))?String(Number(value)):numberText(Number(value));
    return spec.key==='color_temp_kelvin'?Math.round(value)+'K':spec.key==='volume_level'?Math.round(value*100)+'%':
      ['brightness_pct','percentage','position','tilt_position','humidity'].includes(spec.key)?Math.round(value)+'%':['temperature','target_temp_low','target_temp_high'].includes(spec.key)?v+' °C':v;
  }
  function temperatureColor(kelvin){
    const t=Math.max(0,Math.min(1,(kelvin-2000)/4500));
    return (255<<16)|(Math.round(205+50*t)<<8)|Math.round(130+125*t);
  }
  function validRgb(value){return Array.isArray(value)&&value.length===3&&value.every(v=>typeof v==='number'&&Number.isInteger(v)&&v>=0&&v<=255);}
  function rgbColor(rgb){return (rgb[0]<<16)|(rgb[1]<<8)|rgb[2];}
  function lampCanColor(item){return !!item&&item.domain==='light'&&item.color_capable&&(!sharedReady||typeof nativeService.callService==='function')&&supports(item,'turn_on');}
  function lampRgb(item){const target=controlTargets[item.entity_id];return target&&validRgb(target.params&&target.params.rgb_color)?target.params.rgb_color:validRgb(item.rgb_color)?item.rgb_color:[255,150,46];}
  function setLampRgb(entity,rgb){
    const item=lookupEntity(entity);
    if(!lampCanColor(item)||!validRgb(rgb))return;
    requestControl(item,'turn_on',undefined,{rgb_color:rgb.slice()},{rgb_color:rgb.slice()});
  }

  function changeBrightness(delta) {
    const item = current();
    if (!item || !item.dimmable) return;
    const level = displayBrightness(item);
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
  function toggleDeviceFollow(){
    const card=currentDeviceCard();if(!card)return;
    if(card.device_entities.some(id=>favorites.includes(id)))favorites=favorites.filter(id=>!card.device_entities.includes(id));
    else if(favorites.length<MAX_FAVORITES)favorites.push(card.entity_id);
    else{message='关注列表已达 16 个实体';render();return;}
    save('eh_favs',JSON.stringify(favorites));render();
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
    const revision=generation;
    cancelInputs();browseOpen=false;
    prompt.input({ title: '搜索名称或实体 ID', value: search, maxLength: 64 }, value => {
      if (disposed || revision!==generation || value === null || value === undefined) return;
      search = clean(value, 64).trim().toLowerCase();
      discoveryRevision++;resetBoardScroll();roomPage=0;
      selectedId = '';
      page = 0;
      settingsOpen = false;
      detailOpen = false;
      render();
    });
    render();
  }

  function activeDevices() {
    return entities.filter(item => available(item) && item.state === 'on' &&
      ['light', 'switch', 'input_boolean', 'fan'].includes(item.domain)).length;
  }


  // ------------------------------------------------------------------
  // Mi quick cards: category thumbnail, separate power action, name/status.
  // 只用面板、文本和按钮，不做逐点/逐柱的装饰图形，控件数与原生版持平。
  // ------------------------------------------------------------------
  const SIDEBAR_W = 164, HEADER_H = 64, FOOTER_H = 54;
  const sidebarStyle={pad:12,itemWidth:140,itemHeight:44,gap:8,radius:12,paper:dark?0x22252f:0xffffff,selected:dark?0x2e303d:0xf3f2f7,accent:dark?0xc1aedb:0x77658f,ink:dark?0xeeeef5:0x292c38};
  if(ui.theme)Object.assign(sidebarStyle,{paper:ui.theme.card,selected:ui.theme.surface,
    accent:ui.theme.accent,ink:ui.theme.primary,pad:ui.theme.sidebarPad,itemWidth:ui.theme.sidebarItemWidth,
    itemHeight:ui.theme.sidebarItemHeight,gap:ui.theme.sidebarGap,radius:ui.theme.controlRadius});
  const GRID_X = SIDEBAR_W, GRID_Y = HEADER_H;
  const GRID_W = W - SIDEBAR_W;
  const MAIN_X = SIDEBAR_W + 14, MAIN_Y = HEADER_H + 16;
  const MAIN_W = W - MAIN_X - 14;
  const WATERFALL=['viewport','attach','scrollInfo','scrollTo','scrollExtent','scrollLock'].every(name=>typeof ui[name]==='function');
  const VIEW_H=H-HEADER_H-FOOTER_H;
  const CARD_COLS = WATERFALL?(GRID_W<420?1:GRID_W<700?2:3):(W<900?2:3), CARD_ROWS = H < 480 ? 1 : 2;
  const CARD_GAP = 14, CARD_MARGIN = 16;
  const CARD_W = Math.floor((GRID_W - CARD_MARGIN * 2 - CARD_GAP * (CARD_COLS - 1)) /
    CARD_COLS);
  const CARD_TOP = HEADER_H + 12;
  const CARD_H = Math.floor((H - FOOTER_H - CARD_TOP - 12 -
    CARD_GAP * (CARD_ROWS - 1)) / CARD_ROWS);
  const SLOTS = WATERFALL?12:CARD_COLS*CARD_ROWS;
  let boardScroll=0,boardTouchActive=false,boardLocked=false,boardAnchorRestore=null,boardPress=null;
  const attached=Object.create(null);
  function attachToBoard(id,parent){
    if(!WATERFALL||id===null||id===undefined||attached[id]===parent)return;
    ui.attach(id,parent);attached[id]=parent;return true;
  }
  function boardLock(locked){
    if(WATERFALL&&widgets&&widgets.boardViewport&&boardLocked!==locked){ui.scrollLock(widgets.boardViewport,locked);boardLocked=locked;}
  }
  function cancelInputs(consume=true){
    const active=!!quickDrag||!!lampWheelDrag||boardTouchActive;
    if(quickDrag&&quickDrag.group)quickDrag.group.renderKey='';
    if(menuDrag)menuClickUntil=steadyNow()+300;
    quickDrag=null;lampWheelDrag=null;boardTouchActive=false;boardPress=null;menuDrag=null;
    boardLock(false);
    if(active){cardRenderKey='';detailRenderKey='';}
    if(consume&&active)quickGestureOwned=true;
  }
  function validateInputs(){
    if(quickDrag){
      const item=lookupEntity(quickDrag.item.entity_id),spec=quickDrag.rect.spec;
      let valid=connected&&available(item)&&quickDrag.generation===generation&&!layoutEditing;
      if(valid&&spec){const current=functionControls(item).find(row=>row.key===spec.key&&row.service===spec.service&&row.kind==='range');
        valid=!!current&&current.min===spec.min&&current.max===spec.max&&current.step===spec.step;
      }else if(valid)valid=canAdjustBrightness(item);
      if(!valid)cancelInputs();
    }
    if(lampWheelDrag){
      const item=lookupEntity(lampWheelDrag.entity);
      if(!connected||!available(item)||lampWheelDrag.generation!==generation||!lampCanColor(item))cancelInputs();
    }
  }
  function openSettings(){
    cancelInputs();browseOpen=false;optionPicker=null;detailOpen=false;settingsOpen=true;render();
  }
  function resetBoardScroll(){
    boardTouchActive=false;boardAnchorRestore=null;boardPress=null;
    if(WATERFALL&&widgets&&widgets.boardViewport){boardLock(false);ui.scrollTo(widgets.boardViewport,0,false);}
    boardScroll=0;
  }
  function boardScrolled(y){
    if(disposed||!widgets)return;
    y=Math.max(0,Math.round(y));
    const moved=y!==boardScroll;boardScroll=y;if(moved&&boardTouchActive)quickGestureOwned=true;
    if(detailOpen||settingsOpen)return;
    // LVGL moves the clipped children. Only a changed virtual window needs JS paint.
    const key=boardWindow(dashboardPages()[0].items).map(row=>row?row.item.entity_id:'').join(',');
    if(key!==widgets.boardWindowKey)render();
  }
  function masonryHeight(item){
    const rich=item.domain==='light'&&item.dimmable||['fan','climate','cover','humidifier','media_player','water_heater','number','input_number'].includes(item.domain);
    const height=item.domain==='camera'?Math.max(248,Math.round((CARD_W-32)*9/16)+136):rich?212:isSceneCommand(item)?182:controllable(item)?190:168;
    return Math.max(height,Math.ceil(VIEW_H/3));
  }
  function boardWindow(rows){
    if(!WATERFALL)return rows;
    const top=HEADER_H+boardScroll,bottom=top+VIEW_H;
    // A layout's rows are immutable and each column has non-overlapping,
    // increasing vertical ranges. Rebuild only when the layout is replaced.
    if(windowRows!==rows){
      windowColumns=Array.from({length:CARD_COLS},()=>[]);
      for(const row of rows)windowColumns[row.column].push(row);
      windowRows=rows;
    }
    const candidates=[];
    for(const column of windowColumns){
      let low=0,high=column.length;
      while(low<high){const mid=(low+high)>>>1,row=column[mid];
        if(row.y+row.h<=top-110)low=mid+1;else high=mid;}
      for(let index=low;index<column.length;index++){
        const row=column[index];if(row.y>=bottom+110)break;candidates.push(row);
      }
    }
    candidates.sort((a,b)=>{
      const distance=row=>row.y+row.h<=top?top-row.y-row.h:row.y>=bottom?row.y-bottom:0;
      return distance(a)-distance(b)||a.y-b.y||a.x-b.x;
    });
    const wanted=candidates.slice(0,SLOTS),slots=Array(SLOTS).fill(null),used=new Set();
    wanted.forEach(row=>{const index=widgets.cards.findIndex((card,i)=>!used.has(i)&&card.item&&card.item.entity_id===row.item.entity_id);if(index>=0){slots[index]=row;used.add(index);}});
    wanted.forEach(row=>{if(slots.includes(row))return;const index=slots.indexOf(null);if(index>=0)slots[index]=row;});
    return slots;
  }
  function attachCard(card){
    if(!WATERFALL)return;
    let changed=false;
    ['panel','iconBase','powerHalo','switchFace','track','fill','thumb','icon','powerGlyph','name','value','unit','state','moreLabel','minusGlyph','plusGlyph','cameraGlyph'].forEach(key=>{changed=attachToBoard(card[key],widgets.boardViewport)||changed;});
    if(changed&&typeof ui.front==='function')['icon','powerGlyph','thumb','name','state','value','unit','moreLabel','minusGlyph','plusGlyph','cameraGlyph'].forEach(key=>{if(card[key]!==null&&card[key]!==undefined)ui.front(card[key]);});
  }

  const pal = dark ? {
    page: 0x14171b, card: 0x202020, line: 0x303030, ink: 0xf1f3f5,
    muted: 0x989fa7, device: 0xc1cbd3, deviceBase: 0x4a555f, blue: 0x379bff, blueSoft: 0x203348,
    soft: 0x2b3036, track: 0x373d44, good: 0x64c6aa, warn: 0xe7b065,
    yellow: 0xe5bb68, teal: 0x5cbbb2, orange: 0xe7b065, purple: 0xa191c9,
    red: 0xe48787, yellowSoft: 0x3a3325, tealSoft: 0x203a3c,
    orangeSoft: 0x3e3226, purpleSoft: 0x332c40
  } : {
    page: 0xf3f4f6, card: 0xffffff, line: 0xe9ecef, ink: 0x22262a,
    muted: 0x969da4, device: 0x647681, deviceBase: 0xdde5e9, blue: 0x379bff, blueSoft: 0xeaf4ff,
    soft: 0xf7f8f9, track: 0xeef0f2, good: 0x36aa87, warn: 0xda9a42,
    yellow: 0xd7a145, teal: 0x36aaa5, orange: 0xd69345, purple: 0x9a83b8,
    red: 0xcf7272, yellowSoft: 0xfff6e4, tealSoft: 0xe7f7f7,
    orangeSoft: 0xfff3e7, purpleSoft: 0xf2edf8
  };
  // The reviewed unified layout has small overview cards and one detail page.
  Object.assign(pal,dark?{page:0x101519,card:0x20262c,track:0x303640,
    ink:0xf1f3f5,muted:0xa6b0b7,blue:0x329aff,blueSoft:0x21394f}:
    {page:0xf5f6f7,card:0xffffff,track:0xe9edf2,ink:0x23292c,
      muted:0x64747e,blue:0x167be1,blueSoft:0xe6f2ff});

  /* One cache keeps a re-render cheap: only changed widgets reach the runtime. */
  const paint = Object.create(null);
  let loggedEntities = -1;

  // Per-launch FIFO; cap both entry count and retained user text.
  const textMetrics = new Map();
  function measureTextCached(text,font) {
    text=String(text);
    if(text.length>128)return ui.measureText(text,font);
    const key=font+'\u0000'+text;
    if(textMetrics.has(key))return textMetrics.get(key);
    const value=ui.measureText(text,font);
    if(textMetrics.size>=64)textMetrics.delete(textMetrics.keys().next().value);
    textMetrics.set(key,value);
    return value;
  }

  function pushText(id, text) {
    const key = 't' + id;
    if (paint[key] === text) return;
    paint[key] = text;
    ui.setText(id, text);
  }

  function pushColor(id, color, smooth) {
    const key = 'c' + id,mode='cm'+id;
    if (paint[key] === color&&paint[mode]===!!smooth) return;
    paint[key] = color;paint[mode]=!!smooth;
    ui.setColor(id, color, smooth === true);
  }

  function pushHidden(id, hidden) {
    const key = 'h' + id;
    const value = hidden ? 1 : 0;
    if (paint[key] === value) return;
    paint[key] = value;
    ui.setHidden(id, !!hidden);
  }

  function pushSize(id, w, h, smooth) {
    const key = 's' + id;
    const value = w + ',' + h + ',' + !!smooth;
    if (paint[key] === value) return;
    paint[key] = value;
    ui.setSize(id, w, h, !!smooth);
  }

  function pushPos(id, x, y, smooth) {
    const key = 'p' + id;
    const value = x + ',' + y + ',' + !!smooth;
    if (paint[key] === value) return;
    paint[key] = value;
    ui.setPos(id, x, y, !!smooth);
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
      if(target&&target.section==='navigation'&&steadyNow()<menuClickUntil)return;
      if(WATERFALL&&target&&target.viewport){
        if(quickGestureOwned)return;
        if(boardPress&&(boardPress.generation!==generation||!target.card||!target.card.item||boardPress.entity!==target.card.item.entity_id||Math.abs(boardScroll-boardPress.scroll)>3))return;
      }
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
    // Recycle the native slots across views; hidden profile choices do not
    // allocate additional native buttons or consume the 64 visible slots.
    const visibleButtons=buttons.filter(button=>!button.hidden&&(browseOpen?button.section==='browse':optionPicker?button.section==='picker':
      button.section==='common'||button.section==='navigation'&&view!=='settings'||
      (button.section==='card'||button.section==='cardIcon'?button._cardVisible:button.section===view)));
    if (visibleButtons.length > buttonPool.length) throw new Error('Button pool capacity exceeded');
    for (let index = 0; index < buttonPool.length; index++) {
      const slot = buttonPool[index];
      const button = visibleButtons[index]||null;
      const active = !!button;
      slot.target = active ? button : null;
      const style = active ? button.style : null;
      const signature = active ? [button.x, button.y, button.w, button.h, button.text,
        button.color, button.hidden ? 1 : 0, style.radius, style.borderWidth,
        style.borderColor || 0, style.fontSize || 0, style.enabled === 0 ? 0 : 1,button.section==='navigation'?1:0].join('|') : 'off';
      if(WATERFALL){
        const parent=active&&button.viewport&&view==='list'?widgets.boardViewport:0;
        attachToBoard(slot.id,parent);
        const opacity=active&&!button.text?0:255;
        if(slot.opacity!==opacity){ui.setOpacity(slot.id,opacity);slot.opacity=opacity;}
      }
      if(active&&(browseOpen||button===widgets.emptyAction)&&typeof ui.front==='function')ui.front(slot.id);
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
      pushStyle(slot.id, 'B', Object.assign({},button.style,{pressLock:button.section==='navigation'?1:0}));
      pushHidden(slot.id, button.hidden);
    }
  }

  function label(text, x, y, w, h, font, color, icon) {
    const id = ui.text(text, x, y, font, color, icon > 1 ? icon : icon ? 1 : 0);
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
    return !!item && !item.product_readonly && ['light','switch','input_boolean','fan','number','input_number','select','input_select','button','input_button','text','input_text','climate','cover','humidifier','media_player','vacuum','water_heater','lock','scene','script'].includes(item.domain);
  }

  const STATE_WORDS = {
    on: tr('已开启'), off: tr('已关闭'), unavailable: tr('不可用'), unknown: tr('未知'),
    open: tr('已打开'), opening: tr('正在打开'), closing: tr('正在关闭'), closed: tr('已关闭'),
    playing: tr('播放中'), paused: tr('已暂停'), idle: tr('空闲'), standby: tr('待机'),
    home: tr('在家'), not_home: tr('离家'), away: tr('外出'),
    forward:tr('正转'),reverse:tr('反转'),
    above_horizon: tr('日间'), below_horizon: tr('夜间'),
    heat: tr('制热'), cool: tr('制冷'), heat_cool: tr('自动'), auto: tr('自动'),
    dry: tr('除湿'), fan_only: tr('送风'), heating: tr('正在制热'), cooling: tr('正在制冷'),
    locked: tr('已上锁'), unlocked: tr('已解锁'), detected: tr('已触发'), clear: tr('正常'),
    sunny: tr('晴'), cloudy: tr('多云'), partlycloudy: tr('局部多云'), rainy: tr('下雨'),
    pouring: tr('大雨'), snowy: tr('下雪'), fog: tr('雾'), windy: tr('大风'), lightning: tr('雷电'),
    charging: tr('充电中'), discharging: tr('放电中'), full: tr('已充满')
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
    const unit = (item.unit === 'C' ? '°C' : item.unit || '').replace(/℃/g,'°C').replace(/℉/g,'°F');
    if (!unit) return numberText(item.state);
    if (unit === '%') return numberText(item.state) + '%';
    return numberText(item.state) + ' ' + unit;
  }

  function sensorLabel(item) {
    const text = item.name + ' ' + item.entity_id;
    if (isTemperature(item)) return tr('温度');
    if (isHumidity(item)) return tr('湿度');
    if (/illuminance|lux|光照/i.test(text)) return tr('光照');
    if (/voltage|电压/i.test(text)) return tr('电压');
    if (/power|功率/i.test(text)) return tr('功率');
    if (/current|电流/i.test(text)) return tr('电流');
    if (/battery|电量/i.test(text)) return tr('电量');
    if (/rain|降水|雨/i.test(text)) return tr('降水量');
    if (/co2|carbon/i.test(text)) return tr('二氧化碳');
    if (/pressure|气压/i.test(text)) return tr('气压');
    return tr('传感器');
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
    if (!available(item)) return item.state === 'unavailable' ? tr('不可用') : tr('未知');
    if (controllable(item)) {
      if (item.state !== 'on') return tr('已关闭');
      return item.brightness !== null ? tr('已开启 · ') + item.brightness + '%' : tr('已开启');
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

  function friendlyState(item) {
    if(item&&item.domain==='scene'&&!item.stale){
      if(item.state==='unknown')return tr('尚未触发');
      if(item.state!=='unavailable')return tr('已同步');
    }
    if(item&&item.domain==='script'&&['on','off'].includes(item.state))return item.state==='on'?tr('运行中'):tr('待命');
    const states = {
      on: tr('\u5df2\u5f00\u542f'), off: tr('\u5df2\u5173\u95ed'), unavailable: tr('\u4e0d\u53ef\u7528'),
      unknown: tr('\u672a\u77e5'), open: tr('\u5df2\u6253\u5f00'), closed: tr('\u5df2\u5173\u95ed'),
      opening: tr('\u6b63\u5728\u6253\u5f00'), closing: tr('\u6b63\u5728\u5173\u95ed'), playing: tr('\u6b63\u5728\u64ad\u653e'),
      paused: tr('\u5df2\u6682\u505c'), idle: tr('\u7a7a\u95f2'), standby: tr('\u5f85\u673a'), home: tr('\u5728\u5bb6'),
      not_home: tr('\u79bb\u5bb6'), away: tr('\u79bb\u5bb6'), above_horizon: tr('\u5730\u5e73\u7ebf\u4e0a'),
      below_horizon: tr('\u5730\u5e73\u7ebf\u4e0b'), locked: tr('\u5df2\u4e0a\u9501'), unlocked: tr('\u5df2\u89e3\u9501'),
      heat: tr('\u5236\u70ed'), cool: tr('\u5236\u51b7'), auto: tr('\u81ea\u52a8'), dry: tr('\u9664\u6e7f'),
      fan_only: tr('\u9001\u98ce'), heating: tr('\u6b63\u5728\u5236\u70ed'), cooling: tr('\u6b63\u5728\u5236\u51b7'),
      sunny: tr('\u6674'), cloudy: tr('\u591a\u4e91'), partlycloudy: tr('\u5c40\u90e8\u591a\u4e91'), rainy: tr('\u4e0b\u96e8'),
      charging: tr('\u5145\u7535\u4e2d'), discharging: tr('\u653e\u7535\u4e2d'), full: tr('\u5df2\u5145\u6ee1')
      ,turbo:tr('强力'),silent:tr('静音'),sleep:tr('睡眠'),low:tr('低速'),medium:tr('中速'),high:tr('高速'),
      cooking:tr('烹饪中'),preheating:tr('预热中'),washing:tr('洗涤中'),rinsing:tr('漂洗中'),
      spinning:tr('脱水中'),drying:tr('烘干中'),printing:tr('打印中'),completed:tr('已完成'),
      finished:tr('已完成'),error:tr('设备异常'),normal:tr('未触发报警'),detected:tr('已触发'),clear:tr('未触发报警')
    };
    const value = String(item && item.state || '');
    return states[value.toLowerCase()] || value;
  }

  function binaryState(item) {
    const active = item.state === 'on';
    const words = {
      door: [tr('\u5df2\u5173\u95ed'), tr('\u5df2\u6253\u5f00')], window: [tr('\u5df2\u5173\u95ed'), tr('\u5df2\u6253\u5f00')],
      garage_door: [tr('\u5df2\u5173\u95ed'), tr('\u5df2\u6253\u5f00')], opening: [tr('\u5df2\u5173\u95ed'), tr('\u5df2\u6253\u5f00')],
      motion: [tr('\u65e0\u6d3b\u52a8'), tr('\u68c0\u6d4b\u5230\u6d3b\u52a8')], occupancy: [tr('\u65e0\u4eba'), tr('\u6709\u4eba')],
      presence: [tr('\u65e0\u4eba'), tr('\u6709\u4eba')], smoke: [tr('\u6b63\u5e38'), tr('\u68c0\u6d4b\u5230\u70df\u96fe')],
      gas: [tr('\u6b63\u5e38'), tr('\u68c0\u6d4b\u5230\u71c3\u6c14')], moisture: [tr('\u5e72\u71e5'), tr('\u68c0\u6d4b\u5230\u6f0f\u6c34')],
      problem: [tr('\u6b63\u5e38'), tr('\u9700\u8981\u6ce8\u610f')], connectivity: [tr('\u79bb\u7ebf'), tr('\u5728\u7ebf')]
    };
    const pair = words[item.device_class] || [tr('\u6b63\u5e38'), tr('\u5df2\u89e6\u53d1')];
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
    if (!available(item)) return item.state === 'unavailable' ? tr('\u5b9e\u4f53\u79bb\u7ebf') : tr('\u72b6\u6001\u672a\u77e5');
    if (item.domain === 'climate') {
      const target = item.temperature === null ? '' : tr('\u76ee\u6807 ') + numberText(item.temperature) + ' \u00b0C';
      const action = item.hvac_action ? friendlyState({ state: item.hvac_action }) : friendlyState(item);
      return target ? action + '  \u00b7  ' + target : action;
    }
    if (item.domain === 'media_player') {
      if (item.volume_level !== null) return friendlyState(item) + tr('  \u00b7  \u97f3\u91cf ') +
        Math.round(item.volume_level * 100) + '%';
      return friendlyState(item);
    }
    if (item.domain === 'cover') return friendlyState(item) +
      (item.current_position === null ? '' : '  \u00b7  ' + item.current_position + '%');
    if (controllable(item)) return friendlyState(item) +
      (item.brightness !== null && item.state === 'on' ? tr('  \u00b7  \u4eae\u5ea6') : '');
    if (item.domain === 'sensor') return ({
      temperature: tr('\u6e29\u5ea6'), humidity: tr('\u6e7f\u5ea6'), battery: tr('\u7535\u91cf'),
      power: tr('\u529f\u7387'), energy: tr('\u80fd\u8017'), voltage: tr('\u7535\u538b'), current: tr('\u7535\u6d41'),
      illuminance: tr('\u5149\u7167'), pressure: tr('\u6c14\u538b'), carbon_dioxide: 'CO2'
    })[item.device_class] || tr('\u4f20\u611f\u5668');
    return friendlyState(item);
  }

  function dashboardIcon(item) {
    const byClass = {
      temperature: '\uf2c9', humidity: '\uf043', battery: '\uf240', power: '\uf0e7',
      energy: '\uf0e7', voltage: '\uf0e7', current: '\uf0e7', illuminance: '\uf06e',
      door: '\uf0c9', window: '\uf0c9', motion: '\uf06e', occupancy: '\uf06e',
      smoke: '\uf071', gas: '\uf071', moisture: '\uf043', connectivity: '\uf1eb'
    };
    if (byClass[item.device_class]) return byClass[item.device_class];
    return {
      light: '\uf0eb', switch: '\uf011', input_boolean: '\uf011', fan: '\uf2dc',
      sensor: '\uf06e', binary_sensor: '\uf071', cover: '\uf0c9', climate: '\uf0e7',
      media_player: '\uf04b', person: '\uf124', device_tracker: '\uf124', weather: '\uf043',
      humidifier:'\uf043',water_heater:'\uf0e7',
      sun: '\uf06e', update: '\uf019', lock: '\uf00c', vacuum: '\uf079', camera: '\uf03e',
      alarm_control_panel: '\uf071', automation: '\uf074', script: '\uf15b', scene: '\ue001',
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

  // Mi quick-card categories mapped to HA properties. Registration describes
  // presentation only; native-service control permissions remain authoritative.
  const quickCardRegistry = Object.create(null);
  function registerQuickCards(types, family, kind) {
    types.split(' ').forEach(type => { quickCardRegistry[type] = { family, kind }; });
  }
  registerQuickCards('light', 'lamp', 'light');
  registerQuickCards('outlet switch relay', 'plug', 'switch');
  registerQuickCards('fan ceiling-fan', 'fan', 'fan');
  registerQuickCards('air-purifier air-fresh', 'purifier', 'air');
  registerQuickCards('air-conditioner', 'climate', 'climate');
  registerQuickCards('curtain airer', 'curtain', 'position');
  registerQuickCards('temperature-humidity-sensor air-monitor illumination-sensor', 'sensor', 'sensor');
  registerQuickCards('magnet-sensor', 'contact', 'status');
  registerQuickCards('gas-concentration gas-sensor smoke-sensor motion-sensor submersion-sensor', 'sensor', 'status');
  registerQuickCards('speaker', 'media', 'media');
  registerQuickCards('camera video-doorbell', 'camera', 'status');
  registerQuickCards('vacuum', 'vacuum', 'battery');
  registerQuickCards('humidifier diffuser pet-drinking-fountain', 'humidifier', 'humidity');
  registerQuickCards('heater bath-heater towel-rack water-heater water-dispenser', 'heater', 'temperature');
  registerQuickCards('flowerpot plant-monitor', 'plant', 'sensor');
  registerQuickCards('washer dishwasher', 'washer', 'status');
  registerQuickCards('air-fryer coffee-machine cooker fruit-vegetable-purifier health-pot hood induction-cooker integrated-stove juicer kettle oven pressure-cooker water-purifier', 'kitchen', 'status');
  registerQuickCards('fridge garbage-can gateway massage mirror mosquito-dispeller moxibustion printer treadmill walking-pad', 'generic', 'status');
  /* HA_COMMON_PROFILES_BEGIN */
  const commonCardProfiles = JSON.parse("[{\"id\":\"bedside-light\",\"name\":\"灯具\",\"types\":[\"light\"],\"domains\":[\"light\"],\"primary\":[\"brightness_pct\",\"color_temp_kelvin\"],\"secondary\":[\"effect\"],\"quick\":\"brightness_pct\"},{\"id\":\"outlet\",\"name\":\"插座\",\"types\":[\"outlet\",\"switch\",\"relay\"],\"domains\":[\"switch\",\"input_boolean\"],\"primary\":[],\"secondary\":[],\"quick\":\"power\"},{\"id\":\"fan\",\"name\":\"风扇\",\"types\":[\"fan\",\"ceiling-fan\"],\"domains\":[\"fan\"],\"primary\":[\"percentage\",\"preset_mode\"],\"secondary\":[\"oscillating\"],\"quick\":\"percentage\"},{\"id\":\"air-conditioner\",\"name\":\"空调\",\"types\":[\"air-conditioner\"],\"domains\":[\"climate\"],\"primary\":[\"temperature\",\"hvac_mode\"],\"secondary\":[\"fan_mode\",\"swing_mode\",\"preset_mode\"],\"quick\":\"temperature\"},{\"id\":\"curtain\",\"name\":\"窗帘\",\"types\":[\"curtain\",\"airer\"],\"domains\":[\"cover\"],\"primary\":[\"position\",\"open_cover\",\"stop_cover\",\"close_cover\"],\"secondary\":[],\"quick\":\"position\"},{\"id\":\"air-purifier\",\"name\":\"空气净化器\",\"types\":[\"air-purifier\",\"air-fresh\"],\"domains\":[\"fan\"],\"primary\":[\"percentage\",\"preset_mode\"],\"secondary\":[],\"quick\":\"percentage\",\"reading\":\"pm25\",\"readings\":[[\"pm2.5-density\",\"PM2.5\",\"ug/m3\"],[\"filter-life-level\",\"滤芯余量\",\"%\"]]},{\"id\":\"humidifier\",\"name\":\"加湿器\",\"types\":[\"humidifier\",\"diffuser\"],\"domains\":[\"humidifier\"],\"primary\":[\"humidity\",\"mode\"],\"secondary\":[],\"quick\":\"humidity\",\"reading\":\"current_humidity\"},{\"id\":\"speaker\",\"name\":\"音箱\",\"types\":[\"speaker\"],\"domains\":[\"media_player\"],\"primary\":[\"volume_level\",\"media_play\",\"media_pause\"],\"secondary\":[\"is_volume_muted\",\"source\",\"media_previous_track\",\"media_next_track\",\"media_stop\"],\"quick\":\"volume_level\"},{\"id\":\"vacuum\",\"name\":\"扫地机器人\",\"types\":[\"vacuum\"],\"domains\":[\"vacuum\"],\"primary\":[\"start\",\"pause\",\"return_to_base\"],\"secondary\":[\"fan_speed\",\"stop\"],\"quick\":\"actions\"},{\"id\":\"water-heater\",\"name\":\"热水器\",\"types\":[\"water-heater\"],\"domains\":[\"water_heater\"],\"primary\":[\"temperature\",\"operation_mode\"],\"secondary\":[],\"quick\":\"temperature\",\"reading\":\"current_temperature\"},{\"id\":\"temperature-humidity\",\"name\":\"温湿度传感器\",\"types\":[\"temperature-humidity-sensor\"],\"domains\":[\"sensor\"],\"primary\":[],\"secondary\":[],\"quick\":\"reading\",\"readonly\":true},{\"id\":\"air-fryer\",\"name\":\"空气炸锅\",\"types\":[\"air-fryer\"],\"domains\":[],\"layout\":\"appliance\",\"primary\":[],\"secondary\":[],\"quick\":\"reading\",\"readings\":[[\"$state\",\"烹饪状态\",\"\"],[\"left-time\",\"剩余时间\",\"\"],[\"temperature\",\"温度\",\"°C\"]]},{\"id\":\"air-monitor\",\"name\":\"空气质量监测\",\"types\":[\"air-monitor\"],\"domains\":[\"sensor\"],\"layout\":\"metrics\",\"primary\":[],\"secondary\":[],\"quick\":\"reading\",\"readonly\":true,\"readings\":[[\"pm2.5-density\",\"PM2.5\",\"µg/m³\"],[\"co2-density\",\"二氧化碳\",\"ppm\"],[\"hcho-density\",\"甲醛\",\"mg/m³\"],[\"tvoc-density\",\"TVOC\",\"mg/m³\"],[\"temperature\",\"温度\",\"°C\"],[\"relative-humidity\",\"湿度\",\"%\"]]},{\"id\":\"bath-heater\",\"name\":\"浴霸\",\"types\":[\"bath-heater\"],\"domains\":[\"climate\",\"switch\"],\"layout\":\"climate\",\"primary\":[\"temperature\",\"hvac_mode\"],\"secondary\":[\"fan_mode\",\"preset_mode\"],\"quick\":\"temperature\",\"readings\":[[\"temperature\",\"当前温度\",\"°C\"],[\"$state\",\"运行状态\",\"\"]]},{\"id\":\"camera\",\"name\":\"摄像头\",\"types\":[\"camera\"],\"domains\":[\"camera\",\"switch\"],\"layout\":\"camera\",\"primary\":[],\"secondary\":[],\"quick\":\"reading\",\"notice\":\"暂无预览画面\",\"readings\":[[\"$state\",\"设备状态\",\"\"],[\"battery-level\",\"电量\",\"%\"]]},{\"id\":\"coffee-machine\",\"name\":\"咖啡机\",\"types\":[\"coffee-machine\"],\"domains\":[],\"layout\":\"appliance\",\"primary\":[],\"secondary\":[],\"quick\":\"reading\",\"readings\":[[\"$state\",\"制作状态\",\"\"],[\"left-time\",\"剩余时间\",\"\"],[\"water-level\",\"水量\",\"%\"]]},{\"id\":\"cooker\",\"name\":\"电饭煲\",\"types\":[\"cooker\"],\"domains\":[],\"layout\":\"appliance\",\"primary\":[],\"secondary\":[],\"quick\":\"reading\",\"readings\":[[\"$state\",\"烹饪状态\",\"\"],[\"left-time\",\"剩余时间\",\"\"],[\"temperature\",\"温度\",\"°C\"]]},{\"id\":\"dishwasher\",\"name\":\"洗碗机\",\"types\":[\"dishwasher\"],\"domains\":[],\"layout\":\"appliance\",\"primary\":[],\"secondary\":[],\"quick\":\"reading\",\"readings\":[[\"$state\",\"洗涤状态\",\"\"],[\"left-time\",\"剩余时间\",\"\"],[\"temperature\",\"水温\",\"°C\"]]},{\"id\":\"flowerpot\",\"name\":\"智能花盆\",\"types\":[\"flowerpot\"],\"domains\":[\"sensor\"],\"layout\":\"metrics\",\"primary\":[],\"secondary\":[],\"quick\":\"reading\",\"readonly\":true,\"readings\":[[\"relative-humidity\",\"土壤湿度\",\"%\"],[\"illumination\",\"光照\",\"lx\"],[\"soil-ec\",\"土壤电导率\",\"µS/cm\"],[\"temperature\",\"温度\",\"°C\"]]},{\"id\":\"fridge\",\"name\":\"冰箱\",\"types\":[\"fridge\"],\"domains\":[],\"layout\":\"metrics\",\"primary\":[\"temperature\",\"option\"],\"secondary\":[],\"quick\":\"reading\",\"readings\":[[\"temperature\",\"冷藏温度\",\"°C\"],[\"freezer-temperature\",\"冷冻温度\",\"°C\"],[\"mode\",\"运行模式\",\"\"]]},{\"id\":\"fruit-vegetable-purifier\",\"name\":\"果蔬净洗机\",\"types\":[\"fruit-vegetable-purifier\"],\"domains\":[],\"layout\":\"appliance\",\"primary\":[],\"secondary\":[],\"quick\":\"reading\",\"readings\":[[\"$state\",\"净洗状态\",\"\"],[\"left-time\",\"剩余时间\",\"\"]]},{\"id\":\"garbage-can\",\"name\":\"智能垃圾桶\",\"types\":[\"garbage-can\"],\"domains\":[],\"layout\":\"appliance\",\"primary\":[],\"secondary\":[],\"quick\":\"reading\",\"readings\":[[\"$state\",\"运行状态\",\"\"],[\"target-lid-state\",\"桶盖状态\",\"\"],[\"battery-level\",\"电量\",\"%\"]]},{\"id\":\"gas-concentration\",\"name\":\"燃气浓度\",\"types\":[\"gas-concentration\"],\"domains\":[\"sensor\"],\"layout\":\"metrics\",\"primary\":[],\"secondary\":[],\"quick\":\"reading\",\"readonly\":true,\"readings\":[[\"$value\",\"燃气浓度\",\"\"],[\"battery-level\",\"电量\",\"%\"]]},{\"id\":\"gas-sensor\",\"name\":\"燃气报警器\",\"types\":[\"gas-sensor\"],\"domains\":[\"binary_sensor\",\"sensor\"],\"layout\":\"alarm\",\"primary\":[],\"secondary\":[],\"quick\":\"reading\",\"readonly\":true,\"states\":[\"未触发报警\",\"检测到燃气\"],\"readings\":[[\"$state\",\"报警状态\",\"\"],[\"battery-level\",\"电量\",\"%\"]]},{\"id\":\"gateway\",\"name\":\"网关\",\"types\":[\"gateway\"],\"domains\":[],\"layout\":\"status\",\"primary\":[],\"secondary\":[],\"quick\":\"reading\",\"readings\":[[\"$state\",\"设备状态\",\"\"],[\"guard-mode\",\"警戒模式\",\"\"]]},{\"id\":\"health-pot\",\"name\":\"养生壶\",\"types\":[\"health-pot\"],\"domains\":[],\"layout\":\"appliance\",\"primary\":[\"temperature\"],\"secondary\":[],\"quick\":\"reading\",\"readings\":[[\"temperature\",\"水温\",\"°C\"],[\"$state\",\"运行状态\",\"\"],[\"left-time\",\"剩余时间\",\"\"]]},{\"id\":\"heater\",\"name\":\"取暖器\",\"types\":[\"heater\"],\"domains\":[\"climate\",\"switch\"],\"layout\":\"climate\",\"primary\":[\"temperature\",\"hvac_mode\"],\"secondary\":[\"preset_mode\"],\"quick\":\"temperature\",\"readings\":[[\"temperature\",\"当前温度\",\"°C\"],[\"$state\",\"运行状态\",\"\"]]},{\"id\":\"hood\",\"name\":\"油烟机\",\"types\":[\"hood\"],\"domains\":[\"fan\",\"switch\"],\"layout\":\"status\",\"primary\":[\"percentage\",\"preset_mode\"],\"secondary\":[],\"quick\":\"percentage\",\"readings\":[[\"$state\",\"运行状态\",\"\"],[\"mode\",\"工作模式\",\"\"]]},{\"id\":\"illumination-sensor\",\"name\":\"光照传感器\",\"types\":[\"illumination-sensor\"],\"domains\":[\"sensor\"],\"layout\":\"metrics\",\"primary\":[],\"secondary\":[],\"quick\":\"reading\",\"readonly\":true,\"readings\":[[\"illumination\",\"光照度\",\"lx\"],[\"battery-level\",\"电量\",\"%\"]]},{\"id\":\"induction-cooker\",\"name\":\"电磁炉\",\"types\":[\"induction-cooker\"],\"domains\":[],\"layout\":\"appliance\",\"primary\":[],\"secondary\":[],\"quick\":\"reading\",\"readings\":[[\"$state\",\"烹饪状态\",\"\"],[\"left-time\",\"剩余时间\",\"\"]]},{\"id\":\"integrated-stove\",\"name\":\"集成灶\",\"types\":[\"integrated-stove\"],\"domains\":[\"fan\",\"switch\"],\"layout\":\"appliance\",\"primary\":[\"percentage\",\"preset_mode\"],\"secondary\":[],\"quick\":\"percentage\",\"readings\":[[\"mode\",\"工作模式\",\"\"],[\"$state\",\"运行状态\",\"\"]]},{\"id\":\"juicer\",\"name\":\"料理机\",\"types\":[\"juicer\"],\"domains\":[],\"layout\":\"appliance\",\"primary\":[],\"secondary\":[],\"quick\":\"reading\",\"readings\":[[\"$state\",\"运行状态\",\"\"],[\"left-time\",\"剩余时间\",\"\"]]},{\"id\":\"kettle\",\"name\":\"电水壶\",\"types\":[\"kettle\"],\"domains\":[],\"layout\":\"appliance\",\"primary\":[\"temperature\"],\"secondary\":[],\"quick\":\"reading\",\"readings\":[[\"temperature\",\"当前水温\",\"°C\"],[\"target-temperature\",\"目标水温\",\"°C\"],[\"$state\",\"运行状态\",\"\"]]},{\"id\":\"magnet-sensor\",\"name\":\"门窗传感器\",\"types\":[\"magnet-sensor\"],\"domains\":[\"binary_sensor\",\"sensor\"],\"layout\":\"status\",\"primary\":[],\"secondary\":[],\"quick\":\"reading\",\"readonly\":true,\"states\":[\"门窗已关闭\",\"门窗已打开\"],\"readings\":[[\"$state\",\"门窗状态\",\"\"],[\"battery-level\",\"电量\",\"%\"]]},{\"id\":\"massage\",\"name\":\"按摩设备\",\"types\":[\"massage\"],\"domains\":[],\"layout\":\"status\",\"primary\":[],\"secondary\":[],\"quick\":\"reading\",\"readings\":[[\"$state\",\"运行状态\",\"\"],[\"left-time\",\"剩余时间\",\"\"]]},{\"id\":\"mirror\",\"name\":\"智能镜\",\"types\":[\"mirror\"],\"domains\":[\"light\",\"switch\"],\"layout\":\"status\",\"primary\":[\"brightness_pct\",\"color_temp_kelvin\"],\"secondary\":[\"effect\"],\"quick\":\"brightness_pct\",\"readings\":[[\"$state\",\"照明状态\",\"\"]]},{\"id\":\"mosquito-dispeller\",\"name\":\"驱蚊器\",\"types\":[\"mosquito-dispeller\"],\"domains\":[],\"layout\":\"status\",\"primary\":[],\"secondary\":[],\"quick\":\"reading\",\"readings\":[[\"repellent-left-level\",\"耗材余量\",\"%\"],[\"battery-level\",\"电量\",\"%\"],[\"$state\",\"运行状态\",\"\"]]},{\"id\":\"motion-sensor\",\"name\":\"人体传感器\",\"types\":[\"motion-sensor\"],\"domains\":[\"binary_sensor\",\"sensor\"],\"layout\":\"status\",\"primary\":[],\"secondary\":[],\"quick\":\"reading\",\"readonly\":true,\"states\":[\"未检测到活动\",\"检测到活动\"],\"readings\":[[\"$state\",\"活动状态\",\"\"],[\"battery-level\",\"电量\",\"%\"]]},{\"id\":\"moxibustion\",\"name\":\"艾灸设备\",\"types\":[\"moxibustion\"],\"domains\":[],\"layout\":\"status\",\"primary\":[],\"secondary\":[],\"quick\":\"reading\",\"readings\":[[\"$state\",\"运行状态\",\"\"],[\"temperature\",\"温度\",\"°C\"]]},{\"id\":\"oven\",\"name\":\"烤箱\",\"types\":[\"oven\"],\"domains\":[],\"layout\":\"appliance\",\"primary\":[\"temperature\"],\"secondary\":[],\"quick\":\"reading\",\"readings\":[[\"$state\",\"烹饪状态\",\"\"],[\"temperature\",\"当前温度\",\"°C\"],[\"left-time\",\"剩余时间\",\"\"]]},{\"id\":\"pet-drinking-fountain\",\"name\":\"宠物饮水机\",\"types\":[\"pet-drinking-fountain\"],\"domains\":[\"switch\"],\"layout\":\"status\",\"primary\":[],\"secondary\":[],\"quick\":\"power\",\"readings\":[[\"$state\",\"供水状态\",\"\"],[\"water-level\",\"水量\",\"%\"],[\"filter-life-level\",\"滤芯余量\",\"%\"]]},{\"id\":\"plant-monitor\",\"name\":\"植物监测仪\",\"types\":[\"plant-monitor\"],\"domains\":[\"sensor\"],\"layout\":\"metrics\",\"primary\":[],\"secondary\":[],\"quick\":\"reading\",\"readonly\":true,\"readings\":[[\"relative-humidity\",\"土壤湿度\",\"%\"],[\"illumination\",\"光照\",\"lx\"],[\"soil-ec\",\"土壤电导率\",\"µS/cm\"],[\"temperature\",\"温度\",\"°C\"]]},{\"id\":\"pressure-cooker\",\"name\":\"电压力锅\",\"types\":[\"pressure-cooker\"],\"domains\":[],\"layout\":\"appliance\",\"primary\":[],\"secondary\":[],\"quick\":\"reading\",\"readings\":[[\"$state\",\"烹饪状态\",\"\"],[\"left-time\",\"剩余时间\",\"\"]]},{\"id\":\"printer\",\"name\":\"打印机\",\"types\":[\"printer\"],\"domains\":[],\"layout\":\"appliance\",\"primary\":[],\"secondary\":[],\"quick\":\"reading\",\"readings\":[[\"$state\",\"打印状态\",\"\"],[\"progress\",\"任务进度\",\"%\"]]},{\"id\":\"smoke-sensor\",\"name\":\"烟雾报警器\",\"types\":[\"smoke-sensor\"],\"domains\":[\"binary_sensor\",\"sensor\"],\"layout\":\"alarm\",\"primary\":[],\"secondary\":[],\"quick\":\"reading\",\"readonly\":true,\"states\":[\"未触发报警\",\"检测到烟雾\"],\"readings\":[[\"$state\",\"报警状态\",\"\"],[\"battery-level\",\"电量\",\"%\"]]},{\"id\":\"submersion-sensor\",\"name\":\"水浸传感器\",\"types\":[\"submersion-sensor\"],\"domains\":[\"binary_sensor\",\"sensor\"],\"layout\":\"alarm\",\"primary\":[],\"secondary\":[],\"quick\":\"reading\",\"readonly\":true,\"states\":[\"未检测到漏水\",\"检测到漏水\"],\"readings\":[[\"$state\",\"水浸状态\",\"\"],[\"battery-level\",\"电量\",\"%\"]]},{\"id\":\"towel-rack\",\"name\":\"电热毛巾架\",\"types\":[\"towel-rack\"],\"domains\":[\"climate\",\"switch\"],\"layout\":\"climate\",\"primary\":[\"temperature\"],\"secondary\":[],\"quick\":\"temperature\",\"readings\":[[\"$state\",\"运行状态\",\"\"],[\"temperature\",\"温度\",\"°C\"]]},{\"id\":\"treadmill\",\"name\":\"跑步机\",\"types\":[\"treadmill\"],\"domains\":[],\"layout\":\"metrics\",\"primary\":[],\"secondary\":[],\"quick\":\"reading\",\"readings\":[[\"current-distance\",\"运动距离\",\"km\"],[\"working-time\",\"运动时长\",\"min\"],[\"speed\",\"当前速度\",\"km/h\"]]},{\"id\":\"video-doorbell\",\"name\":\"可视门铃\",\"types\":[\"video-doorbell\"],\"domains\":[\"camera\",\"binary_sensor\"],\"layout\":\"camera\",\"primary\":[],\"secondary\":[],\"quick\":\"reading\",\"notice\":\"暂无预览画面\",\"readings\":[[\"$state\",\"设备状态\",\"\"],[\"battery-level\",\"电量\",\"%\"]]},{\"id\":\"walking-pad\",\"name\":\"走步机\",\"types\":[\"walking-pad\"],\"domains\":[],\"layout\":\"metrics\",\"primary\":[],\"secondary\":[],\"quick\":\"reading\",\"readings\":[[\"current-distance\",\"运动距离\",\"km\"],[\"working-time\",\"运动时长\",\"min\"],[\"speed\",\"当前速度\",\"km/h\"]]},{\"id\":\"washer\",\"name\":\"洗衣机\",\"types\":[\"washer\"],\"domains\":[],\"layout\":\"appliance\",\"primary\":[],\"secondary\":[],\"quick\":\"reading\",\"readings\":[[\"$state\",\"洗涤状态\",\"\"],[\"left-time\",\"剩余时间\",\"\"],[\"progress\",\"任务进度\",\"%\"]]},{\"id\":\"water-dispenser\",\"name\":\"饮水机\",\"types\":[\"water-dispenser\"],\"domains\":[],\"layout\":\"appliance\",\"primary\":[\"temperature\"],\"secondary\":[],\"quick\":\"reading\",\"readings\":[[\"temperature\",\"当前水温\",\"°C\"],[\"target-temperature\",\"目标水温\",\"°C\"],[\"water-level\",\"水量\",\"%\"]]},{\"id\":\"water-purifier\",\"name\":\"净水器\",\"types\":[\"water-purifier\"],\"domains\":[],\"layout\":\"metrics\",\"primary\":[],\"secondary\":[],\"quick\":\"reading\",\"readings\":[[\"tds-out\",\"出水 TDS\",\"ppm\"],[\"filter-life-level\",\"滤芯余量\",\"%\"],[\"$state\",\"运行状态\",\"\"]]}]");
  /* HA_COMMON_PROFILES_END */
  const commonProfileTypes=Object.create(null);
  commonCardProfiles.forEach(profile=>profile.types.forEach(type=>commonProfileTypes[type]=profile));
  function commonCardProfile(item) {
    if(!item)return null;
    const profile=commonProfileTypes[quickCardType(item)];
    return profile&&(profile.layout||profile.domains.includes(item.domain))?profile:null;
  }
  function profileControls(item) {
    if(item&&item.product_readonly)return [];
    const profile=commonCardProfile(item),main=lookupEntity(item.entity_id)||item;
    if(profile&&profile.readonly)return [];
    const specs=functionControls(main);
    if(!profile)return specs;
    const order=profile.primary.concat(profile.secondary);
    return specs.map((spec,index)=>({spec,index,rank:order.indexOf(spec.key||spec.service)}))
      .sort((a,b)=>(a.rank<0?100:a.rank)-(b.rank<0?100:b.rank)||a.index-b.index).map(row=>row.spec);
  }
  function profileQuickControl(item) {
    const profile=commonCardProfile(item);
    return profile&&profileControls(item).find(spec=>spec.kind==='range'&&spec.key===profile.quick)||null;
  }
  function profileReadings(item) {
    const profile=commonCardProfile(item);
    if(!profile||!profile.readings)return [];
    const type=quickCardType(item),values=item.quick_values||{},online=available(item);
    const members=(item.device_entities||[item.entity_id]).map(id=>indexedEntity(id)).filter(Boolean);
    const classes={temperature:'temperature',humidity:'relative-humidity',moisture:'relative-humidity',
      illuminance:'illumination',conductivity:'soil-ec',pm25:'pm2.5-density',
      distance:'current-distance',duration:'working-time',speed:'speed',
      carbon_dioxide:'co2-density',formaldehyde:'hcho-density',volatile_organic_compounds:'tvoc-density',battery:'battery-level'};
    return profile.readings.flatMap(([key,title,unit])=>{
      let raw,source=item.entity_id,alert=false;
      if(key==='$state'){
        const property=['contact-state','motion-state','submersion-state','status'].map(name=>type+'.'+name).find(name=>values[name]!==undefined);
        const status=property?values[property]:item.state;
        const active=status===true||['on','detected','alarm','triggered','open'].includes(String(status));
        const inactive=status===false||['off','clear','normal','closed'].includes(String(status));
        raw=profile.states&&(active||inactive)?tr(profile.states[active?1:0]):
          item.domain==='binary_sensor'&&!profile.states?binaryState(item):friendlyState({state:String(status)});
        alert=online&&profile.layout==='alarm'&&active;
      }else if(key==='$value'){
        if(item.domain!=='sensor'||!isNumeric(item))return [];
        raw=Number(item.state);unit=item.unit;
      }else{
        const exact=[type+'.'+key,'environment.'+key];
        const property=exact.find(k=>values[k]!==undefined)||Object.keys(values).find(k=>k.endsWith('.'+key));
        raw=property&&values[property];
        if(raw===undefined||raw===null||Array.isArray(raw))return [];
        const member=members.find(row=>row.domain==='sensor'&&classes[row.device_class]===key&&
          Number.isFinite(Number(row.state))&&Number(row.state)===raw);
        if(member){source=member.entity_id;if(member.unit)unit=member.unit;}
        if(['temperature','target-temperature','freezer-temperature'].includes(key)&&
           values[type+'.temperature-unit'])unit=String(values[type+'.temperature-unit']);
      }
      const numeric=typeof raw==='number'&&Number.isFinite(raw);
      // The embedded font omits micro/superscript glyphs. Keep the same units
      // in their plain-text spelling instead of rendering missing-glyph boxes.
      unit=String(unit).replace(/[µμ]/g,'u').replace(/³/g,'3').replace(/²/g,'2').replace(/℃/g,'°C').replace(/℉/g,'°F');
      let value=numeric?(Math.abs(raw)>0&&Math.abs(raw)<1?String(Number(raw.toPrecision(3))):numberText(raw)):typeof raw==='boolean'?(raw?tr('已开启'):tr('已关闭')):
        friendlyState({state:String(raw)});
      if(!online)value='--';
      return [{key,title:tr(title),value,unit:numeric&&online?unit:'',numeric:numeric&&online,
        display:value+(numeric&&online&&unit?(unit==='%'?'':' ')+unit:''),source,alert}];
    });
  }
  /* MI_QUICK_RULES_BEGIN */
  const quickReferenceRules = JSON.parse("{\"diffuser\":[[1,1,872,1000,[\"diffuser.on\",\"light.on\",\"diffuser.Help Sleep\",\"diffuser.Week Up\",\"diffuser.Cheerful\",\"diffuser.Sport Mode\"]]],\"air-conditioner\":[[1,2,1000,854,[\"air-conditioner.on\",\"air-conditioner.target-temperature\",\"air-conditioner.mode\"]]],\"air-fresh\":[[1,3,524,540,[\"air-fresh.on\",\"environment.pm2.5-density\",\"air-fresh.mode\"]],[2,4,536,354,[\"air-fresh.on\"]]],\"air-fryer\":[[1,5,806,532,[\"air-fryer.status\"]]],\"air-monitor\":[[1,6,776,512,[\"environment.hcho-density\",\"environment.temperature\",\"environment.relative-humidity\"]],[2,6,776,512,[\"environment.pm2.5-density\",\"environment.temperature\",\"environment.relative-humidity\"]],[3,6,776,512,[\"environment.tvoc-density\",\"environment.temperature\",\"environment.relative-humidity\"]],[4,7,536,354,[\"environment.temperature\",\"environment.relative-humidity\"]],[5,8,536,354,[\"environment.pm2.5-density\"]],[6,8,536,354,[\"environment.hcho-density\"]]],\"air-purifier\":[[1,3,524,540,[\"air-purifier.on\",\"environment.pm2.5-density\",\"air-purifier.mode\"]],[2,3,524,540,[\"air-purifier.on\",\"environment.pm2.5-density\",\"air-purifier.fan-level\"]],[3,4,536,354,[\"air-purifier.on\"]]],\"airer\":[[1,9,390,320,[\"airer.motor-control\"]]],\"bath-heater\":[[1,10,682,772,[\"light.on\",\"ptc-bath-heater.ventilation\",\"ptc-bath-heater.heating\",\"ptc-bath-heater.target-temperature\"]],[2,3,524,540,[\"light-bath-heater.on\",\"ptc-bath-heater.status\",\"light-bath-heater.mode\"]],[3,3,524,540,[\"ptc-bath-heater.on\",\"ptc-bath-heater.status\",\"ptc-bath-heater.mode\"]],[4,11,886,774,[\"ptc-bath-heater.on\",\"ptc-bath-heater.temperature\",\"ptc-bath-heater.target-temperature\"]],[5,12,860,614,[\"ptc-bath-heater.on\",\"light.on\",\"fan-control.on\"]],[6,12,860,614,[\"light-bath-heater.on\",\"light.on\",\"fan-control.on\"]],[7,13,540,438,[\"ptc-bath-heater.on\",\"light.on\"]],[8,13,540,438,[\"light-bath-heater.on\",\"light.on\"]],[9,13,540,438,[\"light.on\",\"fan-control.on\"]],[10,4,536,354,[\"light.on\"]]],\"camera\":[[1,14,824,664,[\"camera-control.on\",\"p2p-stream.start-p2p-stream\"]],[2,15,824,662,[\"p2p-stream.start-p2p-stream\"]],[3,4,536,354,[\"camera-control.on\"]]],\"ceiling-fan\":[[1,16,536,354,[\"ceiling-fan.on\",\"ceiling-fan.horizontal-swing\",\"ceiling-fan.fan-level\"]],[2,4,536,354,[\"ceiling-fan.on\"]]],\"coffee-machine\":[[1,5,806,532,[\"coffee-machine.status\"]]],\"cooker\":[[1,5,806,532,[\"cooker.status\"]]],\"curtain\":[[1,3,524,540,[\"curtain.status\",\"curtain.motor-control\"]],[2,3,524,540,[\"curtain.current-position\",\"curtain.motor-control\"]],[3,9,390,320,[\"curtain.motor-control\"]]],\"dishwasher\":[[1,5,806,532,[\"dishwasher.status\"]]],\"fan\":[[1,16,536,354,[\"fan.on\",\"fan.horizontal-swing\",\"fan.fan-level\"]],[2,4,536,354,[\"fan.on\"]]],\"flowerpot\":[[1,6,776,512,[\"plant-monitor.relative-humidity\",\"plant-monitor.illumination\",\"plant-monitor.soil-ec\"]],[2,6,776,512,[\"plant-monitor.relative-humidity\",\"plant-monitor.temperature\",\"plant-monitor.illumination\"]],[3,7,536,354,[\"plant-monitor.relative-humidity\",\"plant-monitor.soil-ec\"]],[4,7,536,354,[\"plant-monitor.relative-humidity\",\"plant-monitor.illumination\"]]],\"fridge\":[[1,5,806,532,[\"fridge.mode\"]]],\"fruit-vegetable-purifier\":[[1,5,806,532,[\"fruit-vegetable-purifier.status\"]]],\"garbage-can\":[[1,17,1000,922,[\"battery.battery-level\",\"garbage-can.target-lid-state\",\"garbage-can.status\",\"garbage-can.package\"]]],\"gas-concentration\":[[1,5,806,532,[\"gas-concentration.status\"]]],\"gas-sensor\":[[1,5,806,532,[\"gas-sensor.status\"]]],\"gateway\":[[1,13,540,438,[\"light.on\",\"gateway.guard-mode\"]],[2,4,536,354,[\"gateway.guard-mode\"]],[3,5,806,532,[\"arming.arming-mode\"]],[4,18,536,354,[\"light.on\",\"light.brightness\"]]],\"health-pot\":[[1,5,806,532,[\"health-pot.status\"]]],\"heater\":[[1,11,886,774,[\"heater.on\",\"environment.temperature\",\"heater.target-temperature\"]],[2,4,536,354,[\"heater.on\"]]],\"hood\":[[1,12,860,614,[\"hood.on\",\"light.on\",\"fan-control.on\"]],[2,13,540,438,[\"hood.on\",\"light.on\"]],[3,4,536,354,[\"hood.on\"]]],\"humidifier\":[[1,19,1000,872,[\"humidifier.on\",\"environment.relative-humidity\",\"humidifier.fan-level\"]],[2,3,524,540,[\"humidifier.on\",\"environment.relative-humidity\",\"humidifier.mode\"]],[3,4,536,354,[\"humidifier.on\"]]],\"illumination-sensor\":[[1,8,536,354,[\"illumination-sensor.illumination\"]]],\"induction-cooker\":[[1,7,536,354,[\"induction-cooker.status\",\"induction-cooker.left-time\"]],[2,5,806,532,[\"induction-cooker.status\"]]],\"integrated-stove\":[[1,7,536,354,[\"integrated-stove.mode\",\"hood.mode\"]],[2,4,536,354,[\"light.on\"]]],\"juicer\":[[1,7,536,354,[\"juicer.status\",\"juicer.left-time\"]],[2,5,806,532,[\"juicer.status\"]]],\"kettle\":[[1,7,536,354,[\"kettle.temperature\",\"kettle.target-temperature\"]],[2,8,536,354,[\"kettle.temperature\"]]],\"light\":[[1,20,682,526,[\"light.on\",\"light.brightness\",\"fan.on\",\"fan.fan-level\"]],[2,21,392,358,[\"light.on\",\"light.brightness\",\"light.color-temperature\"]],[3,18,536,354,[\"light.on\",\"light.brightness\"]],[4,4,536,354,[\"light.on\"]]],\"magnet-sensor\":[[1,22,808,532,[\"magnet-sensor.contact-state\"]]],\"massage\":[[1,4,536,354,[\"massage.on\"]]],\"mirror\":[[1,21,392,358,[\"light.on\",\"light.brightness\",\"light.color-temperature\"]],[2,18,536,354,[\"light.on\",\"light.brightness\"]],[3,4,536,354,[\"light.on\"]]],\"mosquito-dispeller\":[[1,7,536,354,[\"repellent.repellent-left-level\",\"battery.battery-level\"]],[2,8,536,354,[\"repellent.repellent-left-level\"]],[3,4,536,354,[\"mosquito-dispeller.on\"]]],\"motion-sensor\":[[1,22,808,532,[\"motion-sensor.motion-state\"]]],\"moxibustion\":[[1,4,536,354,[\"moxibustion.on\"]]],\"outlet\":[[1,12,860,614,[\"switch.on\",\"switch.on\",\"switch.on\"]],[2,13,540,438,[\"switch.on\",\"switch.on\"]],[3,4,536,354,[\"switch.on\"]]],\"oven\":[[1,5,806,532,[\"oven.status\"]]],\"pet-drinking-fountain\":[[1,4,536,354,[\"pet-drinking-fountain.on\"]]],\"plant-monitor\":[[1,6,776,512,[\"plant-monitor.relative-humidity\",\"plant-monitor.illumination\",\"plant-monitor.soil-ec\"]],[2,7,536,354,[\"plant-monitor.relative-humidity\",\"plant-monitor.soil-ec\"]]],\"pressure-cooker\":[[1,7,536,354,[\"pressure-cooker.status\",\"pressure-cooker.left-time\"]],[2,5,806,532,[\"pressure-cooker.status\"]]],\"printer\":[[1,5,806,532,[\"printer.status\"]]],\"relay\":[[1,12,860,614,[\"switch.on\",\"switch.on\",\"switch.on\"]],[2,13,540,438,[\"switch.on\",\"switch.on\"]],[3,4,536,354,[\"switch.on\"]]],\"smoke-sensor\":[[1,5,806,532,[\"smoke-sensor.status\"]]],\"speaker\":[[1,23,946,1000,[\"speaker.mute\",\"microphone.mute\",\"speaker.volume\"]],[2,24,946,1000,[\"play-control.playing-state\",\"play-control.play\",\"play-control.pause\",\"microphone.mute\",\"speaker.volume\"]]],\"submersion-sensor\":[[1,22,808,532,[\"submersion-sensor.submersion-state\"]]],\"switch\":[[1,12,860,614,[\"switch.on\",\"switch.on\",\"switch.on\"]],[2,13,540,438,[\"switch.on\",\"switch.on\"]],[3,4,536,354,[\"switch.on\"]]],\"temperature-humidity-sensor\":[[1,6,776,512,[\"temperature-humidity-sensor.temperature\",\"temperature-humidity-sensor.relative-humidity\",\"temperature-humidity-sensor.atmospheric-pressure\"]],[2,7,536,354,[\"temperature-humidity-sensor.temperature\",\"temperature-humidity-sensor.relative-humidity\"]]],\"towel-rack\":[[1,4,536,354,[\"towel-rack.on\"]]],\"treadmill\":[[1,7,536,354,[\"treadmill.working-time\",\"treadmill.current-distance\"]]],\"vacuum\":[[1,25,524,540,[\"vacuum.status\",\"vacuum.status\",\"vacuum.start-sweep\",\"vacuum.pause-sweeping\",\"vacuum.status\",\"battery.start-charge\"]],[2,25,524,540,[\"vacuum.status\",\"vacuum.status\",\"vacuum.start-sweep\",\"vacuum.pause\",\"vacuum.status\",\"vacuum.start-charge\"]],[3,25,524,540,[\"vacuum.status\",\"vacuum.status\",\"vacuum.start-sweep\",\"vacuum.stop-sweeping\",\"battery.charging-state\",\"battery.start-charge\"]],[4,25,524,540,[\"vacuum.status\",\"vacuum.status\",\"vacuum.start-sweep\",\"vacuum.stop-sweeping\",\"vacuum.status\",\"battery.start-charge\"]],[5,25,524,540,[\"vacuum.status\",\"vacuum.status\",\"vacuum.start-sweep\",\"vacuum.stop-sweeping\",\"vacuum.status\",\"vacuum.start-charge\"]],[6,5,806,532,[\"vacuum.status\"]]],\"video-doorbell\":[[1,22,808,532,[\"motion-detection.motion-detection\"]]],\"walking-pad\":[[1,7,536,354,[\"walking-pad.working-time\",\"walking-pad.current-distance\"]]],\"washer\":[[1,5,806,532,[\"washer.status\"]],[2,5,806,532,[\"washer.mode\"]]],\"water-dispenser\":[[1,7,536,354,[\"water-dispenser.temperature\",\"water-dispenser.target-temperature\"]],[2,8,536,354,[\"water-dispenser.temperature\"]]],\"water-heater\":[[1,4,536,354,[\"water-heater.on\"]],[2,7,536,354,[\"water-heater.temperature\",\"water-heater.status\"]],[3,7,536,354,[\"water-heater.target-temperature\",\"water-heater.status\"]],[4,7,536,354,[\"water-heater.target-temperature\",\"water-heater.temperature\"]],[5,5,806,532,[\"water-heater.status\"]],[6,8,536,354,[\"water-heater.temperature\"]]],\"water-purifier\":[[1,8,536,354,[\"tds-sensor.tds-out\"]],[2,5,806,532,[\"water-purifier.status\"]]]}");
  /* MI_QUICK_RULES_END */
  const thumbnailCodes = {lamp:0xe015,plug:0xe016,fan:0xe017,sensor:0xe018,
    contact:0xe019,climate:0xe01a,media:0xe01b,generic:0xe01c,purifier:0xe01d,
    curtain:0xe027,vacuum:0xe028,camera:0xe029,humidifier:0xe02a,heater:0xe02b,
    kitchen:0xe02c,washer:0xe02d,plant:0xe02e,lock:0xe02f};
  function quickCardType(item) {
    const declaredType = String(item.card_type || '');
    const urn = declaredType.match(/^urn:miot-spec-v2:device:([a-z0-9-]+):/);
    const hint = urn ? urn[1] : declaredType.toLowerCase().replace(/_/g,'-');
    if (quickCardRegistry[hint]) return hint;
    if(hint==='lock'||item.domain==='lock')return 'lock';
    if (item.domain === 'fan') return item.icon === 'mdi:air-purifier' || /净化|purifier/i.test(item.name) ? 'air-purifier' : 'fan';
    if (item.domain === 'binary_sensor') return ({door:'magnet-sensor',window:'magnet-sensor',
      opening:'magnet-sensor',motion:'motion-sensor',occupancy:'motion-sensor',presence:'motion-sensor',
      smoke:'smoke-sensor',gas:'gas-sensor',moisture:'submersion-sensor'})[item.device_class] || '';
    if (item.domain === 'sensor') return ['pm25','pm10','carbon_dioxide','volatile_organic_compounds','formaldehyde'].includes(item.device_class) ? 'air-monitor' :
      item.device_class === 'illuminance' ? 'illumination-sensor' : ['temperature','humidity'].includes(item.device_class)||!item.device_class&&['C','°C','F','°F'].includes(item.unit)||/湿度|humidity/i.test(item.name)?'temperature-humidity-sensor':'';
    return ({light:'light',switch:'switch',input_boolean:'switch',climate:'air-conditioner',
      cover:'curtain',media_player:'speaker',camera:'camera',vacuum:'vacuum',
      humidifier:'humidifier',water_heater:'water-heater'})[item.domain] || '';
  }
  function quickCardModel(item) {
    const type = quickCardType(item), definition = quickCardRegistry[type] ||
      {family:type === 'lock' ? 'lock' : ['sensor','binary_sensor'].includes(item.domain) ? 'sensor' : 'generic',kind:item.domain==='sensor'?'sensor':'status'};
    let progress = null, value = '', subtitle = dashboardSubtitle(item), variant = definition.kind + '-status';
    if (available(item)) {
      if (definition.kind === 'light' && item.domain === 'light' && item.dimmable && item.brightness !== null) {
        progress = item.brightness; value = tr('亮度 ') + progress + '%'; subtitle = friendlyState(item); variant = 'light-brightness';
        if (item.color_temp_kelvin !== null) { subtitle = friendlyState(item) + ' · ' + item.color_temp_kelvin + 'K'; variant = 'light-color-temperature'; }
      } else if (['fan','air'].includes(definition.kind) && item.percentage !== null) {
        progress = item.percentage; value = tr('风速 ') + progress + '%'; variant = definition.kind + '-speed';
      } else if (definition.kind === 'position' && item.current_position !== null) {
        progress = item.current_position; value = tr('开合 ') + progress + '%'; variant = 'curtain-position';
      } else if (definition.kind === 'media' && item.volume_level !== null) {
        progress = Math.round(item.volume_level * 100); value = tr('音量 ') + progress + '%';
        subtitle = item.media_title || friendlyState(item); variant = 'speaker-volume';
      } else if (definition.kind === 'battery' && item.battery_level !== null) {
        value = item.battery_level + '%'; subtitle = friendlyState(item) + tr(' · 电量'); variant = 'vacuum-battery';
      } else if (definition.kind === 'humidity' && item.current_humidity !== null) {
        value = item.current_humidity + '%'; subtitle = friendlyState(item) + tr(' · 湿度'); variant = 'humidifier-humidity';
      } else if (['climate','temperature'].includes(definition.kind) && (item.current_temperature !== null || item.temperature !== null)) {
        value = numberText(item.current_temperature === null ? item.temperature : item.current_temperature) + ' °C'; variant = 'temperature-reading';
      } else if (['sensor','climate','position','media'].includes(definition.kind) || item.domain === 'weather') {
        value = dashboardValue(item); variant = definition.kind + '-reading';
      }
      if (definition.kind === 'air' && item.pm25 !== null) { value = 'PM2.5 ' + numberText(item.pm25); progress = null; variant = 'air-quality'; }
      if (['fan','air'].includes(definition.kind)) {
        subtitle = friendlyState(item) + (item.preset_mode ? ' · ' + item.preset_mode : item.oscillating ? tr(' · 摇头') : '');
      }
      if(type==='lock') {value=friendlyState(item);subtitle=item.battery_level===null?tr('门锁状态'):tr('电量 ')+numberText(item.battery_level)+'%';}
      if(item.device_id&&item.device_entities&&item.device_entities.length>1) {
        if(isTemperature(item)&&item.current_humidity!==null)subtitle=tr('湿度 ')+numberText(item.current_humidity)+'%';
        else if(isHumidity(item)&&item.current_temperature!==null)subtitle=tr('温度 ')+numberText(item.current_temperature)+' °C';
      }
    }
    const profile=commonCardProfile(item),readings=profileReadings(item);
    if(profile&&profile.layout){
      const main=readings[0];progress=null;
      value=main?main.display:dashboardValue(item);
      variant=main&&main.numeric?'profile-metric':'profile-status';
      subtitle=main?main.title:tr(profile.name);
      if(readings[1])subtitle+=' · '+readings[1].title+' '+readings[1].display;
      else if(profile.notice)subtitle=tr(profile.notice);
    }
    return {type, family:definition.family, variant, progress, value, subtitle,
      alert:readings.some(reading=>reading.alert)};
  }
  function thumbnail(item) { return String.fromCharCode(thumbnailCodes[quickCardModel(item).family]); }
  function thumbnailBase(item) { return String.fromCharCode(thumbnail(item).charCodeAt(0) + 9); }

  function quickPropertyValues(raw, attributes) {
    const domain=raw.entity_id.split('.')[0], values=Object.create(null);
    const type=quickCardType({domain,name:String(attributes.friendly_name||''),
      card_type:String(attributes.miot_type||attributes.device_type||''),
      icon:String(attributes.icon||''),unit:String(attributes.unit_of_measurement||''),device_class:String(attributes.device_class||'')});
    function put(key,value) {
      if(typeof value==='number'&&Number.isFinite(value))values[key]=value;
      else if(typeof value==='boolean')values[key]=value;
      else if(typeof value==='string'&&value.length<=80)values[key]=clean(value,80);
      else if(Array.isArray(value)) {const list=value.slice(0,4).filter(v=>typeof v==='boolean'||typeof v==='number'&&Number.isFinite(v)||typeof v==='string'&&v.length<=40);if(list.length)values[key]=list;}
    }
    if(attributes.miot_properties&&typeof attributes.miot_properties==='object'&&!Array.isArray(attributes.miot_properties)) {
      Object.keys(attributes.miot_properties).slice(0,32).forEach(key=>{if(/^[a-z0-9-]+\.[a-zA-Z0-9 .-]{1,48}$/.test(key))put(key,attributes.miot_properties[key]);});
      if(Array.isArray(attributes.miot_actions))attributes.miot_actions.slice(0,12).forEach(key=>{if(typeof key==='string'&&/^[a-z0-9-]+\.[a-zA-Z0-9 .-]{1,48}$/.test(key))put(key,true);});
      return values;
    }
    if(type) {put(type+'.status',raw.state);if(raw.state!=='unknown'&&raw.state!=='unavailable')put(type+'.on',raw.state!=='off');}
    if(['light','switch','input_boolean','fan'].includes(domain))put((domain==='input_boolean'?'switch':domain)+'.on',raw.state==='on');
    const aliases={temperature:'temperature',current_temperature:'temperature',
      current_humidity:'relative-humidity',humidity:'relative-humidity',pressure:'atmospheric-pressure',
      pm25:'pm2.5-density',pm2_5:'pm2.5-density',hcho:'hcho-density',tvoc:'tvoc-density',
      illuminance:'illumination',soil_ec:'soil-ec',battery_level:'battery-level',
      working_time:'working-time',current_distance:'current-distance',remaining_time:'left-time',
      water_level:'water-level',filter_life_level:'filter-life-level',filter_life_remaining:'filter-life-level',
      freezer_temperature:'freezer-temperature',progress:'progress',speed:'speed',
      repellent_left_level:'repellent-left-level',
      tds:'tds-out',preset_mode:'mode',percentage:'fan-level',oscillating:'horizontal-swing',
      current_position:'current-position',color_temp_kelvin:'color-temperature',
      volume_level:'volume',temperature_unit:'temperature-unit'};
    Object.keys(aliases).forEach(key=>{if(attributes[key]!==undefined) {put(type+'.'+aliases[key],attributes[key]);put('environment.'+aliases[key],attributes[key]);}});
    if(attributes.temperature!==undefined)put(type+'.target-temperature',attributes.temperature);
    if(brightnessPercent(attributes.brightness)!==null)put('light.brightness',raw.state==='off'?0:brightnessPercent(attributes.brightness));
    if(domain==='cover')put(type+'.motor-control',raw.state);
    if(domain==='climate')put(type+'.mode',raw.state);
    if(domain==='media_player') {put('play-control.playing-state',raw.state);put('speaker.volume',attributes.volume_level);if(typeof attributes.is_volume_muted==='boolean')put('speaker.mute',attributes.is_volume_muted);}
    if(domain==='sensor'&&raw.state!=='unknown'&&raw.state!=='unavailable') {
      const classKey={temperature:'temperature',humidity:'relative-humidity',pressure:'atmospheric-pressure',
        pm25:'pm2.5-density',pm10:'pm10-density',carbon_dioxide:'co2-density',
        volatile_organic_compounds:'tvoc-density',formaldehyde:'hcho-density',illuminance:'illumination',
        conductivity:'soil-ec',moisture:'relative-humidity',distance:'current-distance',duration:'working-time',speed:'speed',
        battery:'battery-level'}[attributes.device_class]||(!attributes.device_class&&['C','°C','F','°F'].includes(attributes.unit_of_measurement)?'temperature':/湿度|humidity/i.test(attributes.friendly_name||'')?'relative-humidity':null);
      if(classKey) {const reading=Number(raw.state);put(type+'.'+classKey,Number.isFinite(reading)?reading:raw.state);put('environment.'+classKey,Number.isFinite(reading)?reading:raw.state);}
    }
    if(domain==='binary_sensor')put(type+'.'+(type==='submersion-sensor'?'submersion-state':type==='motion-sensor'?'motion-state':'status'),raw.state);
    return values;
  }

  function selectQuickReference(item) {
    const type=quickCardType(item), rules=quickReferenceRules[type];
    // HA locks use the existing read-only status composition; auxiliary
    // camera/settings switches must never become the door's main control.
    if(type==='lock')return {type,variant:0,layout:22,size:[808,532],requires:['lock.status']};
    if(!rules)return {type,variant:0,layout:item.domain==='sensor'?8:5,size:[524,358],requires:[]};
    const values=item.quick_values||{}, matches=rule=>{
      const seen=Object.create(null);let count=0;
      rule[4].forEach(key=>{seen[key]=(seen[key]||0)+1;const value=values[key];
        if(value!==undefined&&(Array.isArray(value)?value.length:1)>=seen[key])count++;});return count;
    };
    let best=rules[0],bestScore=-1;
    rules.forEach(rule=>{const count=matches(rule),score=(count===rule[4].length?1000:0)+count*10-rule[4].length;
      if(score>bestScore){best=rule;bestScore=score;}});
    return {type,variant:best[0],layout:best[1],size:[best[2],best[3]],requires:best[4]};
  }

  const quickPalette=dark?{card:0x202020,ink:0xf1f3f5,muted:0xa0a6ac,line:0x303030,
    track:0x303439,green:0x59cf98,blue:0x3d99f4,paleBlue:0x377797,orange:0xe6a46b,purple:0x8985ef}:
    {card:0xffffff,ink:0x333333,muted:0x999999,line:0xf3f3f3,track:0xf4f4f4,
      green:0x58d395,blue:0x379bff,paleBlue:0xb0dfff,orange:0xe8ac73,purple:0x8882f0};

  function buildQuickCardUi(out, slot) {
    slot=slot||0;if(!out.quickGroups)out.quickGroups=[];if(out.quickGroups[slot])return;
    expandButtonPool();const q={all:[],metrics:[],actions:[],ranges:[],item:null,embedded:false};out.quickGroups[slot]=q;
    if(slot===0){out.quick=q;out.quickWidgets=q.all;}
    function box(){const id=panel(0,0,4,4,quickPalette.card,0);q.all.push(id);return id;}
    function text(){const id=label('',0,0,4,4,16,quickPalette.ink);q.all.push(id);return id;}
    q.panel=box();q.headerLine=box();q.footerLine=box();q.name=text();q.moreLabel=text();
    q.powerFace=box();q.powerGlyph=text();q.camera=box();q.cameraGlyph=text();
    for(let i=0;i<3;i++)q.metrics.push({value:text(),caption:text()});
    for(let i=0;i<4;i++) {
      const a={face:box(),glyph:text(),caption:text(),item:null};
      a.button=createButton('',0,0,4,4,()=>{if(a.item){if(a.spec)requestFunction(indexedEntity(a.item.entity_id),a.spec,a.value);else toggleItem(a.item);}},quickPalette.card,{radius:48,borderWidth:0});a.button.section='detail';
      q.actions.push(a);
    }
    for(let i=0;i<2;i++)q.ranges.push({track:box(),fill:box(),label:text(),leftFace:box(),leftGlyph:text(),rightFace:box(),rightGlyph:text()});
    q.more=createButton('',0,0,4,4,()=>{if(!q.item)return;if(q.embedded&&layoutEditing){toggleCardSize(q.item);return;}openDetail(q.item);},quickPalette.card,{radius:0,borderWidth:0});q.more.section='common';
    q.power=createButton('',0,0,4,4,()=>{if(q.item)toggleItem(q.item);},quickPalette.card,{radius:48,borderWidth:0});q.power.section='common';
    q.actions.forEach(a=>a.button.section='common');q.all.forEach(id=>hide(id,true));q.actions.forEach(a=>hide(a.button,true));hide(q.more,true);hide(q.power,true);
  }

  function renderQuickCard(item, visible, frame, slot) {
    const q=widgets.quickGroups&&widgets.quickGroups[slot||0];if(!q)return;
    const key=[generation,entityRevision,!!visible,frame?frame.x+','+frame.y+','+frame.w+','+frame.h:'detail',
      layoutEditing,connected,controlRevision].join('|');
    if(q.renderKey===key&&q.renderItem===item)return;
    q.renderKey=key;q.renderItem=item;
    q.all.forEach(id=>hide(id,true));q.actions.forEach(a=>{hide(a.button,true);a.item=null;a.spec=null;a.value=undefined;});hide(q.more,true);hide(q.power,true);q.brightnessRect=null;q.controlRects=[];q.sliderRects=[];q.item=item;q.embedded=!!frame;
    if(!visible)return;
    if(q.motionEntity!==item.entity_id){
      q.motionEntity=item.entity_id;
      q.all.forEach(id=>{delete paint['p'+id];delete paint['s'+id];delete paint['c'+id];delete paint['r'+id];});
    }
    const target=controlTargets[item.entity_id];
    if(target&&target.generation===generation&&target.brightness!==undefined)
      item=Object.assign({},item,{brightness:target.brightness});
    const ref=selectQuickReference(item), layout=ref.layout, lampRange=ref.type==='light';
    const c=lampRange?Object.assign({},quickPalette,{card:dark?0x202020:0xffffff}):quickPalette;
    let cw=Math.min(440,GRID_W-40),ch=Math.round(cw*ref.size[1]/ref.size[0]);
    const maxHeight=H-HEADER_H-(W>=900?FOOTER_H:0)-24;
    if(ch>maxHeight){ch=maxHeight;cw=Math.round(ch*ref.size[0]/ref.size[1]);}
    if(frame){cw=frame.w;ch=frame.h;}
    const x=frame?frame.x:W>=900?W-CARD_MARGIN-cw:GRID_X+Math.floor((GRID_W-cw)/2),y=frame?frame.y:HEADER_H+12;
    const s=frame?Math.min(cw/524,ch/ref.size[1]):cw/524;
    // Embedded cards may be only one grid row tall. Text bands need their
    // own line-height budget rather than the reference image's scale factor.
    const hh=frame?52:Math.round(82*s),fh=frame?44:Math.round(72*s),bodyY=y+hh,bodyH=ch-hh-fh;
    function box(id,bx,by,bw,bh,color,radius){
      const smooth=!quickDrag&&paint['p'+id]!==undefined&&q.ranges.some(r=>r.fill===id||r.thumb===id);
      hide(id,false);pushPos(id,Math.round(bx),Math.round(by),smooth);pushSize(id,Math.max(1,Math.round(bw)),Math.max(1,Math.round(bh)),smooth);
      pushColor(id,color,id===q.powerFace&&paint['c'+id]!==undefined);pushStyle(id,'Q',{radius:Math.round(radius||0),borderWidth:0});
    }
    function text(id,value,bx,by,bw,bh,font,color,icon,center){hide(id,false);pushPos(id,Math.round(bx),Math.round(by));pushSize(id,Math.max(1,Math.round(bw)),Math.max(1,Math.round(bh)));pushText(id,String(value));pushColor(id,color);pushStyle(id,'Q',{fontSize:font,icon:icon||0,center:center===false?0:1,ellipsis:1});}
    box(q.panel,x,y,cw,ch,c.card,28*s);box(q.headerLine,x+1,y+hh,cw-2,1,c.line);box(q.footerLine,x+1,y+ch-fh,cw-2,1,c.line);
    const commonProfile=commonCardProfile(item);
    const hasHeaderPower=commonProfile&&commonProfile.layout?
      !commonProfile.readonly&&supports(item,powerIsOn(item)?'turn_off':'turn_on'):
      [1,2,3,11,14,16,19,21].includes(layout)||!!commonProfile&&supports(item,item.state==='off'?'turn_on':'turn_off');
    const nameFont=frame?22:Math.max(20,Math.round(36*s));
    const nameInset=frame?16:28*s,nameWidth=frame?cw-(hasHeaderPower?84:32):cw-(hasHeaderPower?110:56)*s;
    text(q.name,fit(item.name,nameWidth,nameFont),x+nameInset,y+(frame?10:22*s),nameWidth,frame?32:Math.max(45*s,nameFont+4),nameFont,c.ink,0,false);
    text(q.moreLabel,target?(target.sent?tr('正在同步 · 更多操作'):tr('待执行 · 更多操作')):tr('更多操作'),x,y+ch-fh+(frame?9:21*s),cw,frame?26:30*s,frame?16:Math.max(14,Math.round(22*s)),c.ink);
    Object.assign(q.more,{x,y:y+ch-fh,w:cw,h:fh,hidden:false,style:{radius:Math.round(28*s),borderWidth:0}});
    const on=available(item)&&item.state!=='off'&&item.state!=='closed',writable=!layoutEditing&&connected&&available(item)&&controllable(item)&&supports(item,powerIsOn(item)?'turn_off':'turn_on');
    function power(px,py,d,header){
      if(header){
        px=x+cw-54;py=y+(frame?8:20);
        box(q.powerFace,px,py,36,36,on?c.green:pal.track,18);
        text(q.powerGlyph,'\uf011',px+6,py+6,24,24,24,on?0xffffff:pal.muted,1);
        Object.assign(q.power,{x:Math.round(px-6),y:Math.round(py-6),w:48,h:48,hidden:!writable,style:{radius:24,borderWidth:0,enabled:writable?1:0}});
        return;
      }
      if(!header)box(q.powerFace,px,py,d,d,on?c.green:c.track,d/2);
      text(q.powerGlyph,'\uf011',px+(d-32)/2,py+(d-32)/2,d<32?d:32,36,32,on?(header?c.green:0xffffff):c.muted,3);
      Object.assign(q.power,{x:Math.round(px),y:Math.round(py),w:Math.round(d),h:Math.round(d),hidden:!writable,style:{radius:Math.round(d/2),borderWidth:0,enabled:writable?1:0}});
    }
    if(hasHeaderPower)power(x+cw-67*s,y+21*s,40*s,true);
    const props=item.quick_values||{}, seen=Object.create(null);
    const records=ref.requires.map(key=>{const index=seen[key]||0;seen[key]=index+1;const raw=props[key],value=Array.isArray(raw)?raw[index]:index===0?raw:undefined;return {key,property:key.split('.').slice(1).join('.'),value};});
    const names={temperature:tr('温度'),'target-temperature':tr('设定温度'),'relative-humidity':tr('湿度'),'atmospheric-pressure':tr('气压'),'pm2.5-density':'PM2.5','hcho-density':tr('甲醛'),'tvoc-density':'TVOC','illumination':tr('光照度'),'soil-ec':tr('土壤肥力'),'battery-level':tr('电量'),'working-time':tr('运动时长'),'current-distance':tr('距离'),'left-time':tr('剩余时间'),'tds-out':tr('出水 TDS'),'status':ref.type==='lock'?tr('门锁状态'):tr('状态'),'submersion-state':tr('水浸状态')};
    function display(record){if(!available(item)||record.value===undefined)return '--';const v=record.value,p=record.property;if(typeof v==='boolean')return v?tr('已开启'):tr('已关闭');if(p==='status'||p.endsWith('-state'))return item.domain==='binary_sensor'?binaryState(item):friendlyState({state:String(v)});if(typeof v==='number'){const unit=['temperature','target-temperature'].includes(p)?' °C':p==='relative-humidity'||p==='battery-level'||p==='current-position'?'%':p==='pm2.5-density'?' ug/m3':p==='atmospheric-pressure'?' hPa':p==='illumination'?' lx':p==='current-distance'?' km':'';return numberText(v)+unit;}return String(v);}
    let readings=records.filter(r=>!['on','mode','fan-level','horizontal-swing','motor-control','brightness','color-temperature','volume','mute'].includes(r.property)&&!r.property.startsWith('start-')&&!['play','pause','stop-sweeping','package'].includes(r.property));
    let metricIndex=0,actionIndex=0,rangeIndex=0;
    function metric(record,mx,my,mw,font){if(metricIndex>=3)return;const m=q.metrics[metricIndex++],mf=frame&&bodyH<160?Math.min(24,font||24):font||Math.max(24,Math.round(44*s));text(m.value,record?display(record):dashboardValue(item),mx,my,mw,Math.max(48*s,mf+4),mf,c.ink);text(m.caption,record?names[record.property]||domainTitle(item):item.domain==='sensor'?dashboardSubtitle(item):domainTitle(item),mx,my+Math.max(51*s,mf+8),mw,30*s,Math.max(13,Math.round(23*s)),c.muted);}
    function action(glyph,caption,ax,ay,d,active,color,icon,linked){if(actionIndex>=4)return;const a=q.actions[actionIndex++];box(a.face,ax,ay,d,d,active?(color||c.paleBlue):c.track,d/2);text(a.glyph,glyph,ax,ay+(d-36)/2,d,36,glyph==='AUTO'?16:24,active?0xffffff:c.muted,icon?1:0);text(a.caption,caption,ax-12*s,ay+d+12*s,d+24*s,29*s,Math.max(12,Math.round(21*s)),c.muted);if(linked&&connected&&available(linked)&&controllable(linked)&&supports(linked,linked.state==='on'?'turn_off':'turn_on')){a.item=linked;Object.assign(a.button,{x:Math.round(ax),y:Math.round(ay),w:Math.round(d),h:Math.round(d),hidden:false,style:{radius:Math.round(d/2),borderWidth:0}});}}
    function range(label,percent,rx,ry,rw,rh,steppers,warm){
      if(rangeIndex>=2)return;const r=q.ranges[rangeIndex++],value=Math.max(0,Math.min(100,percent||0));
      const temperature=lampRange&&warm;
      const capped=lampRange||!!commonProfile,fillWidth=capped?20+(rw-20)*value/100:rw*value/100;
      box(r.track,rx,ry,rw,rh,temperature?temperatureColor(item.min_color_temp_kelvin):lampRange?(dark?0x4b3928:0xf5e9dd):c.track,lampRange?18:rh/2);
      pushStyle(r.track,'G',{gradientEnd:temperature?temperatureColor(item.max_color_temp_kelvin):0xffffff,gradientHorizontal:temperature?1:0});
      if(value>0&&!temperature)box(r.fill,rx,ry,fillWidth,rh,warm?0xecc56c:lampRange?(dark?0xffaa60:0xee9748):c.blue,lampRange?18:rh/2);
      text(r.label,label,lampRange?rx:rx+16*s,lampRange?ry-30:ry+(rh-30*s)/2,lampRange?rw:rw-32*s,30*s,Math.max(13,Math.round(22*s)),lampRange?c.ink:value>30?0xffffff:c.muted,0,false);
      if(capped){if(!r.thumb){r.thumb=panel(0,0,6,32,0xffffff,3);q.all.push(r.thumb);}box(r.thumb,rx+fillWidth-18,ry+(rh-32)/2,6,32,0xffffff,3);pushStyle(r.thumb,'G',{borderWidth:temperature?1:0,borderColor:0x65717f});}
      if(steppers){const d=rh;box(r.leftFace,rx-d-12*s,ry,d,d,c.track,d/2);text(r.leftGlyph,'\uf068',rx-d-12*s,ry+(d-24)/2,d,28,24,c.ink,1);box(r.rightFace,rx+rw+12*s,ry,d,d,c.track,d/2);text(r.rightGlyph,'\uf067',rx+rw+12*s,ry+(d-24)/2,d,28,24,c.ink,1);}
      const rect={x:rx,y:ry,w:rw,h:rh,fill:r.fill,label:r.label,gradient:temperature};
      if(capped)Object.assign(rect,{inputX:rx+5,inputW:rw-20,minFill:20,thumb:r.thumb,labelColor:c.ink});
      q.sliderRects.push(rect);return rect;
    }
    function modes(count,ay,labels,glyphs){const d=Math.min(106*s,(cw-72*s)/count-20*s),gap=(cw-48*s)/count;
      const mode=String(props[ref.type+'.mode']||item.preset_mode||item.state).toLowerCase();
      let active=layout===25&&item.state==='cleaning'?0:-1;
      if(layout===2||layout===3){
        const motor=records.find(r=>r.property==='motor-control');
        const choices=motor?['open','stop','close']:item.quick_modes.length?item.quick_modes:[mode];
        const chosen=motor?String(motor.value):mode;
        const icons={auto:'AUTO',cool:'\ue014',heat:'\ue00f',turbo:'\uf0e7',sleep:'\uf04c',silent:'\uf04c',open:'\uf077',stop:'\uf04c',close:'\uf078'};
        active=choices.map(v=>v.toLowerCase()).indexOf(chosen.toLowerCase());
        for(let i=0;i<count;i++){const value=choices[i];labels[i]=value?(motor?[tr('打开'),tr('暂停'),tr('关闭')][i]:friendlyState({state:value})):'--';glyphs[i]=value?icons[value.toLowerCase()]||'--':'--';}
      }
      for(let i=0;i<count;i++)action(glyphs[i],labels[i],x+24*s+i*gap+(gap-d)/2,ay,d,i===active,c.paleBlue,glyphs[i].charCodeAt(0)>=0xe000);}
    const middle=bodyY+Math.max(16*s,(bodyH-92*s)/2);
    if(commonProfile&&commonProfile.id!=='bedside-light'){
      const main=indexedEntity(item.entity_id)||item,spec=profileQuickControl(item);
      const inset=Math.min(24,Math.max(12,cw*.055)),rw=cw-inset*2;
      const specs=profileControls(item),actions=specs.filter(spec=>spec.kind==='action'&&!['turn_on','turn_off'].includes(spec.service)).slice(0,3);
      if(commonProfile.layout&&!spec){
        const values=profileReadings(item),primary=values[0];
        const color=values.some(row=>row.alert)?pal.warn:pal.ink;
        if(commonProfile.layout==='camera'){
          const pw=Math.min(rw*.49,Math.max(64,bodyH-20)*16/9),ph=pw*9/16;
          const px=x+inset,py=bodyY+(bodyH-ph)/2,tx=px+pw+20,tw=x+cw-inset-tx;
          box(q.camera,px,py,pw,ph,pal.track,12);
          text(q.cameraGlyph,thumbnail(item),px+(pw-68)/2,py+(ph-68)/2,68,68,40,pal.muted,2);
          const ty=bodyY+Math.max(2,(bodyH-80)/2);
          text(q.metrics[0].caption,tr('设备状态'),tx,ty,tw,24,14,pal.muted,0,false);
          text(q.metrics[0].value,primary?primary.display:dashboardValue(item),tx,ty+25,tw,30,22,available(item)?pal.ink:pal.muted,0,false);
          text(q.metrics[1].caption,commonProfile.notice,tx,ty+60,tw,24,14,pal.muted,0,false);
          pushText(q.moreLabel,tr('查看设备详情'));
        }else if(primary&&bodyH<170&&values.length>1){
          const visible=values.slice(0,3),mw=rw/visible.length,my=bodyY+Math.max(4,(bodyH-76)/2);
          visible.forEach((row,index)=>{
            const mx=x+inset+index*mw;
            text(q.metrics[index].value,row.display,mx,my,mw,40,row.display.length>9?20:28,row.alert?pal.warn:pal.ink);
            text(q.metrics[index].caption,row.title,mx,my+44,mw,24,14,pal.muted);
          });
        }else if(primary){
          const secondary=values.slice(1,3),heroY=bodyY+Math.max(6,(bodyH-(secondary.length?150:90))/2);
          text(q.metrics[0].value,primary.display,x+inset,heroY,rw,48,primary.numeric?36:26,color);
          text(q.metrics[0].caption,primary.title,x+inset,heroY+50,rw,24,14,pal.muted);
          if(bodyH>=150)secondary.forEach((row,index)=>{
            const mw=rw/secondary.length,mx=x+inset+index*mw;
            text(q.metrics[index+1].value,row.display,mx,heroY+86,mw,32,22,pal.ink);
            text(q.metrics[index+1].caption,row.title,mx,heroY+120,mw,24,14,pal.muted);
          });
        }else metric(null,x+inset,middle,rw,28);
      }
      else if(commonProfile.id==='outlet'){const d=Math.min(96,Math.max(44,bodyH-16));power(x+(cw-d)/2,bodyY+(bodyH-d)/2,d,false);}
      else if(spec){
        const value=controlValue(main,spec),percent=available(main)&&Number.isFinite(value)?(value-spec.min)*100/(spec.max-spec.min):0;
        const rh=Math.min(52,Math.max(32,bodyH-16)),ry=bodyY+(bodyH-rh)/2;
        const r=range(available(main)?spec.title+' '+formatControl(spec,value):tr('设备不可用'),percent,x+inset,ry,rw,rh,false);
        Object.assign(r,{spec,item:main,labelColor:pal.ink});
        pushColor(r.label,pal.ink);
        if(!available(main)){hide(r.fill,true);hide(r.thumb,true);}
        if(!layoutEditing&&connected&&available(main))q.controlRects.push(r);
        if(commonProfile.reading&&item[commonProfile.reading]!==null&&bodyH>=150){
          pushPos(q.ranges[0].track,x+inset,bodyY+bodyH-rh-10);pushPos(q.ranges[0].fill,x+inset,bodyY+bodyH-rh-10);
          pushPos(q.ranges[0].label,x+inset+12,bodyY+bodyH-rh-10+(rh-30)/2);r.y=bodyY+bodyH-rh-10;
          if(r.thumb)pushPos(r.thumb,r.x+20+(r.w-20)*Math.max(0,Math.min(100,percent))/100-18,r.y+(rh-32)/2,!quickDrag&&!lampWheelDrag&&paint['p'+r.thumb]!==undefined);
          const title=commonProfile.reading==='pm25'?'PM2.5':commonProfile.reading==='current_humidity'?tr('当前湿度'):tr('当前温度');
          text(q.metrics[0].value,numberText(item[commonProfile.reading]),x+inset,bodyY+10,rw,44,32,pal.ink,0,false);
          text(q.metrics[0].caption,title,x+inset,bodyY+54,rw,24,14,pal.muted,0,false);
        }
      }else if(actions.length&&bodyH>=72){
        const d=Math.min(54,bodyH-32),gap=rw/actions.length,ay=bodyY+(bodyH-d-24)/2;
        actions.forEach((spec,index)=>{action(spec.service==='return_to_base'?'\uf015':spec.service==='pause'?'\uf04c':spec.service==='start'?'\uf04b':'\uf011',spec.title,x+inset+index*gap+(gap-d)/2,ay,d,true,c.blue,true);
          const button=q.actions[index];button.item=main;button.spec=spec;Object.assign(button.button,{x:Math.round(x+inset+index*gap),y:Math.round(ay),w:Math.round(gap),h:Math.round(d+24),hidden:!connected||!available(main)||layoutEditing});});
      }else{
        const values=commonProfile.readonly?[[tr('温度'),item.current_temperature],[tr('湿度'),item.current_humidity]].filter(row=>row[1]!==null):[];
        if(values.length)values.slice(0,2).forEach((row,index)=>{const width=rw/values.length; text(q.metrics[index].value,numberText(row[1])+(row[0]==='湿度'?'%':' °C'),x+inset+index*width,middle,width,46,28,pal.ink);text(q.metrics[index].caption,row[0],x+inset+index*width,middle+48,width,24,14,pal.muted);});
        else metric(null,x+inset,middle,rw,32);
      }
      return;
    }
    if(layout===4){const d=frame?Math.min(96,bodyH-12):116*s;power(x+(cw-d)/2,bodyY+(bodyH-d)/2,d,false);}
    else if([5,8,22].includes(layout))metric(readings[0],x+20*s,middle,cw-40*s,Math.max(26,Math.round(56*s)));
    else if([6,7].includes(layout)){const n=layout===6?3:2;for(let i=0;i<n;i++)metric(readings[i],x+20*s+i*(cw-40*s)/n,middle,(cw-40*s)/n,Math.max(20,Math.round((n===3?31:42)*s)));}
    else if(layout===18){const d=frame?Math.min(64,bodyH-16):66*s,rx=frame?x+40*s+d+20:x+132*s;power(x+40*s,bodyY+(bodyH-d)/2,d,false);const rect=range(tr('亮度 ')+(item.brightness===null?'--':item.brightness+'%'),item.brightness,rx,bodyY+(bodyH-d)/2,x+cw-40*s-rx,d,false);if(canAdjustBrightness(item))q.brightnessRect=rect;}
    else if(layout===21){const rh=64*Math.min(1,s),row=rh+30+20*s;
      const ry=bodyY+Math.max(0,(bodyH-(2*row-20*s))/2)+30;
      const rect=range(tr('亮度 ')+(item.brightness===null?'--':item.brightness+'%'),item.brightness,x+40*s,ry,cw-80*s,rh,false);
      const spec=functionControls(item).find(c=>c.key==='color_temp_kelvin');
      const value=spec?controlValue(item,spec):item.color_temp_kelvin;
      const ct=range(value===null?tr('色温 --'):tr('色温 ')+value+'K',spec?(Number(value)-spec.min)*100/(spec.max-spec.min):50,x+40*s,ry+row,cw-80*s,rh,false,true);
      if(spec&&connected&&available(item)&&!layoutEditing){ct.spec=spec;q.controlRects.push(ct);}
      if(canAdjustBrightness(item))q.brightnessRect=rect;}
    else if(layout===20){power(x+40*s,bodyY+62*s,66*s,false);const rect=range(tr('亮度 ')+(item.brightness===null?'--':item.brightness+'%'),item.brightness,x+128*s,bodyY+62*s,cw-168*s,66*s,false);range(tr('风速 ')+(item.percentage===null?'--':item.percentage+'%'),item.percentage,x+128*s,bodyY+178*s,cw-168*s,66*s,false);if(canAdjustBrightness(item))q.brightnessRect=rect;}
    else if(layout===16){action('\uf2dc',item.oscillating?tr('摇头'):tr('送风'),x+40*s,bodyY+(bodyH-66*s)/2,66*s,item.oscillating,c.blue,true);range(item.percentage===null?tr('风速 --'):tr('风速 ')+item.percentage+'%',item.percentage,x+132*s,bodyY+(bodyH-66*s)/2,cw-172*s,66*s,false);}
    else if(layout===2){range(tr('设定温度 ')+(item.temperature===null?'--':numberText(item.temperature)+'°C'),item.temperature===null?0:Math.min(100,item.temperature/40*100),x+112*s,bodyY+30*s,cw-224*s,64*s,true);modes(3,bodyY+122*s,[tr('自动'),tr('制冷'),tr('制热')],['A','\ue014','\ue00f']);}
    else if([3,19,11].includes(layout)){metric(readings[0],x+20*s,bodyY+36*s,cw-40*s);if(layout===3)modes(3,bodyY+164*s,[tr('自动'),tr('强力'),tr('睡眠')],['AUTO','\uf0e7','\uf04c']);else range(layout===19?tr('风速 ')+(item.percentage===null?'--':item.percentage+'%'):tr('温度 ')+(item.temperature===null?'--':numberText(item.temperature)+'°C'),layout===19?item.percentage:50,x+(layout===11?112:40)*s,bodyY+186*s,cw-(layout===11?224:80)*s,68*s,layout===11);}
    else if([12,13].includes(layout)){const count=layout===12?3:2;const toggles=records.filter(r=>r.property==='on');const d=100*s,gap=(cw-40*s)/count;for(let i=0;i<count;i++){const linked=indexedEntity((item.members||[])[i]);action('\uf011',linked?fit(linked.name,90*s,14):tr('开关 ')+(i+1),x+20*s+i*gap+(gap-d)/2,bodyY+(bodyH-d-50*s)/2,d,linked?linked.state==='on':toggles[i]&&toggles[i].value===true,c.green,true,linked);}}
    else if(layout===10){modes(3,bodyY+28*s,[tr('照明'),tr('换气'),tr('加热')],['\uf011','\uf011','\uf011']);range(tr('温度 ')+(item.temperature===null?'--':numberText(item.temperature)+'°C'),50,x+112*s,bodyY+208*s,cw-224*s,64*s,true);}
    else if(layout===9){modes(3,bodyY+(bodyH-130*s)/2,[tr('打开'),tr('暂停'),tr('关闭')],['\uf077','\uf04c','\uf078']);}
    else if([14,15].includes(layout)){box(q.camera,x+44*s,bodyY+32*s,cw-88*s,bodyH-64*s,dark?0x343044:0x555063,15*s);text(q.cameraGlyph,layout===14?'\uf021':'\uf04b',x+44*s,bodyY+bodyH/2-16,cw-88*s,36,24,0xc8c5cc,1);}
    else if([23,24].includes(layout)){modes(2,bodyY+40*s,[tr('播放状态'),tr('麦克风')],[item.state==='playing'?'\uf04c':'\uf04b','\ue005']);range(tr('音量 ')+(item.volume_level===null?'--':Math.round(item.volume_level*100)+'%'),item.volume_level===null?0:item.volume_level*100,x+112*s,bodyY+224*s,cw-224*s,64*s,true);}
    else if([17,25].includes(layout)){metric(readings[0],x+20*s,bodyY+38*s,cw-40*s);modes(2,bodyY+174*s,layout===25?[tr('清扫状态'),tr('回充')]:[tr('打包'),tr('开盖')],layout===25?['\uf04c','\uf0e7']:['AUTO','AUTO']);}
    else if(layout===1){action('\ue00f',tr('氛围灯'),x+(cw-106*s)/2,bodyY+36*s,106*s,props['light.on']===true,c.orange,true);modes(4,bodyY+230*s,[tr('助眠'),tr('唤醒'),tr('阅读'),tr('运动')],['\uf04c','\uf0f3','\uf15b','\uf079']);}
    if(!available(item))text(q.metrics[0].caption,item.state==='unavailable'?tr('设备离线'):tr('状态未知'),x+20*s,y+ch-fh-40*s,cw-40*s,28*s,14,pal.warn);
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
      light: tr('\u706f\u5149'), switch: tr('\u5f00\u5173'), input_boolean: tr('\u6a21\u5f0f'), fan: tr('\u98ce\u6247'),
      sensor: tr('\u4f20\u611f\u5668'), binary_sensor: tr('\u72b6\u6001'), cover: tr('\u906e\u9633'), climate: tr('\u6e29\u63a7'),
      media_player: tr('\u5a92\u4f53'), person: tr('\u6210\u5458'), device_tracker: tr('\u4f4d\u7f6e'), weather: tr('\u5929\u6c14'),
      sun: tr('\u65e5\u7167'), update: tr('\u66f4\u65b0'), lock: tr('\u95e8\u9501'), vacuum: tr('\u6e05\u6d01'), camera: tr('\u6444\u50cf\u5934'),
      alarm_control_panel: tr('\u5b89\u9632'), automation: tr('\u81ea\u52a8\u5316'), script: tr('\u811a\u672c'), scene: tr('\u573a\u666f')
    }[item.domain] || item.domain.replace(/_/g, ' ');
  }

  function entitySection(item) {
    if(['scene','script'].includes(item.domain))return 'scenes';
    if (['light', 'switch', 'input_boolean', 'fan', 'cover', 'climate', 'media_player',
      'vacuum', 'lock', 'camera', 'alarm_control_panel'].includes(item.domain)) return 'devices';
    return 'devices';
  }

  function deviceCardKey(item) { const meta=entityDevices[item.entity_id];const id=item.device_id||meta&&meta.id;return id?'device.'+id:item.entity_id; }
  function validCardKey(key) { return validEntity(key)||/^device\.[a-zA-Z0-9_-]{1,64}$/.test(key); }

  function flushUsage() {
    if(!usageDirty||!storage||typeof storage.set!=='function')return;
    try{storage.set('eh_card_usage',JSON.stringify({server:usageServer,counts:usageCounts}));usageDirty=false;usageSaveAt=Infinity;}
    catch(_){usageSaveAt=steadyNow()+30000;}
  }
  function ensureUsage() {
    if(usageServer===url)return;
    flushUsage();usageServer=url;usageCounts=Object.create(null);
    try{const saved=JSON.parse(storage&&storage.get('eh_card_usage')||'{}');
      if(saved.server===url&&saved.counts&&typeof saved.counts==='object'&&!Array.isArray(saved.counts))
        Object.keys(saved.counts).slice(0,32).forEach(key=>{const count=saved.counts[key];
          if(validCardKey(key)&&Number.isSafeInteger(count)&&count>0&&count<=1000000)usageCounts[key]=count;});
    }catch(_){}
    homeWeights=Object.assign(Object.create(null),usageCounts);homeOrderRevision++;usageDirty=false;
  }
  function recordUsage(item) {
    if(!item)return;ensureUsage();const key=deviceCardKey(item),keys=Object.keys(usageCounts);if(!validCardKey(key))return;
    if(!usageCounts[key]&&keys.length>=32){keys.sort((a,b)=>usageCounts[a]-usageCounts[b]);delete usageCounts[keys[0]];}
    usageCounts[key]=Math.min(1000000,(usageCounts[key]||0)+1);
    // Keep the current page stable; rank on the next overview visit. Batch
    // preference writes instead of writing flash on every tap or drag.
    usageDirty=true;usageSaveAt=steadyNow()+10000;desktopRevision++;
  }
  // Pins and usage belong to one HA endpoint, independent of sync scope.
  function ensureDesktopPins() {
    if(desktopServer===url)return;
    desktopServer=url;desktopPins=[];desktopPublished='';desktopRevision++;
    try{const saved=JSON.parse(storage&&storage.get('eh_desktop_pins')||'{}');
      if(saved.server===url&&Array.isArray(saved.pins))desktopPins=saved.pins.filter(validCardKey)
        .filter((key,i,list)=>list.indexOf(key)===i).slice(0,2);
    }catch(_){}
  }
  function desktopCards() {
    ensureUsage();ensureDesktopPins();
    const cards=groupedDeviceCards().slice();
    cards.sort((a,b)=>{
      const ai=desktopPins.indexOf(deviceCardKey(a)),bi=desktopPins.indexOf(deviceCardKey(b));
      if(ai>=0||bi>=0)return (ai<0?2:ai)-(bi<0?2:bi);
      return (usageCounts[deviceCardKey(b)]||0)-(usageCounts[deviceCardKey(a)]||0)||
        String(a.name).localeCompare(String(b.name))||a.entity_id.localeCompare(b.entity_id);
    });
    return cards.slice(0,2);
  }
  function toggleDesktopPin() {
    const card=currentDeviceCard();if(!card)return;
    ensureDesktopPins();const key=deviceCardKey(card),index=desktopPins.indexOf(key);
    if(index>=0)desktopPins.splice(index,1);
    else if(desktopPins.length>=2){message=tr('桌面已固定两台设备，请先取消一个固定');render();return;}
    else desktopPins.push(key);
    save('eh_desktop_pins',JSON.stringify({server:url,pins:desktopPins}));
    desktopRevision++;detailRenderKey='';publishDesktop();render();
  }
  function publishDesktop() {
    if(!system.desktop||typeof system.desktop.devices!=='function'||!connected||quickDrag||lampWheelDrag)return;
    ensureDesktopPins();const key=[url,entityRevision,desktopRevision].join('|');
    if(key===desktopInputKey)return;
    const items=desktopCards().map(card=>({entity:card.entity_id,name:clean(card.name,30),
      state:clean(card.device_online===false?tr('设备离线'):friendlyState(card)+(card.domain==='light'&&card.state==='on'&&card.brightness!==null?' · '+card.brightness+'%':''),18)||tr('状态未知'),
      pinned:desktopPins.includes(deviceCardKey(card))}));
    const json=JSON.stringify({server:url,items});
    if(json===desktopPublished){desktopInputKey=key;return;}
    try{if(system.desktop.devices(json)===0){desktopPublished=json;desktopInputKey=key;}}catch(_){}
  }
  function resolveDesktopDevice() {
    if(!pendingDesktop||!connected)return;
    if(pendingDesktop.server!==url){pendingDesktop=null;return;}
    const request=pendingDesktop;pendingDesktop=null;
    const card=groupedDeviceCards().find(card=>card.device_entities.includes(request.entity));
    if(card)openDetail(card);
    else {message=tr('该设备已移除或未在当前同步范围内');render();}
  }
  function openDesktopDevice(server,entity) {
    if(server!==url||!validEntity(entity)||disposed)return false;
    pendingDesktop={server:url,entity};
    if(connected)resolveDesktopDevice();
    else {settingsOpen=false;autoRefresh=true;refresh();}
    return true;
  }
  function preferredHomeCard(item) {
    return item.device_entities.some(id=>favorites.includes(id))||!!homeWeights[deviceCardKey(item)];
  }
  function devicePrimaryRank(item) {
    const order=['light','climate','fan','cover','vacuum','camera','lock','media_player',
      'humidifier','water_heater','switch','input_boolean','weather','binary_sensor','sensor'];
    const rank=order.indexOf(item.domain);
    return (rank<0?100:rank)*10+(item.domain==='sensor'?(isTemperature(item)?0:isHumidity(item)?1:2):0);
  }
  function registeredDeviceType(meta,rows) {
    // Device registry model, unlike an entity's HA domain, describes the
    // product. A camera's indicator light is not a light product.
    const model=String(meta&&meta.model||'').toLowerCase();
    const product=model.match(/^[a-z0-9_-]+\.([a-z0-9_-]+)\.[a-z0-9_.-]+$/);
    if(product) {
      const aliases={aircondition:'air-conditioner',airconditioner:'air-conditioner',
        airpurifier:'air-purifier',airfresh:'air-fresh',airmonitor:'air-monitor',
        sensor_ht:'temperature-humidity-sensor',sensor_magnet:'magnet-sensor',
        sensor_motion:'motion-sensor',sensor_smoke:'smoke-sensor',sensor_wleak:'submersion-sensor',
        sensor_gas:'gas-sensor',plug:'outlet',airdryer:'airer',wifispeaker:'speaker',
        doorbell:'video-doorbell',waterheater:'water-heater',waterpurifier:'water-purifier'};
      const type=aliases[product[1]]||product[1].replace(/_/g,'-');
      if(quickCardRegistry[type]||type==='lock')return type;
    }
    // Explicit provider declarations preserve every registered category,
    // including appliances whose HA main entity is only a status sensor.
    const declared=rows.map(row=>({row,type:quickCardType(row)})).filter(entry=>entry.row.card_type);
    declared.sort((a,b)=>devicePrimaryRank(a.row)-devicePrimaryRank(b.row));
    return declared.find(entry=>entry.type)?.type||'';
  }
  function productPrimaryRank(item,type) {
    if(!type)return devicePrimaryRank(item);
    const domains={light:['light'],mirror:['light'],camera:['camera'],lock:['lock'],
      'video-doorbell':['camera','binary_sensor'],fan:['fan'],'ceiling-fan':['fan'],
      'air-purifier':['fan'],'air-fresh':['fan'],'air-conditioner':['climate'],
      curtain:['cover'],airer:['cover'],vacuum:['vacuum'],speaker:['media_player'],
      humidifier:['humidifier'],'water-heater':['water_heater']};
    if((domains[type]||[]).includes(item.domain))return 0;
    if(type==='lock'&&item.domain==='sensor'&&
       (/(?:^|_)(?:door_state|lock_state)(?:_p_\d+_\d+)?$/.test(item.entity_id)||item.device_class==='lock'))return 1;
    // MIoT integrations expose the product's on property as its own switch.
    // Match that exact property, never 'motion_detection', 'mute', etc.
    if(type!=='lock'&&['switch','input_boolean'].includes(item.domain)&&
       /(?:^|[._])on_p_\d+_\d+$/.test(item.entity_id))return 2;
    const declared=quickCardType(item);
    if(item.card_type&&declared===type)return 3;
    if(['outlet','switch','relay'].includes(type)&&['switch','input_boolean'].includes(item.domain))return 4;
    if(['sensor','binary_sensor'].includes(item.domain))return 10+devicePrimaryRank(item)/100;
    // If the native main control is absent, show a read-only status member.
    // An unrelated light/switch cannot act as the whole product's power.
    return 100+devicePrimaryRank(item);
  }
  const nonDeviceDomains=new Set(['automation','calendar','conversation','device_tracker','event','notify','person','persistent_notification','scene','script','stt','sun','todo','tts','update','zone']);
  function deviceGroupVisible(group,type){
    const rows=group.items,meta=group.meta;
    // HA registers software services as devices too. Do not classify by names.
    if(meta&&meta.manufacturer==='Home Assistant'&&meta.model==='Home Assistant Backup')return false;
    if(type)return true;
    const candidates=rows.filter(row=>!nonDeviceDomains.has(row.domain));
    if(!candidates.length)return false;
    // Unregistered timestamp-only groups (for example Sun events) are not
    // appliances. Keep known hardware and explicitly followed sensor entities.
    return !!(meta&&(meta.model||meta.manufacturer))||candidates.some(row=>
      row.domain!=='sensor'||!['timestamp','date'].includes(row.device_class)||favorites.includes(row.entity_id));
  }
  function groupedDeviceCards() {
    const cacheKey=entityRevision+'|'+favorites.join(',');
    if(deviceCardsKey===cacheKey)return deviceCards;
    const groups=Object.create(null),order=[];
    entities.forEach(item=>{
      const meta=entityDevices[item.entity_id];
      const id=meta?meta.id:/^[a-zA-Z0-9_-]{1,64}$/.test(item.device_id||'')?item.device_id:'';
      const key=id?'device.'+id:item.entity_id;
      if(!groups[key]){groups[key]={id,meta,items:[]};order.push(key);}
      groups[key].items.push(item);
    });
    deviceCards=order.map(key=>{
      const group=groups[key],productType=registeredDeviceType(group.meta,group.items);
      if(!deviceGroupVisible(group,productType))return null;
      const ranks=Object.create(null);group.items.forEach(item=>ranks[item.entity_id]=productPrimaryRank(item,productType));
      const rows=group.items.slice().sort((a,b)=>ranks[a.entity_id]-ranks[b.entity_id]);
      const primary=rows[0],card=Object.assign({},primary,{
        device_id:group.id,card_id:key,name:group.meta&&group.meta.name||primary.device_name||primary.name,
        area:group.meta&&group.meta.area||'',device_entities:rows.map(r=>r.entity_id),
        device_sections:[...new Set(rows.map(entitySection))],device_online:rows.some(available),
        card_type:productType||primary.card_type,device_model:group.meta&&group.meta.model||'',
        quick_values:Object.assign(Object.create(null),primary.quick_values)
      });
      const type=quickCardType(card);
      // No main product entity: keep auxiliary controls in More only.
      if(productType&&productPrimaryRank(primary,productType)>=100)card.product_readonly=true;
      if(type==='camera'&&primary.domain==='switch'&&productPrimaryRank(primary,type)===2)
        card.quick_values['camera-control.on']=primary.state==='on';
      if(type==='lock')card.quick_values['lock.status']=primary.state;
      rows.forEach(member=>{
        if(!available(member))return;
        Object.entries(member.quick_values||{}).forEach(([property,value])=>{
          if(card.quick_values[property]===undefined)card.quick_values[property]=value;
        });
        const value=Number(member.state);
        if(member.domain!=='sensor'||!Number.isFinite(value))return;
        const key=member.device_class==='temperature'?'temperature':member.device_class==='humidity'?'relative-humidity':
          member.device_class==='pm25'?'pm2.5-density':member.device_class==='battery'?'battery-level':null;
        if(key){
          if(card.quick_values['environment.'+key]===undefined)card.quick_values['environment.'+key]=value;
          if(type&&card.quick_values[type+'.'+key]===undefined)card.quick_values[type+'.'+key]=value;
          if(key==='temperature'&&card.current_temperature===null)card.current_temperature=value;
          if(key==='relative-humidity'&&card.current_humidity===null)card.current_humidity=value;
          if(key==='pm2.5-density'&&card.pm25===null)card.pm25=value;
          if(key==='battery-level'&&card.battery_level===null)card.battery_level=value;
        }
      });
      if(group.id&&!primary.card_type&&['switch','input_boolean'].includes(primary.domain)&&['outlet','switch','relay'].includes(type)) {
        const switches=rows.filter(r=>['switch','input_boolean'].includes(r.domain)).slice(0,3);
        if(switches.length>1&&!Array.isArray(primary.quick_values['switch.on'])){
          card.members=switches.map(r=>r.entity_id);card.quick_values['switch.on']=switches.map(r=>available(r)&&r.state==='on');}
      }
      return card;
    }).filter(Boolean);
    deviceCardsKey=cacheKey;return deviceCards;
  }

  function currentDeviceCard() {
    const item=current();
    if(nav==='scenes'&&item&&['scene','script'].includes(item.domain))return Object.assign({},item,{device_entities:[item.entity_id]});
    return groupedDeviceCards().find(card=>card.device_entities.includes(selectedId))||current();
  }

  function dashboardStats() {
    const key=entityRevision+'|'+generation+'|'+favorites.join(',');
    if(dashboardStatsKey===key)return dashboardStatsValue;
    const cards=groupedDeviceCards();
    const running=new Set(entities.filter(item=>available(item)&&controllable(item)&&item.state==='on').map(item=>item.entity_id));
    let online=0,active=0;
    cards.forEach(card=>{
      if(card.device_online)online++;
      if(card.device_entities.some(id=>running.has(id)))active++;
    });
    dashboardStatsValue=[tr('设备 ')+cards.length,tr('在线 ')+online,tr('运行 ')+active];
    dashboardStatsKey=key;return dashboardStatsValue;
  }

  function changeDeviceFunction(delta) {
    const card=currentDeviceCard();if(!card||!card.device_entities)return;
    const ids=deviceFunctions().map(e=>e.entity_id),index=ids.indexOf(selectedId),next=index+delta;
    if(next>=0&&next<ids.length){selectedId=ids[next];controlPage=0;render();}
  }

  function dashboardEntities() {
    if(nav==='logs')return activityLog;
    ensureUsage();
    const cacheKey = entityRevision + '|' + nav + '|' + favorites.join(',')+'|'+homeOrderRevision+'|'+discoveryRevision;
    if (dashboardCacheKey === cacheKey) return dashboardCache;
    const cards=groupedDeviceCards();
    const list = nav === 'scenes' ? (sceneSyncReady?sceneEntities:entities.filter(item=>['scene','script'].includes(item.domain))).map(item=>Object.assign({},item,{device_entities:[item.entity_id]})) : nav === 'overview' ? cards.slice() :
      cards.filter(item => item.device_sections.includes(nav));
    const filtered=list.filter(item=>(!selectedArea||item.area===selectedArea)&&(!favoritesOnly||item.device_entities.some(id=>favorites.includes(id)))&&
      (!search||(item.name+' '+item.entity_id+' '+(item.area||'')+' '+item.device_entities.join(' ')).toLowerCase().includes(search)));
    dashboardCache = filtered.map((item, index) => {
      let score = item.device_entities.some(id=>favorites.includes(id)) ? -100 : 0;
      if (!available(item)) score += 80;
      if (['light','switch','input_boolean','fan'].includes(item.domain)&&controllable(item)) score -= 40;
      else if (isTemperature(item) || isHumidity(item)) score -= 30;
      else if (item.domain === 'binary_sensor') score -= 20;
      else if (['climate', 'cover', 'media_player', 'weather'].includes(item.domain)) score -= 10;
      return { item, index, score, favorite:item.device_entities.some(id=>favorites.includes(id)),uses:homeWeights[deviceCardKey(item)]||0 };
    }).sort((a, b) => (nav==='overview'?(Number(b.favorite)-Number(a.favorite)||b.uses-a.uses):0)||a.score-b.score||a.index-b.index).map(row => row.item);
    dashboardCacheKey = cacheKey;
    return dashboardCache;
  }

  function pageCount() {
    return dashboardPages().length;
  }

  function cardSize(item) { return 'small'; }

  function dashboardPages() {
    const items=dashboardEntities();if(items.length)cardSize(items[0]);
    const key=entityRevision+'|'+nav+'|'+(nav==='logs'?logSequence:0)+'|'+sizeRevision+'|'+layoutEditing+'|'+favorites.join(',')+'|'+homeOrderRevision+'|'+discoveryRevision;
    if(boardCacheKey===key)return boardCache;
    const previousRows=WATERFALL&&boardCacheNav===nav&&boardCacheGeneration===generation&&boardCache.length?boardCache[0].items:[];
    const anchorRows=boardScroll>0&&!boardTouchActive&&!quickDrag?previousRows.filter(row=>row.y+row.h>HEADER_H+boardScroll).sort((a,b)=>a.y-b.y||a.x-b.x):[];
    const common=nav==='overview'?items.filter(preferredHomeCard):[],commonSet=new Set(common);
    const ordered=common.concat(items.filter(item=>!commonSet.has(item))),pages=[];
    if(WATERFALL){
      const columns=Array(CARD_COLS).fill(CARD_TOP),rows=ordered.map(item=>{
        const column=columns.indexOf(Math.min(...columns)),h=nav==='logs'?Math.max(172,Math.ceil(VIEW_H/3)):masonryHeight(item);
        const row={item,large:false,x:GRID_X+CARD_MARGIN+column*(CARD_W+CARD_GAP),y:columns[column],w:CARD_W,h,column};
        columns[column]+=h+CARD_GAP;return row;
      });
      pages.push({items:rows,height:Math.max(VIEW_H,Math.max(...columns)-HEADER_H)});
      for(const anchor of anchorRows){
        const next=rows.find(row=>row.item.entity_id===anchor.item.entity_id);
        if(next){boardAnchorRestore=Math.max(0,Math.min(pages[0].height-VIEW_H,boardScroll+next.y-anchor.y));break;}
      }
    }else for(let start=0;start<Math.max(1,ordered.length);start+=SLOTS){
      pages.push({items:ordered.slice(start,start+SLOTS).map((item,index)=>({item,large:false,
        x:GRID_X+CARD_MARGIN+(index%CARD_COLS)*(CARD_W+CARD_GAP),
        y:CARD_TOP+Math.floor(index/CARD_COLS)*(CARD_H+CARD_GAP),w:CARD_W,h:CARD_H}))});
    }
    boardCache=pages;boardCacheKey=key;boardCacheNav=nav;boardCacheGeneration=generation;return pages;
  }

  function toggleCardSize(item) {
    const next=cardSize(item)==='small'?'large':'small';cardSizePrefs[deviceCardKey(item)]=next;sizeRevision++;
    layoutSaveError=false;
    try{if(!storage||typeof storage.set!=='function')throw Error('storage unavailable');
      const ids=Object.keys(cardSizePrefs);if(ids.length>MAX_CARD_PREFS)throw Error('layout limit');['eh_card_sizes','eh_card_sizes_2'].forEach((key,index)=>{const sizes=Object.create(null);ids.slice(index*64,(index+1)*64).forEach(id=>sizes[id]=cardSizePrefs[id]);storage.set(key,JSON.stringify({server:url,sizes}));});}
    catch(_){layoutSaveError=true;}
    const pages=dashboardPages();roomPage=Math.max(0,pages.findIndex(p=>p.items.some(row=>row.item.entity_id===item.entity_id)));
    render();
  }

  function ensureQuickGroups(count) {
    quickGroupTarget=Math.max(quickGroupTarget,Math.min(2,count));
    const ready=widgets.quickGroups?widgets.quickGroups.length:0;
    if(ready>=quickGroupTarget||quickGroupBuilding||disposed)return;
    quickGroupBuilding=true;setTimeout(()=>{
      if(disposed)return;buildQuickCardUi(widgets,ready);quickGroupBuilding=false;
      render();ensureQuickGroups(quickGroupTarget);
    },20);
  }

  function applyDiscovery(){
    cancelInputs();discoveryRevision++;browseOpen=false;selectedId='';roomPage=0;resetBoardScroll();render();
  }
  function clearDiscovery(){search='';selectedArea='';favoritesOnly=false;applyDiscovery();}
  function openBrowse(){cancelInputs();browseOpen=true;browsePage=0;boardLock(true);render();}
  function roomChoices(){return ['',...new Set(groupedDeviceCards().map(item=>item.area).filter(Boolean))].sort((a,b)=>a===''?-1:b===''?1:a.localeCompare(b));}
  function browsePageSize(){return H<450?2:3;}
  function renderBrowse(){
    if(!browseOpen){if(browseUi){if(browseUi.locked){boardLock(false);browseUi.locked=false;}browseUi.decor.forEach(id=>hide(id,true));browseUi.buttons.forEach(button=>hide(button,true));}return;}
    if(!browseUi){
      const u={decor:[],buttons:[],rooms:[]},keep=id=>{u.decor.push(id);return id;};
      u.face=keep(panel(0,0,1,1,pal.card,20));u.title=keep(label('',0,0,1,1,20,pal.ink));u.note=keep(label('',0,0,1,1,13,pal.muted));
      const make=(text,action)=>{const b=createButton(text,0,0,1,1,action,pal.page,{radius:10,borderWidth:0,fontSize:15});b.section='browse';u.buttons.push(b);return b;};
      u.favorite=make('',()=>{favoritesOnly=!favoritesOnly;applyDiscovery();});
      for(let i=0;i<3;i++)u.rooms.push(make('',()=>{const rooms=roomChoices(),index=browsePage*browsePageSize()+i;if(index<rooms.length){selectedArea=rooms[index];applyDiscovery();}}));
      u.prev=make(tr('上一页'),()=>{browsePage=Math.max(0,browsePage-1);render();});u.next=make(tr('下一页'),()=>{browsePage++;render();});
      u.clear=make(tr('清除筛选'),clearDiscovery);u.close=make(tr('返回'),()=>{browseOpen=false;render();});browseUi=u;
    }
    const rooms=roomChoices(),count=browsePageSize(),pages=Math.max(1,Math.ceil(rooms.length/count));browsePage=Math.min(browsePage,pages-1);
    const shown=Math.min(count,rooms.length-browsePage*count);
    const u=browseUi,x=GRID_X+18,y=HEADER_H+12,w=GRID_W-36,h=Math.min(H-HEADER_H-26,186+shown*44+(pages>1?44:0)),pad=16,inner=w-32;
    boardLock(true);u.locked=true;
    u.decor.forEach(id=>hide(id,false));pushPos(u.face,x,y);pushSize(u.face,w,h);
    detailText(u.title,tr('筛选设备'),x+pad,y+12,inner,28,20,pal.ink);
    detailText(u.note,(search?tr('搜索：')+search+' · ':'')+(rooms.length>1?(rooms.length-1)+tr(' 个房间')+(pages>1?tr(' · 第 ')+(browsePage+1)+' / '+pages+tr(' 页'):''):tr('家庭中枢尚未分配房间')),x+pad,y+43,inner,22,13,pal.muted);
    const draw=(b,text,bx,by,bw,bh,active)=>{buttonText(b,text);Object.assign(b,{x:bx,y:by,w:bw,h:bh,color:active?pal.blueSoft:pal.page,style:{radius:10,borderWidth:0,fontSize:15}});hide(b,false);};
    draw(u.favorite,favoritesOnly?tr('已选：关注设备'):tr('仅看关注设备'),x+pad,y+70,inner,40,favoritesOnly);
    u.rooms.forEach((b,index)=>{const room=rooms[browsePage*count+index];if(index>=count||room===undefined){hide(b,true);return;}draw(b,(room===selectedArea?tr('已选 · '):'')+(room||tr('全部房间')),x+pad,y+118+index*44,inner,40,room===selectedArea);});
    const half=Math.floor((inner-8)/2),bottom=y+h-48;
    draw(u.prev,tr('上一页'),x+pad,bottom-44,half,40,false);draw(u.next,tr('下一页'),x+pad+half+8,bottom-44,half,40,false);hide(u.prev,browsePage===0);hide(u.next,browsePage===pages-1);
    draw(u.clear,tr('清除筛选'),x+pad,bottom,half,40,false);draw(u.close,tr('返回'),x+pad+half+8,bottom,half,40,false);
    if(typeof ui.front==='function')u.decor.forEach(id=>ui.front(id));
  }
  // System back consumes exactly one local layer; home bypasses this hook.
  function backOneLevel() {
    if (disposed) return false;
    if (optionPicker) { optionPicker=null;detailRenderKey='';render();return true; }
    if (browseOpen) { cancelInputs(false);browseOpen=false;render();return true; }
    if (detailOpen) { cancelInputs(false);detailOpen=false;quickGestureOwned=false;render();return true; }
    if (settingsOpen) { cancelInputs(false);settingsOpen=false;render();return true; }
    if (layoutEditing) { cancelInputs(false);layoutEditing=false;render();return true; }
    if (nav!=='overview') { selectSection('overview');return true; }
    return false;
  }
  function selectSection(section) {
    const previousSection=nav;
    cancelInputs(false);
    quickGestureOwned=false;
    if(section==='overview'){ensureUsage();homeWeights=Object.assign(Object.create(null),usageCounts);homeOrderRevision++;}
    nav = section;
    browseOpen=false;
    resetBoardScroll();roomPage = 0;
    selectedId = '';
    settingsOpen = false;
    detailOpen = false;
    optionPicker = null;
    if(connected){if(section==='scenes'){nextRefresh=0;refresh();}else if(previousSection==='scenes'&&!pending&&!queued&&!nativeBusy)nextRefresh=0;}
    render();
  }

  function* buildUiReference() {
    ui.background(pal.page);
    const out = { cards: [], settings: [], menu: [], stats: [] };
    out.settingsPanel = borderedPanel(GRID_X + 18, GRID_Y + 18, GRID_W - 36,
      H - GRID_Y - FOOTER_H - 36, pal.card, 10);
    initializeButtonPool();
    yield out;
    out.sidebar = panel(0, 0, SIDEBAR_W, H, sidebarStyle.paper, 0);
    out.brand = label(tr('我的家'), 24, 30, SIDEBAR_W - 36, 28, 20, sidebarStyle.ink);
    const sections = [
      ['overview', tr('\u603b\u89c8'), '\uf015'], ['devices', tr('\u8bbe\u5907'), '\uf00b'],
      ['scenes', tr('场景'), '\ue001'], ['logs', tr('日志'), '\uf15b']
    ];
    sections.forEach((entry, index) => {
      const y = (H<480?64:104) + index * 52;
      const row = {
        key: entry[0], panel: panel(12, y, 140, 44, sidebarStyle.paper, 12),
        icon: label(entry[2], 24, y + 11, 24, 22, 20, pal.muted, true),
        text: label(entry[1], 54, y + 11, 98, 22, 14, sidebarStyle.ink)
      };
      row.button = createButton('', 12, y, 140, 44,
        () => selectSection(entry[0]), sidebarStyle.paper, { radius: 12, borderWidth: 0 });
      row.button.section = 'navigation';
      out.menu.push(row);
    });
    for (let i = 0; i < 3; i++) out.stats.push({
      dot: panel(18, Math.min(352,H-112) + 5 + i * 28, 9, 9, i === 0 ? pal.blue : pal.good, 5),
      text: label('', 36, Math.min(352,H-112) + i * 28, SIDEBAR_W - 48, 20, 12, pal.muted)
    });
    out.note = label(tr('设备状态自动同步'), 16, H - 36,
      SIDEBAR_W - 28, 18, 11, pal.muted);
    yield out;

    out.header = panel(GRID_X, 0, GRID_W, HEADER_H, pal.page, 0);
    out.back = iconButton('\uf053', GRID_X + 12, 16, 32, 30,
      backOneLevel, pal.page);
    out.title = label('', GRID_X + 56, 17, 280, 30, 24, pal.ink);
    out.desktopPin=createButton('',W-160,10,144,40,toggleDesktopPin,pal.blueSoft,{radius:12,borderWidth:0,fontSize:15});out.desktopPin.section='detail';
    out.followButton=createButton('',W-278,10,110,44,toggleDeviceFollow,pal.blueSoft,{radius:12,borderWidth:0,fontSize:15});out.followButton.section='detail';
    out.status = label('', W - 370, 21, 110, 20, 13, pal.muted);
    pushStyle(out.status, 'L', { center: 1 });
    out.refresh = iconButton('\uf021', W - 48, 10, 44, 44, refresh, pal.page);
    out.settingsButton = iconButton('\uf013', W - 92, 10, 44, 44,
      openSettings, pal.page);
    out.searchButton=iconButton('\uf002',W-180,10,44,44,promptSearch,pal.page);
    out.browseButton=iconButton('\ue003',W-136,10,44,44,openBrowse,pal.page);
    out.emptyTitle=label('',GRID_X+24,HEADER_H+60,GRID_W-48,36,23,pal.ink);
    out.emptyNote=label('',GRID_X+24,HEADER_H+106,GRID_W-48,56,15,pal.muted);
    out.emptyAction=createButton('',GRID_X+24,HEADER_H+176,180,44,()=>{
      if(search||selectedArea||favoritesOnly)clearDiscovery();else if(!connected)openSettings();else refresh();
    },pal.blueSoft,{radius:12,borderWidth:0,fontSize:16});out.emptyAction.section='list';
    [out.emptyTitle,out.emptyNote,out.emptyAction].forEach(id=>hide(id,true));
    out.layoutEdit=createButton(tr('编辑卡片'),W-246,10,144,40,()=>{cancelInputs(false);quickGestureOwned=false;layoutEditing=!layoutEditing;roomPage=0;render();},pal.page,{radius:12,borderWidth:0,fontSize:16});out.layoutEdit.section='list';
    out.layoutEditFace=panel(W-246,10,144,40,pal.blueSoft,12);
    out.layoutEditIcon=label('\ue003',W-232,20,24,24,18,pal.blue,true);
    out.layoutEditLabel=label(tr('编辑卡片'),W-202,20,90,24,16,pal.blue);pushStyle(out.layoutEditLabel,'L',{center:1});
    yield out;

    if(WATERFALL){
      expandButtonPool();out.boardViewport=ui.viewport(GRID_X,HEADER_H,GRID_W,VIEW_H,boardScrolled);
      hide(out.boardViewport,true);
    }
    for (let index = 0; index < SLOTS; index++) {
      const x = GRID_X + CARD_MARGIN + (index % CARD_COLS) * (CARD_W + CARD_GAP);
      const y = CARD_TOP + Math.floor(index / CARD_COLS) * (CARD_H + CARD_GAP);
      const card = {
        item: null,
        x, y,
        panel: panel(x, y, CARD_W, CARD_H, pal.card, 18),
        iconBase: label('', x + 18, y + 16, 68, 70, 64, pal.deviceBase, 2),
        icon: label('', x + 18, y + 16, 68, 70, 64, pal.device, 2),
        powerHalo: panel(x + CARD_W - 68, y + 18, 48, 48, pal.track, 24),
        powerGlyph: label('\uf011', x + CARD_W - 60, y + 26, 32, 36, 32, pal.muted, 3),
        name: label('', x + 20, y + CARD_H - 66, CARD_W - 40, 28, 22, pal.ink),
        value: label('', x + 100, y + 40, CARD_W - 120, 38, 26, pal.ink),
        unit: label('', x + 190, y + 38, CARD_W - 210, 30, 20, pal.ink),
        state: label('', x + 20, y + CARD_H - 36, CARD_W - 40, 22, 14, pal.muted),
        track: panel(x + 20, y + CARD_H - 86, CARD_W - 40, 8, pal.track, 4),
        fill: panel(x + 20, y + CARD_H - 86, 2, 8, pal.blue, 4),
        thumb:null,switchFace:null,brightnessRect:null,sliderRects:[]
      };
      pushStyle(card.name, 'L', { ellipsis: 1 });
      pushStyle(card.value, 'L', { ellipsis: 1 });
      pushStyle(card.state, 'L', { ellipsis: 1 });
      card.button = createButton('', x, y, CARD_W, CARD_H,
        () => { if (card.item&&!layoutEditing&&!quickGestureOwned&&!card.passivePressed)openDetail(card.item); }, pal.card,
        { radius: 18, borderWidth: 0 });
      card.button.section = 'card';card.button.viewport=true;card.button.card=card;
      card.button._cardVisible = false;
      card.iconButton = createButton('', x + CARD_W - 68, y + 18, 48, 48,
        () => { if (card.item&&!quickGestureOwned) {if(layoutEditing)toggleCardSize(card.item);else toggleItem(card.item);} }, pal.card,
        { radius: 24, borderWidth: 0, enabled: 1 });
      card.iconButton.section = 'cardIcon';card.iconButton.viewport=true;card.iconButton.card=card;
      card.iconButton._cardVisible = false;
      card.moreLabel=card.unit;card.minusGlyph=null;card.plusGlyph=null;
      for(const direction of [-1,1]){
        const button=createButton('',0,0,40,44,()=>{
          if(isSceneCommand(card.item)){activateScene(card.item);return;}
          const main=card.item&&indexedEntity(card.item.entity_id),spec=card.quickSpec;
          if(main&&spec)requestFunction(main,spec,Number(controlValue(main,spec))+direction*spec.step);
        },pal.track,{radius:14,borderWidth:0});
        button.section='list';button.viewport=true;button.card=card;hide(button,true);if(direction<0)card.minus=button;else card.plus=button;
      }
      [card.moreLabel,card.minusGlyph,card.plusGlyph].forEach(id=>hide(id,true));
      [card.panel, card.iconBase, card.icon, card.powerHalo, card.powerGlyph, card.name, card.value, card.unit, card.state,
        card.track, card.fill,card.thumb].forEach(id => hide(id, true));
      out.cards.push(card);
      yield out;
    }

    out.footer = panel(GRID_X, H - FOOTER_H, GRID_W, FOOTER_H, pal.card, 0);
    out.footerLine = panel(GRID_X, H - FOOTER_H, GRID_W, 1, pal.line, 0);
    out.selectedName = label('', GRID_X + 16, H - FOOTER_H + 4, 278, 24, 15, pal.ink);
    out.selectedState = label('', GRID_X + 16, H - FOOTER_H + 28, 278, 24, 11, pal.muted);
    out.pager=createPageIndicator('list',target=>{
      quickDrag=null;quickGestureOwned=false;roomPage=target;render();
    });
    yield out;
    out.url = label('', GRID_X + 42, GRID_Y + 42, GRID_W - 84, 24, 16, pal.ink);
    out.secret = label('', GRID_X + 42, GRID_Y + 72, GRID_W - 84, 24, 14, pal.muted);
    out.transport = label('', GRID_X + 42, GRID_Y + 102, GRID_W - 84, 24, 14, pal.warn);
    const settingsWidth = Math.floor((GRID_W - 96) / 2);
    [
      [tr('\u670d\u52a1\u7aef'), promptUrl], [tr('\u8bbf\u95ee\u4ee4\u724c'), promptToken],
      [tr('\u5141\u8bb8 HTTP'), () => { httpAllowed = !httpAllowed; render(); }],
      [tr('\u5168\u90e8\u5b9e\u4f53'), () => {
        scope = scope === 'all' ? 'favorites' : 'all';
        save('eh_scope', scope);
        if (connected) { beginStates(); startQueued(); }
        render();
      }],
      [tr('\u6dfb\u52a0\u5173\u6ce8'), addFavorite], [tr('\u8fde\u63a5'), connect], [tr('\u65ad\u5f00'), disconnect]
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
    const dx=GRID_X+18,dy=HEADER_H+12,dw=GRID_W-36,dh=H-HEADER_H-36;
    const leftW=Math.min(224,Math.round(dw*.29)),cx=dx,cw=dw;
    expandButtonPool();out.detailWidgets=[];
    out.detailBox={x:cx,y:dy,w:cw,h:dh};
    out.detailMainBox={x:cx,y:dy,w:cw-leftW-24,h:dh};
    out.detailAuxBox={x:cx+cw-leftW-20,y:dy+84,w:leftW,h:dh-148};
    const remember=id=>{out.detailWidgets.push(id);return id;};
    out.detailPanel=remember(borderedPanel(cx,dy,cw,dh,dark?0x20262e:0xffffff,24));
    out.desktopPinFace=panel(W-160,10,144,40,pal.blueSoft,12);
    out.desktopPinLabel=label(tr('固定到桌面'),W-154,20,132,24,15,pal.blue);pushStyle(out.desktopPinLabel,'L',{center:1});

    pushStyle(out.detailPanel,'S',{radius:24,borderWidth:1,borderColor:dark?0x303845:pal.line});
    out.detailCrumb=remember(label('',dx+8,dy+8,leftW-16,28,20,pal.ink));
    out.detailName=remember(label('',cx+20,dy+16,cw-40,32,24,pal.ink));
    out.detailState=remember(label('',cx+20,dy+50,cw-40,24,14,pal.muted));
    out.detailLampPowerFace=remember(panel(cx+cw-60,dy+14,36,36,pal.track,18));
    out.detailMembers=[];
    for(let i=0;i<3;i++){
      const y=dy+54+i*76;
      const row={face:remember(panel(dx,y,leftW,66,pal.card,16)),name:remember(label('',dx+14,y+9,leftW-28,25,18,pal.ink)),
        state:remember(label('',dx+14,y+36,leftW-28,22,13,pal.muted)),item:null};
      row.fill=remember(panel(0,0,2,40,pal.blue,14));row.thumb=remember(panel(0,0,5,24,0xffffff,2));
      row.button=createButton('',dx,y,leftW,66,()=>detailRowAction(row,1),pal.card,{radius:16,borderWidth:0});
      row.button.section='detail';out.detailMembers.push(row);
    }
    const bottom=dy+dh-42;
    const navWidth=Math.floor((leftW-8)/2);
    out.detailPager=createPageIndicator('detail',target=>{controlPage=target;render();},remember);
    out.detailEntity=remember(label('',cx+20,bottom-24,cw-40,20,11,pal.muted));
    out.detailAvailability=remember(label('',cx+20,bottom,cw-212,30,13,pal.muted));
    out.detailMore=createButton('',W-162,H-44,144,40,()=>{cancelInputs(false);detailExpanded=!detailExpanded;controlPage=0;detailRenderKey='';render();},pal.page,{radius:12,borderWidth:0,fontSize:14});out.detailMore.section='detail';hide(out.detailMore,true);
    out.detailControlRows=[];
    out.detailControlCount=Math.max(1,Math.min(3,Math.floor((dh-146)/90)));
    out.detailAuxCount=out.detailControlCount;
    out.detailMemberCount=Math.max(1,Math.min(3,Math.floor((dh-100)/76)));
    const rowH=Math.min(102,Math.floor((dh-146)/out.detailControlCount));
    for(let i=0;i<3;i++){
      const y=dy+84+i*rowH,rx=cx+20,rw=out.detailMainBox.w-40;
      const row={x:rx,y,w:rw,h:rowH,item:null,spec:null};
      row.title=remember(label('',rx,y,rw,24,16,pal.ink));
      row.track=remember(panel(rx,y+30,rw,52,dark?0x303740:0xeef0f2,18));
      row.fill=remember(panel(rx,y+30,20,52,pal.blue,18));
      row.thumb=remember(panel(rx+2,y+40,6,32,0xffffff,3));
      row.value=remember(label('',rx+64,y+43,rw-128,28,18,pal.ink));pushStyle(row.value,'L',{center:1,ellipsis:1});
      row.minus=createButton('',rx,y+35,58,42,()=>detailRowAction(row,-1),pal.card,{radius:18,borderWidth:0});
      row.plus=createButton('',rx+rw-58,y+35,58,42,()=>detailRowAction(row,1),pal.card,{radius:18,borderWidth:0});
      row.minus.section=row.plus.section='detail';
      row.minusGlyph=remember(label('-',rx,y+43,58,28,20,pal.ink));pushStyle(row.minusGlyph,'L',{center:1});
      row.plusGlyph=remember(label('+',rx+rw-58,y+43,58,28,20,pal.ink));pushStyle(row.plusGlyph,'L',{center:1});
      out.detailControlRows.push(row);
    }

    out.detailDragGroup={item:null,brightnessRect:null,sliderRects:[],controlRects:[]};
    out.detailWidgets.forEach(id=>hide(id,true));
  }
  function detailRowAction(row,direction){
    const action=direction<0?row.minusAction:row.plusAction;if(action){action();return;}
    const item=row.item,spec=row.spec;if(!item||!spec||!available(item))return;
    if(spec.kind==='range'){const value=controlValue(item,spec);requestFunction(item,spec,(Number.isFinite(value)?value:spec.min)+direction*(spec.key==='brightness_pct'?10:spec.step));}
    else if(spec.kind==='options'){
      openOptionPicker(item,spec);
    }else if(spec.kind==='text'){
      const entity=item.entity_id,revision=generation;
      prompt.input({title:spec.title,value:item.state,maxLength:128},value=>{if(revision!==generation||value===null||value===undefined)return;requestFunction(indexedEntity(entity),spec,value);});
    }else requestFunction(item,spec);
  }
  function optionLabel(value){const names={day:tr('日间'),night:tr('夜间'),relax:tr('放松'),quiet:tr('安静'),silent:tr('静音'),sleep:tr('睡眠'),normal:tr('标准'),natural:tr('自然风'),boost:tr('强力'),eco:tr('节能'),comfort:tr('舒适'),low:tr('低'),medium:tr('中'),high:tr('高'),vertical:tr('上下'),horizontal:tr('左右'),both:tr('全向'),none:tr('无')};return value===true?tr('开启'):value===false?tr('关闭'):names[String(value).toLowerCase()]||STATE_WORDS[String(value)]||String(value);}
  function detailText(id,value,x,y,w,h,font,color,center){
    hide(id,false);pushPos(id,Math.round(x),Math.round(y));pushSize(id,Math.floor(w),Math.floor(h));
    pushStyle(id,'L',{fontSize:font,center:center?1:0,icon:0,ellipsis:1});pushColor(id,color);pushText(id,String(value));
  }
  function detailIcon(id,value,x,y,w,h,mode,color){
    // Icon fonts paint from the top; text alignment only centers horizontally.
    const pixels=mode===3?32:24;
    detailText(id,value,x,y+(h-pixels)/2,w,pixels+4,pixels,color,true);
    pushStyle(id,'I',{icon:mode,fontSize:pixels,center:1});
  }
  function paintDetailButton(button,caption,x,y,w,h,active,usable,font=14){
    const out=widgets;
    if(!button.face){
      button.face=panel(0,0,4,4,pal.track,12);button.caption=label('',0,0,4,4,16,pal.ink);
      out.detailWidgets.push(button.face,button.caption);
      (out.detailDecorations||(out.detailDecorations=[])).push(button.face,button.caption);
    }
    Object.assign(button,{x:Math.round(x),y:Math.round(y),w:Math.floor(w),h:Math.floor(h),color:active?pal.blueSoft:pal.track,
      style:{radius:12,borderWidth:0,fontSize:font,enabled:usable?1:0}});buttonText(button,caption);hide(button,!usable);
    hide(button.face,false);pushPos(button.face,x,y);pushSize(button.face,w,h);pushColor(button.face,button.color,usable&&paint['c'+button.face]!==undefined);
    detailText(button.caption,caption,x+4,y+(h-26)/2,w-8,26,font,!usable?pal.muted:active?pal.blue:pal.ink,true);
    if(typeof ui.front==='function'){ui.front(button.face);ui.front(button.caption);}
  }
  function renderDirectOptions(row,item,spec,x,y,w,h){
    const actions=spec.kind==='actions',presets=spec.kind==='presets',values=actions?spec.actions:spec.options;
    if(!actions&&!presets&&spec.kind!=='options'||!values||values.length>4)return false;
    const gap=6,width=(w-gap*(values.length-1))/values.length,font=14;
    const caption=(value,index)=>actions?value.title:presets?spec.labels[index]:optionLabel(value);
    if(values.some((value,index)=>fit(caption(value,index),width-12,font)!==caption(value,index)))return false;
    if(!row.optionButtons)row.optionButtons=Array.from({length:4},(_,index)=>{
      const button=createButton('',0,0,4,4,()=>{if(row.item&&row.spec){const current=indexedEntity(row.item.entity_id);if(row.spec.kind==='actions')requestFunction(current,row.spec.actions[index]);else requestFunction(current,row.spec.kind==='presets'?row.spec.target:row.spec,row.spec.options[index]);}},pal.track,{radius:12,borderWidth:0,fontSize:14});button.section='detail';hide(button,true);return button;
    });
    const value=controlValue(item,presets?spec.target:spec);
    row.optionButtons.forEach((button,index)=>{const visible=index<values.length;hide(button,!visible);hide(button.face,!visible);hide(button.caption,!visible);if(!visible)return;
      paintDetailButton(button,caption(values[index],index),x+index*(width+gap),y,width,h,!actions&&values[index]===value,connected&&available(item)&&!layoutEditing);
      if(actions){
        const code={open_cover:'\uf077',stop_cover:'\uf04d',close_cover:'\uf078',media_previous_track:'\uf048',media_play:'\uf04b',media_pause:'\uf04c',media_next_track:'\uf051',start:'\uf04b',pause:'\uf04c',return_to_base:'\uf015'}[values[index].service];
        if(code){const tw=typeof ui.measureText==='function'?measureTextCached(caption(values[index],index),font).width:caption(values[index],index).length*font;const offset=Math.max(2,(width-tw-25)/2);
          const glyph=detailExtra(button,'actionGlyph');detailIcon(glyph,code,button.x+offset,y,22,h,1,pal.ink);pushPos(button.caption,button.x+offset+25,y+(h-26)/2);pushSize(button.caption,Math.max(20,width-offset-27),26);pushStyle(button.caption,'L',{center:0});}
      }
    });
    return true;
  }
  function openOptionPicker(item,spec){
    if(!connected||!available(item)||layoutEditing)return;
    optionPicker={entity:item.entity_id,service:spec.service,key:spec.key,page:0,generation};render();
  }
  function renderOptionPicker(){
    if(!optionPicker)return;
    const item=indexedEntity(optionPicker.entity),spec=item&&functionControls(item).find(row=>row.service===optionPicker.service&&row.key===optionPicker.key);
    if(optionPicker.generation!==generation||!item||!spec||!available(item)||settingsOpen||!detailOpen){optionPicker=null;return;}
    const out=widgets,box=out.activeDetailBox||out.detailBox;if(!box){optionPicker=null;return;}
    if(!optionPickerUi){
      const make=(text,action)=>{const button=createButton(text,0,0,4,4,action,pal.track,{radius:12,borderWidth:0,fontSize:18});button.section='picker';hide(button,true);return button;};
      optionPickerUi={choices:Array.from({length:4},(_,index)=>make('',()=>{
        if(!optionPicker)return;const state=optionPicker,entity=indexedEntity(state.entity),current=entity&&functionControls(entity).find(row=>row.service===state.service&&row.key===state.key);
        const value=current&&current.options[state.page*4+index];optionPicker=null;detailRenderKey='';if(value!==undefined)requestFunction(entity,current,value);render();
      })),close:make(tr('返回'),()=>{optionPicker=null;detailRenderKey='';render();})};
      optionPickerUi.pager=createPageIndicator('picker',target=>{if(optionPicker){optionPicker.page=target;render();}},id=>out.detailWidgets.push(id));
    }
    out.detailWidgets.forEach(id=>hide(id,true));hide(out.detailPanel,false);hide(out.detailName,false);hide(out.detailState,false);
    pushPos(out.detailName,box.x+20,box.y+16);pushSize(out.detailName,box.w-40,36);
    pushPos(out.detailState,box.x+20,box.y+54);pushSize(out.detailState,box.w-40,24);
    pushText(out.detailName,spec.title);pushText(out.detailState,item.name+tr(' · 直接选择'));
    const pages=Math.ceil(spec.options.length/4);optionPicker.page=Math.min(optionPicker.page,pages-1);
    const rowH=Math.min(58,Math.floor((box.h-150)/4)),y=box.y+90;
    optionPickerUi.choices.forEach((button,index)=>{const value=spec.options[optionPicker.page*4+index];hide(button,value===undefined);if(value===undefined)return;
      paintDetailButton(button,optionLabel(value),box.x+20,y+index*rowH,box.w-40,rowH-8,value===controlValue(item,spec),true);});
    const bottom=box.y+box.h-52;
    hidePageIndicator(out.detailPager);
    updatePageIndicator(optionPickerUi.pager,optionPicker.page,pages,true,box.x+box.w/2,bottom,pal.card);
    Object.assign(optionPickerUi.close,{x:box.x+box.w-108,y:bottom,w:88,h:40});hide(optionPickerUi.close,false);
    paintDetailButton(optionPickerUi.close,tr('返回'),optionPickerUi.close.x,bottom,88,40,false,true);

  }
  function lampUi(){
    const out=widgets;if(out.lampUi)return out.lampUi;
    const keep=id=>{out.detailWidgets.push(id);hide(id,true);return id;};
    const u={buttons:[],swatches:[],entity:'',specs:[]};
    u.marker=keep(panel(0,0,14,14,0xffffff,7));pushStyle(u.marker,'S',{radius:7,borderWidth:2,borderColor:0x20262c});
    u.icon=keep(panel(0,0,50,50,dark?0x1f3b50:pal.blueSoft,18));
    u.glyph=keep(label('\uf0eb',0,0,50,50,32,pal.blue,1));
    u.paletteTitle=keep(label('',0,0,200,28,18,pal.ink));
    u.effectTitle=keep(label('',0,0,200,28,18,pal.ink));
    u.notice=keep(label('',0,0,200,44,14,pal.muted));
    u.preview=keep(panel(0,0,44,44,0xff962e,12));
    u.colorLabel=keep(label(tr('自选颜色'),0,0,150,32,18,pal.ink));
    const make=action=>{const b=createButton('',0,0,4,4,action,pal.track,{radius:12,borderWidth:0,fontSize:16});b.section='detail';hide(b,true);u.buttons.push(b);return b;};
    const decorate=b=>{b.face=keep(panel(0,0,4,4,pal.track,12));b.caption=keep(label('',0,0,4,4,16,pal.ink));return b;};
    for(let i=0;i<8;i++){
      const s={face:keep(panel(0,0,44,44,0xffffff,22)),value:null};
      s.button=make(()=>{
        const current=lookupEntity(u.entity);if(!current||!s.value)return;
        if(s.value.rgb)setLampRgb(u.entity,s.value.rgb);
        else requestFunction(current,functionControls(current).find(c=>c.key==='color_temp_kelvin'),s.value.kelvin);
      });u.swatches.push(s);
    }
    u.extra=decorate(make(()=>{lampAuxEntity=lampAuxEntity===u.entity?'':u.entity;detailRenderKey='';render();}));
    u.effects=Array.from({length:1},()=>decorate(make(()=>{
      const current=lookupEntity(u.entity),spec=current&&functionControls(current).find(c=>c.key==='effect');
      if(spec)openOptionPicker(current,spec);
    })));
    out.lampUi=u;return u;
  }
  function rgbHsv(rgb){
    const values=rgb.map(v=>v/255),max=Math.max(...values),min=Math.min(...values),delta=max-min;
    let h=0;if(delta){const i=values.indexOf(max);h=(i===0?(values[1]-values[2])/delta:i===1?2+(values[2]-values[0])/delta:4+(values[0]-values[1])/delta)*60;}
    return {h:(h+360)%360,s:max?delta/max:0};
  }
  function wheelRgb(rect,x,y){
    const dx=(x-rect.x-rect.w/2)/(rect.w/2-1),dy=(y-rect.y-rect.h/2)/(rect.h/2-1);
    const h=((Math.atan2(dy,dx)*180/Math.PI+90+360)%360)/60,s=Math.min(1,Math.hypot(dx,dy)),c=s,m=1-c;
    const v=c*(1-Math.abs(h%2-1)),parts=h<1?[c,v,0]:h<2?[v,c,0]:h<3?[0,c,v]:h<4?[0,v,c]:h<5?[v,0,c]:[c,0,v];
    return parts.map(value=>Math.round((value+m)*255));
  }
  function placeWheelMarker(u,rgb){
    if(!u.wheelRect)return;const r=u.wheelRect,hsv=rgbHsv(rgb),angle=(hsv.h-90)*Math.PI/180;
    const radius=hsv.s*(r.w/2-7);hide(u.marker,false);
    pushPos(u.marker,Math.round(r.x+r.w/2+Math.cos(angle)*radius-7),Math.round(r.y+r.h/2+Math.sin(angle)*radius-7),!quickDrag&&!lampWheelDrag&&paint['p'+u.marker]!==undefined);
    if(typeof ui.front==='function')ui.front(u.marker);
  }
  function handleLampWheel(state,nx,ny){
    const u=widgets&&widgets.lampUi,r=detailOpen&&u&&u.wheelRect;
    if(disposed||settingsOpen||optionPicker||!r||!Number.isFinite(nx)||!Number.isFinite(ny)){lampWheelDrag=null;return false;}
    const x=nx*W,y=ny*H;
    if(state==='down'){
      lampWheelDrag=null;
      if(Math.hypot(x-r.x-r.w/2,y-r.y-r.h/2)>r.w/2)return false;
      quickGestureOwned=true;quickDrag=null;
      const item=lookupEntity(r.entity);
      if(connected&&available(item)&&lampCanColor(item)&&!layoutEditing)lampWheelDrag={generation,entity:r.entity};
      else return true;
    }
    if(!lampWheelDrag)return false;
    if(lampWheelDrag.generation!==generation||lampWheelDrag.entity!==r.entity){lampWheelDrag=null;return true;}
    if(state==='down'||state==='move'){
      const rgb=wheelRgb(r,x,y);placeWheelMarker(u,rgb);pushColor(u.preview,rgbColor(rgb),!quickDrag&&!lampWheelDrag&&paint['c'+u.preview]!==undefined);
      pushText(u.colorLabel,'#'+rgbColor(rgb).toString(16).padStart(6,'0').toUpperCase());
    }
    if(state==='up'){const rgb=wheelRgb(r,x,y);lampWheelDrag=null;detailRenderKey='';setLampRgb(r.entity,rgb);render();}
    if(state==='cancel'){lampWheelDrag=null;detailRenderKey='';render();}
    return true;
  }
  function renderLampDetail(item,specs){
    const out=widgets,u=lampUi(),box=out.detailBox;
    const pad=detailDesign.padding,left=box.x+pad,top=box.y+pad+detailDesign.headerHeight+detailDesign.headerGap+1,gap=detailDesign.columnGap;
    const column=(box.w-2*pad-gap)*detailDesign.columnRatio/(detailDesign.columnRatio+1),rw=Math.min(detailDesign.primaryMax,Math.floor(column)),right=left+column+gap,auxW=box.x+box.w-pad-right;
    const ranges=specs.filter(s=>s.kind==='range').slice(0,2),temperature=specs.find(s=>s.key==='color_temp_kelvin');
    const effect=specs.find(s=>s.key==='effect'),power=specs.find(s=>s.kind==='action');
    const usable=connected&&available(item)&&!layoutEditing&&!item.product_readonly;
    const color=lampCanColor(item);
    u.entity=item.entity_id;
    const text=(id,value,x,y,w,h,font,ink)=>{hide(id,false);pushPos(id,x,y);pushSize(id,w,h);pushStyle(id,'L',{fontSize:font,center:0,ellipsis:1});pushColor(id,ink===undefined?pal.ink:ink);pushText(id,value);};
    const button=(b,caption,x,y,w,h,active)=>{
      Object.assign(b,{x:Math.round(x),y:Math.round(y),w:Math.floor(w),h:Math.floor(h),color:active?pal.blueSoft:pal.track,style:{radius:12,borderWidth:0,fontSize:16,enabled:usable?1:0}});buttonText(b,caption);hide(b,!usable);
      if(b.face){hide(b.face,false);pushPos(b.face,x,y);pushSize(b.face,w,h);pushColor(b.face,b.color,usable&&paint['c'+b.face]!==undefined);
        text(b.caption,caption,x,y+(h-24)/2,w,24,16,!usable?pal.muted:active?pal.blue:pal.ink);pushStyle(b.caption,'L',{fontSize:16,center:1});}
    };
    hide(u.icon,false);pushPos(u.icon,left,box.y+25);pushSize(u.icon,46,46);pushColor(u.icon,pal.blueSoft);pushStyle(u.icon,'S',{radius:15});
    detailIcon(u.glyph,'\uf0eb',left,box.y+25,46,46,1,pal.blue);
    text(out.detailName,fit(currentDeviceCard().name,box.w-180,23),left+58,box.y+20,box.w-160,33,23);
    const area=entityDevices[item.entity_id]&&entityDevices[item.entity_id].area;
    text(out.detailState,(area?area+' · ':'')+tr('设备功能'),left+58,box.y+56,box.w-160,22,14,pal.muted);
    out.detailControlRows.forEach(row=>{row.item=item;row.spec=null;[row.title,row.track,row.fill,row.thumb,row.value,row.minusGlyph,row.plusGlyph].forEach(id=>hide(id,true));});
    const rh=detailDesign.rangeHeight,rowGap=92+detailDesign.fieldGap;
    ranges.forEach((spec,index)=>{
      const row=out.detailControlRows[index],ty=top+index*rowGap,ry=ty+40;
      const value=controlValue(item,spec),v=Number.isFinite(value)?value:spec.min,percent=Math.max(0,Math.min(100,(v-spec.min)*100/(spec.max-spec.min))),warm=spec.key==='color_temp_kelvin';row.spec=spec;
      text(row.title,spec.title,left,ty,rw-104,31,detailDesign.label);
      const caption=formatControl(spec,value);
      text(row.value,caption,left+rw-104,ty-1,104,32,detailDesign.value);pushStyle(row.value,'L',{fontSize:detailDesign.value,center:0,rightAlign:1});
      let rect;
      if(warm){
        hide(row.fill,true);hide(row.track,false);pushPos(row.track,left,ry);pushSize(row.track,rw,rh);pushColor(row.track,0xffb16e);pushStyle(row.track,'S',{radius:18,borderWidth:0});pushStyle(row.track,'G',{gradientHorizontal:1,gradientVertical:0,gradientEnd:0xfffdfa,gradientMainStop:0,gradientEndStop:255});
        hide(row.thumb,false);pushPos(row.thumb,left+12+(rw-30)*percent/100,ry+12,!quickDrag&&!lampWheelDrag&&paint['p'+row.thumb]!==undefined);pushSize(row.thumb,6,28);pushStyle(row.thumb,'S',{radius:4,borderWidth:1,borderColor:0x6d8092});
        rect={x:left,y:ry,w:rw,h:rh,inputX:left+15,inputW:rw-30,thumbTrack:true,thumbH:28,fill:row.fill,thumb:row.thumb,label:row.value,valueOnly:true,labelColor:pal.ink,spec,item,gradient:true};
      }else rect=designSlider(row,row.track,row.value,item,spec,left,ry,rw,rh,percent,0xffa64e);
      out.detailDragGroup.sliderRects.push(rect);if(usable)out.detailDragGroup.controlRects.push(rect);
    });
    const owner=out.detailControlRows[0],px=box.x+box.w-64,py=box.y+25;
    hide(out.detailLampPowerFace,!power);pushPos(out.detailLampPowerFace,px,py);pushSize(out.detailLampPowerFace,46,46);pushStyle(out.detailLampPowerFace,'S',{radius:23});pushColor(out.detailLampPowerFace,item.state==='on'?(dark?0x204b39:0xdff5e7):pal.track,!quickDrag&&!lampWheelDrag&&paint['c'+out.detailLampPowerFace]!==undefined);
    if(power){
      detailIcon(owner.minusGlyph,'\uf011',px,py,46,46,1,item.state==='on'?0xb9f0d0:pal.muted);
      Object.assign(owner.minus,{x:px-1,y:py-1,w:48,h:48});buttonText(owner.minus,'');hide(owner.minus,!usable);
      owner.minusAction=()=>toggleItem(lookupEntity(item.entity_id));
    }
    const extras=deviceFunctions().filter(member=>member.entity_id!==item.entity_id);
    if(!extras.length||lampAuxEntity!==item.entity_id&&(temperature||color||effect))hidePageIndicator(out.detailPager);
    if(extras.length){
      const sideControls=temperature||color||effect;
      if(sideControls)button(u.extra,lampAuxEntity===item.entity_id?tr('返回灯光配色'):tr('其他设备功能'),right,box.y+box.h-44,auxW,34,false);
      if(lampAuxEntity===item.entity_id||!sideControls){
        out.detailAuxBox={x:right,y:top,w:auxW,h:box.h-172};
        out.detailAuxCount=out.detailControlCount;renderAuxFields(extras);return;
      }
    }
    let nextY=top;
    if(temperature||color){
      text(u.paletteTitle,temperature?(color?tr('常用色温 · 颜色'):tr('常用色温')):tr('常用颜色'),right,nextY,auxW,28,18);
      const values=[];
      if(temperature)for(let i=0;i<4;i++){
        const k=Math.round(temperature.min+(temperature.max-temperature.min)*i/3);values.push({kelvin:k,color:[0xffaf59,0xffd2af,0xffebd6,0xfffdfa][i]});
      }
      if(color)for(const rgb of [[127,172,255],[215,150,255],[255,158,243],[255,110,84]])values.push({rgb,color:rgbColor(rgb)});
      const dense=H<520&&color&&typeof ui.colorWheel==='function';
      const d=Math.min(dense?24:44,Math.floor((auxW-3*12)/4)),stride=d+(dense?8:12),sy=nextY+28;
      const presetEnd=sy+Math.ceil(values.length/4)*stride-(dense?8:12)+8;
      const directEffect=effect&&effect.options.length<=4&&effect.options.every(v=>fit(optionLabel(v),(auxW-6*(effect.options.length-1))/effect.options.length-12,14)===optionLabel(v));
      const reserved=(effect?directEffect?78:54:12)+(extras.length?38:0);
      u.swatches.forEach((s,index)=>{
        const value=values[index];s.value=value;if(!value)return;
        const x=right+index%4*stride,y=sy+Math.floor(index/4)*stride;
        const currentValue=temperature&&controlValue(item,temperature);
        const selected=value.rgb?validRgb(item.rgb_color)&&item.color_mode!=='color_temp'&&item.rgb_color.every((v,i)=>Math.abs(v-value.rgb[i])<8):item.color_mode!=='rgb'&&item.color_mode!=='hs'&&item.color_mode!=='xy'&&Math.abs(currentValue-value.kelvin)<60;
        hide(s.face,false);pushPos(s.face,x,y);pushSize(s.face,d,d);pushColor(s.face,value.color);pushStyle(s.face,'S',{radius:Math.ceil(d/2),borderWidth:selected?2:1,borderColor:selected?pal.blue:pal.track});
        button(s.button,'',x-2,y-2,d+4,d+4,false);s.button.style.radius=Math.ceil(d/2);
      });
      nextY=presetEnd;
      if(color&&typeof ui.colorWheel==='function'){
        const diameter=Math.max(64,Math.min(160,auxW-82,box.y+box.h-20-nextY-reserved-10));
        if(u.wheel===undefined){u.wheel=ui.colorWheel(right,nextY,Math.round(diameter));out.detailWidgets.push(u.wheel);}
        hide(u.wheel,false);pushPos(u.wheel,right,nextY);pushSize(u.wheel,Math.round(diameter),Math.round(diameter));
        u.wheelRect={x:right,y:nextY,w:diameter,h:diameter,entity:item.entity_id};
        text(u.colorLabel,tr('颜色选择'),right+diameter+12,nextY+diameter/2-13,auxW-diameter-12,28,16);
        hide(u.preview,false);pushPos(u.preview,right+diameter+12,nextY+diameter/2+23);pushSize(u.preview,28,18);pushColor(u.preview,rgbColor(lampRgb(item)),!quickDrag&&!lampWheelDrag&&paint['c'+u.preview]!==undefined);
        placeWheelMarker(u,lampRgb(item));nextY+=diameter+10;
      }
    }
    if(effect){
      const row=u.effectRow||(u.effectRow={});row.item=item;row.spec=effect;
      if(nextY+32+detailDesign.buttonHeight<=box.y+box.h-(extras.length?54:16)&&renderDirectOptions(row,item,effect,right,nextY+32,auxW,detailDesign.buttonHeight)){
        text(u.effectTitle,tr('灯光效果'),right,nextY,auxW,26,detailDesign.label,pal.ink);
        if(typeof ui.front==='function')ui.front(u.effectTitle);
      }else{
        button(u.effects[0],'',right,nextY,auxW,44,false);
        text(u.effectTitle,tr('灯光效果'),right+12,nextY+10,auxW-128,26,16,pal.muted);
        text(u.effects[0].caption,fit(optionLabel(controlValue(item,effect)||tr('选择')),106,16)+'  v',right+auxW-120,nextY+10,108,26,16);
        pushStyle(u.effects[0].caption,'L',{fontSize:16,center:1});
        if(typeof ui.front==='function'){ui.front(u.effectTitle);ui.front(u.effects[0].caption);}
      }
    }else if(!temperature&&!color){text(u.notice,tr('此灯具未提供色温、颜色或灯光效果'),right,top,auxW,60,14,pal.muted);}
  }

  function deviceFields(members){
    const card=currentDeviceCard(),profile=commonCardProfile(card);
    const readings=profileReadings(card),represented=new Set(readings.map(row=>row.source));
    const summary=readings.map(row=>({item:card,spec:{title:row.title,kind:'read',value:row.display,
      profileReading:true,alert:row.alert,key:row.key,numeric:row.numeric,number:row.value,unit:row.unit}}));
    const fields=[];
    members.forEach(item=>{
      const specs=profileControls(item);
      if(specs.length)specs.forEach(spec=>fields.push({item,spec}));
      else if(!represented.has(item.entity_id))fields.push({item,spec:{title:item.name,kind:'read',value:dashboardValue(item)}});
      if(isSceneCommand(item))fields.push({item,spec:{title:tr('触发记录'),kind:'read',value:sceneTimestamp(item)}});
    });
    return profile&&(profile.layout==='climate'||profileQuickControl(card))?fields.concat(summary):summary.concat(fields);
  }
  function renderAuxFields(members,supplied,totalPages){
    const out=widgets,box=out.detailAuxBox,fields=supplied||deviceFields(members);
    const count=out.detailAuxCount,pages=totalPages||Math.max(1,Math.ceil(fields.length/count));controlPage=Math.min(controlPage,pages-1);
    const visible=supplied||fields.slice(controlPage*count,controlPage*count+count);
    updatePageIndicator(out.detailPager,controlPage,pages,true,out.detailBox.x+out.detailBox.w/2,H-44,pal.page);
    const rowH=out.detailAuxRowHeight||Math.min(102,Math.floor(box.h/count));
    out.detailMembers.forEach((row,index)=>{
      const field=index<count?visible[index]:null;row.item=field&&field.item;row.spec=field&&field.spec;
      [row.face,row.name,row.state,row.fill,row.thumb].forEach(id=>hide(id,!field));hide(row.button,true);
      if(!field)return;
      const {item,spec}=field,y=box.y+index*rowH,value=controlValue(item,spec),range=spec.kind==='range';
      const usable=connected&&available(item)&&!layoutEditing&&spec.kind!=='read';
      pushPos(row.name,box.x,y);pushSize(row.name,box.w,24);pushStyle(row.name,'L',{fontSize:18,center:0,ellipsis:1});pushColor(row.name,pal.ink);
      pushText(row.name,spec.kind==='read'?spec.title:item.name+(range?' '+formatControl(spec,value):''));
      pushPos(row.face,box.x,y+30);pushSize(row.face,box.w,52);pushColor(row.face,dark?0x303740:pal.soft);pushStyle(row.face,'S',{radius:18});
      pushPos(row.state,box.x+8,y+43);pushSize(row.state,box.w-16,26);pushStyle(row.state,'L',{fontSize:18,center:1,ellipsis:1});pushColor(row.state,pal.ink);
      hide(row.fill,!range);hide(row.thumb,!range);hide(row.state,range);
      if(spec.kind==='read'){
        hide(row.face,true);pushStyle(row.name,'L',{fontSize:14,center:0});pushColor(row.name,pal.muted);
        pushPos(row.state,box.x,y+30);pushSize(row.state,box.w,40);pushStyle(row.state,'L',{fontSize:24,center:0,ellipsis:1});
        pushColor(row.state,spec.alert?pal.warn:pal.ink);pushText(row.state,String(value));return;
      }
      if(range){
        const v=Number.isFinite(value)?value:spec.min,percent=Math.max(0,Math.min(100,(v-spec.min)*100/(spec.max-spec.min)));
      const width=27+(box.w-27)*percent/100;
        pushPos(row.fill,box.x,y+30);pushSize(row.fill,width,52,!quickDrag&&paint['s'+row.fill]!==undefined);pushColor(row.fill,pal.blue);pushStyle(row.fill,'S',{radius:18});
        pushPos(row.thumb,box.x+12+(box.w-30)*percent/100,y+40,!quickDrag&&!lampWheelDrag&&paint['p'+row.thumb]!==undefined);pushSize(row.thumb,6,32);
        const rect={x:box.x,y:y+30,w:box.w,h:52,minFill:27,inputX:box.x+15,inputW:box.w-30,thumbTrack:true,
          fill:row.fill,thumb:row.thumb,thumbH:32,label:row.name,labelColor:pal.ink,labelName:item.name,spec,item};
        out.detailDragGroup.sliderRects.push(rect);if(usable)out.detailDragGroup.controlRects.push(rect);
      }else{
        if(renderDirectOptions(row,item,spec,box.x,y+30,box.w,52)){
          hide(row.face,true);hide(row.state,true);return;
        }
        pushText(row.state,spec.kind==='read'?String(value):spec.kind==='options'?String(value===true?tr('开启'):value===false?tr('关闭'):value||tr('未选择'))+'  >':spec.kind==='text'?tr('编辑内容'):spec.title);
        Object.assign(row.button,{x:box.x,y:y+30,w:box.w,h:52});hide(row.button,!usable);
      }
    });
  }

  /* HA_DETAIL_DESIGN_BEGIN */
  const detailDesign = {"padding":20,"minHeight":384,"headerHeight":54,"headerGap":22,"icon":46,"title":23,"columnGap":36,"primaryMax":400,"fieldGap":23,"label":15,"value":22,"labelGap":9,"rangeHeight":52,"rangeRadius":18,"thumbHeight":28,"buttonHeight":46,"buttonFont":14,"buttonGap":6,"stepHeight":58,"stepFont":30,"metricFont":43,"layouts":{"bedside":{"left":[["brightness","\u4eae\u5ea6"],["kelvin","\u8272\u6e29"]],"right":[["effect","\u706f\u5149\u6548\u679c"]],"readings":[]},"outlet":{"left":[["watt","\u5f53\u524d\u529f\u7387"]],"right":[["today","\u4eca\u65e5\u7528\u7535"],["voltage","\u7535\u538b"]],"readings":[]},"fan":{"left":[["speed","\u98ce\u901f"],["preset","\u8fd0\u884c\u6a21\u5f0f"]],"right":[["swing","\u6446\u98ce"]],"readings":[]},"climate":{"left":[["target","\u76ee\u6807\u6e29\u5ea6"],["mode","\u8fd0\u884c\u6a21\u5f0f"]],"right":[["fan","\u98ce\u901f"],["swing","\u6446\u98ce"]],"readings":[["room-temp","\u5ba4\u6e29"]]},"cover":{"left":[["position","\u5f00\u542f\u7a0b\u5ea6"],["move","\u7a97\u5e18\u63a7\u5236"]],"right":[["favorite","\u5e38\u7528\u4f4d\u7f6e"]],"readings":[]},"purifier":{"left":[["pm25","PM2.5"],["speed","\u98ce\u901f"]],"right":[["preset","\u51c0\u5316\u6a21\u5f0f"],["filter","\u6ee4\u82af\u4f59\u91cf"]],"readings":[]},"humidifier":{"left":[["humidity","\u76ee\u6807\u6e7f\u5ea6"],["current","\u5f53\u524d\u6e7f\u5ea6"]],"right":[["mode","\u52a0\u6e7f\u6a21\u5f0f"]],"readings":[]},"speaker":{"left":[["volume","\u97f3\u91cf"],["playback","\u64ad\u653e\u63a7\u5236"]],"right":[["mute","\u9759\u97f3"],["source","\u97f3\u6e90"]],"readings":[]},"vacuum":{"left":[["state","\u5f53\u524d\u72b6\u6001"],["clean","\u6e05\u626b\u63a7\u5236"]],"right":[["suction","\u5438\u529b"],["battery","\u7535\u91cf"]],"readings":[]},"heater":{"left":[["target","\u76ee\u6807\u6c34\u6e29"],["current","\u5f53\u524d\u6c34\u6e29"]],"right":[["mode","\u52a0\u70ed\u6a21\u5f0f"]],"readings":[]},"sensor":{"left":[["temperature","\u6e29\u5ea6"],["humidity","\u6e7f\u5ea6"]],"right":[["battery","\u7535\u91cf"],["state","\u8bbe\u5907\u72b6\u6001"]],"readings":[]}},"columnRatio":1.45};
  /* HA_DETAIL_DESIGN_END */
  function detailExtra(row,name,color){
    if(row[name]===undefined){row[name]=color===undefined?label('',0,0,4,4,15,pal.ink):panel(0,0,4,4,color,12);widgets.detailWidgets.push(row[name]);(widgets.detailDecorations||(widgets.detailDecorations=[])).push(row[name]);}
    return row[name];
  }
  function paintSliderTrack(track,percent,accent,thumb){
    const stop=Math.max(0,Math.min(255,Math.round(percent*255/100)));
    // Native stop interpolation keeps the fill edge and thumb on the UI cadence.
    if(typeof ui.range==='function'){
      delete paint['c'+track];delete paint['yG'+track];
      const key='r'+track,animate=!quickDrag&&paint[key]!==undefined;
      const linked=thumb!==undefined&&typeof ui.rangeLinked==='function';
      const value=stop+'|'+accent+'|'+pal.track+'|'+animate+'|'+(linked?thumb+'|'+paint['p'+track]+'|'+paint['s'+track]:'');
      if(paint[key]!==value){paint[key]=value;if(linked)ui.rangeLinked(track,stop,accent,pal.track,animate,thumb);else ui.range(track,stop,accent,pal.track,animate);}
      return;
    }
    // Equal gradient stops still paint an endpoint pixel in LVGL.
    pushColor(track,percent<=0?pal.track:accent);
    pushStyle(track,'G',{gradientHorizontal:1,gradientVertical:0,gradientEnd:percent>=100?accent:pal.track,gradientMainStop:stop,gradientEndStop:stop});
  }
  function designSlider(row,track,valueLabel,item,spec,x,y,w,h,percent,accent){
    hide(track,false);pushPos(track,x,y);pushSize(track,w,h);
    pushStyle(track,'S',{radius:h===48?16:detailDesign.rangeRadius,borderWidth:0});
    const linked=typeof ui.rangeLinked==='function';
    hide(row.fill,true);hide(row.thumb,false);if(!linked)pushPos(row.thumb,Math.round(x+12+(w-30)*percent/100),Math.round(y+(h-detailDesign.thumbHeight)/2),!quickDrag&&!lampWheelDrag&&paint['p'+row.thumb]!==undefined);pushSize(row.thumb,6,detailDesign.thumbHeight);pushColor(row.thumb,0xffffff);pushStyle(row.thumb,'S',{radius:4,borderWidth:0});
    // Linking requires siblings, including the first render of a lazy thumb.
    if(linked&&WATERFALL&&attached[track]!==undefined)attachToBoard(row.thumb,attached[track]);
    paintSliderTrack(track,percent,accent,row.thumb);
    // A recycled waterfall card can bring its track above an existing thumb.
    if(typeof ui.front==='function')ui.front(row.thumb);
    return {x,y,w,h,inputX:x+15,inputW:w-30,cssRange:true,track,accent,fill:row.fill,thumb:row.thumb,thumbTrack:true,thumbH:detailDesign.thumbHeight,label:valueLabel,valueOnly:true,labelColor:pal.ink,spec,item};
  }
  function renderProfileDetail(item,device,profile,members){
    const out=widgets,D=detailDesign,box=Object.assign({},out.detailBox,{h:Math.min(out.detailBox.h,D.minHeight)}),left=box.x+D.padding;
    const inner=box.w-2*D.padding,column=(inner-D.columnGap)*D.columnRatio/(D.columnRatio+1),width=Math.min(D.primaryMax,Math.floor(column)),right=left+column+D.columnGap,auxWidth=inner-column-D.columnGap;
    out.activeDetailBox=box;pushSize(out.detailPanel,box.w,box.h);pushColor(out.detailPanel,pal.card);pushStyle(out.detailPanel,'S',{radius:24,borderWidth:0});
    if(!out.profileUi){
      const keep=id=>{out.detailWidgets.push(id);return id;};
      const u={icon:keep(panel(0,0,D.icon,D.icon,pal.blueSoft,15)),glyph:keep(label('',0,0,D.icon,42,28,pal.blue,1)),powerGlyph:keep(label('',0,0,44,36,26,pal.muted,1)),entity:''};
      u.power=createButton('',0,0,48,48,()=>toggleItem(lookupEntity(u.entity)),pal.track,{radius:24,borderWidth:0});u.power.section='detail';out.profileUi=u;
    }
    const u=out.profileUi;u.entity=item.entity_id;
    hide(u.icon,false);pushPos(u.icon,left,box.y+25);pushSize(u.icon,D.icon,D.icon);pushColor(u.icon,pal.blueSoft);
    detailIcon(u.glyph,dashboardIcon(device),left,box.y+25,D.icon,D.icon,3,pal.blue);
    detailText(out.detailName,device.name,left+D.icon+12,box.y+20,box.w-160,33,D.title,pal.ink);
    const area=entityDevices[item.entity_id]&&entityDevices[item.entity_id].area;
    detailText(out.detailState,(area?area+' · ':'')+(profile.readonly?tr('传感器只读'):tr('设备功能')),left+D.icon+12,box.y+56,box.w-160,22,14,pal.muted);
    const all=deviceFields(members),used=new Set(),own=field=>field.item.entity_id===item.entity_id;
    const power=all.find(f=>own(f)&&f.spec.kind==='action'&&['turn_on','turn_off'].includes(f.spec.service));if(power)used.add(power);
    const usable=connected&&available(item)&&!layoutEditing&&!item.product_readonly,px=box.x+box.w-D.padding-44,py=box.y+25;
    hide(out.detailLampPowerFace,!power);hide(u.power,!power||!usable);hide(u.powerGlyph,!power);
    if(power){Object.assign(u.power,{x:px-2,y:py-2,w:48,h:48});pushPos(out.detailLampPowerFace,px,py);pushSize(out.detailLampPowerFace,44,44);pushStyle(out.detailLampPowerFace,'S',{radius:22});pushColor(out.detailLampPowerFace,powerIsOn(item)?dark?0x234936:0xdcf7e8:pal.track,!quickDrag&&!lampWheelDrag&&paint['c'+out.detailLampPowerFace]!==undefined);detailIcon(u.powerGlyph,'\uf011',px,py,44,44,1,powerIsOn(item)?dark?0xc7ffdf:0x13744d:pal.muted);}
    const find=key=>{const f=all.find(f=>!used.has(f)&&own(f)&&(f.spec.key||f.spec.service)===key);if(f)used.add(f);return f;};
    const reading=(key,title,unit)=>{
      const existing=all.find(f=>!used.has(f)&&f.spec.kind==='read'&&(f.spec.key===key||f.item.device_class===key));
      if(existing){used.add(existing);return Object.assign({},existing,{spec:Object.assign({},existing.spec,{title})});}
      const value=item[key];return typeof value==='number'&&Number.isFinite(value)?{item,spec:{title,kind:'read',value:available(item)?numberText(value)+(unit==='%'?'':' ')+unit:'--',numeric:true,number:numberText(value),unit}}:null;
    };
    const grouped=services=>{const actions=services.map(key=>find(key)).filter(Boolean).map(f=>f.spec);return actions.length?{item,spec:{kind:'actions',actions}}:null;};
    const layouts={light:'bedside',outlet:'outlet',switch:'outlet',relay:'outlet',fan:'fan','ceiling-fan':'fan','air-conditioner':'climate',curtain:'cover',airer:'cover','air-purifier':'purifier','air-fresh':'purifier',humidifier:'humidifier',diffuser:'humidifier',speaker:'speaker',vacuum:'vacuum','water-heater':'heater','temperature-humidity-sensor':'sensor'};
    const layout=layouts[quickCardType(device)],recipe=D.layouts[layout],main=[],side=[];
    const resolve=(id,title)=>{
      let field;
      const keys={speed:'percentage',fan:'fan_mode',preset:'preset_mode',target:'temperature',position:'position',humidity:'humidity',volume:'volume_level',mute:'is_volume_muted',source:'source',suction:'fan_speed',kelvin:'color_temp_kelvin',brightness:'brightness_pct'};
      if(layout==='sensor'&&['temperature','humidity'].includes(id))field=reading(id,title,id==='humidity'?'%':'°C');
      else if(keys[id])field=find(keys[id]);
      else if(id==='mode')field=find(item.domain==='climate'?'hvac_mode':item.domain==='water_heater'?'operation_mode':'mode');
      else if(id==='swing')field=find(item.domain==='fan'?'oscillating':'swing_mode');
      else if(id==='current'||id==='room-temp')field=reading(item.domain==='humidifier'?'current_humidity':'current_temperature',title,item.domain==='humidifier'?'%':'°C');
      else if(id==='pm25')field=reading('pm2.5-density',title,'ug/m3')||reading('pm25',title,'ug/m3');
      else if(id==='filter')field=reading('filter-life-level',title,'%');
      else if(id==='battery')field=reading('battery-level',title,'%')||reading('battery_level',title,'%')||reading('battery',title,'%');
      else if(id==='temperature'||id==='humidity')field=reading(id,title,id==='humidity'?'%':'°C');
      else if(id==='watt'||id==='voltage'||id==='today')field=reading(id==='watt'?'power':id==='today'?'daily_energy':'voltage',title,'');
      else if(id==='move')field=grouped(['open_cover','stop_cover','close_cover']);
      else if(id==='playback')field=grouped(['media_previous_track','media_play','media_pause','media_next_track']);
      else if(id==='clean')field=grouped(['start','pause','return_to_base']);
      else if(id==='state')field={item,spec:{kind:'read',status:true,value:available(item)?layout==='sensor'?tr('在线'):friendlyState(item):'--'}};
      else if(id==='favorite'){
        const target=functionControls(item).find(s=>s.key==='position');if(target)field={item,spec:{kind:'presets',target,options:[100,50,0],labels:[tr('全开'),tr('半开'),tr('全关')]}};
      }
      else if(id==='power'&&power)field={item,spec:{kind:'power',target:power.spec}};
      if(field)field.spec=Object.assign({},field.spec,{title});return field;
    };
    if(recipe){
      recipe.left.forEach(([id,title])=>{const f=resolve(id,title);if(f)main.push(f);});
      recipe.right.forEach(([id,title])=>{const f=resolve(id,title);if(f)side.push(f);});
      recipe.readings.forEach(([id,title])=>{const f=resolve(id,title);if(f){f.spec.compact=true;side.push(f);}});
      if(layout==='outlet'&&power)main.unshift({item,spec:{title:tr('电源'),kind:'power',target:power.spec}});
    }else if(profile.layout==='camera'){
      main.push({item,spec:{kind:'camera',title:tr('画面预览')}});all.filter(f=>f.spec.kind==='read').forEach(f=>{used.add(f);side.push(f);});
    }else{
      const fields=all.filter(f=>!used.has(f));fields.forEach(f=>used.add(f));
      if(fields.every(f=>f.spec.kind==='read')){const half=Math.ceil(fields.length/2);main.push(...fields.slice(0,half));side.push(...fields.slice(half));}
      else fields.forEach(f=>(profile.primary.includes(f.spec.key||f.spec.service)?main:side).push(f));
    }
    if(!main.length&&!side.length)main.push({item,spec:{title:tr('设备状态'),kind:'read',status:true,value:available(item)?friendlyState(item):'--'}});
    const extras=all.filter(f=>!used.has(f));
    const grid=!!profile.readonly&&profile.layout&&profile.layout!=='camera';
    const height=field=>field.spec.kind==='camera'?Math.round(width*9/16):field.spec.kind==='power'?156:field.spec.kind==='options'&&['oscillating','is_volume_muted'].includes(field.spec.key)?48:field.spec.kind==='read'?(field.spec.compact?52:grid?76:field.spec.status?81:96):field.spec.key==='temperature'?98:field.spec.kind==='range'?92:83;
    const bodyY=box.y+D.padding+D.headerHeight+D.headerGap+1,bodyHeight=box.h-(bodyY-box.y)-D.padding;
    const paginate=fields=>{const pages=[[]];let size=0;for(const f of fields){const h=height(f),gap=size?(grid?18:D.fieldGap):0;if(pages.at(-1).length===3||size+gap+h>bodyHeight&&size){pages.push([]);size=0;}pages.at(-1).push(f);size+= (size?(grid?18:D.fieldGap):0)+h;}return pages;};
    const leftPages=paginate(main),rightPages=paginate(side),basePages=Math.max(leftPages.length,rightPages.length);
    const extraPages=paginate(extras),pages=basePages+(detailExpanded&&extras.length?extraPages.length:0);controlPage=Math.min(controlPage,pages-1);
    hide(out.detailMore,!extras.length);buttonText(out.detailMore,detailExpanded?tr('收起参数'):tr('更多参数'));
    const paintRow=(row,field,x,y,w,aux)=>{
      row.item=field&&field.item;row.spec=field&&field.spec;
      const ids=aux?[row.face,row.name,row.state,row.fill,row.thumb]:[row.title,row.track,row.fill,row.thumb,row.value,row.minusGlyph,row.plusGlyph];ids.forEach(id=>hide(id,true));if(!field)return;
      const entity=field.item,spec=field.spec,value=controlValue(entity,spec),enabled=connected&&available(entity)&&!layoutEditing&&spec.kind!=='read';
      const title=aux?row.name:row.title,track=aux?row.face:row.track,valueLabel=aux?row.state:row.value;
      const toggle=spec.kind==='options'&&['oscillating','is_volume_muted','away_mode'].includes(spec.key);
      if(toggle){
        detailText(title,spec.title,x,y+11,w-146,28,16,pal.ink);
        detailText(valueLabel,value?tr('已开启'):tr('已关闭'),x+w-134,y+15,80,22,13,pal.muted,true);
        hide(track,false);pushPos(track,x+w-46,y+10);pushSize(track,46,28);pushColor(track,value?pal.blue:pal.track);pushStyle(track,'S',{radius:14,borderWidth:0});pushStyle(track,'G',{gradientHorizontal:0,gradientVertical:0});
        hide(row.thumb,false);pushPos(row.thumb,x+w-46+(value?21:3),y+13,!quickDrag&&!lampWheelDrag&&paint['p'+row.thumb]!==undefined);pushSize(row.thumb,22,22);pushStyle(row.thumb,'S',{radius:11,borderWidth:0});pushColor(row.thumb,value||!dark?0xffffff:0xbcc7d0);
        const b=aux?row.button:row.plus;Object.assign(b,{x,y,w,h:48,style:{radius:0,borderWidth:0,enabled:enabled?1:0}});buttonText(b,'');hide(b,!enabled);
        if(aux)row.plusAction=()=>requestFunction(lookupEntity(entity.entity_id),spec,!controlValue(entity,spec));else row.plusAction=()=>requestFunction(lookupEntity(entity.entity_id),spec,!controlValue(entity,spec));return;
      }
      if(spec.kind==='camera'){
        hide(track,false);pushPos(track,x,y);pushSize(track,w,Math.round(w*9/16));pushColor(track,pal.page);pushStyle(track,'S',{radius:18,borderWidth:0});pushStyle(track,'G',{gradientHorizontal:0,gradientVertical:0});
        detailIcon(valueLabel,'\ue002',x,y+w*9/32-47,w,64,3,pal.muted);detailText(title,tr('暂无预览画面'),x,y+w*9/16-41,w,24,13,pal.muted,true);
        if(typeof ui.front==='function'){ui.front(track);ui.front(valueLabel);ui.front(title);}return;
      }
      if(spec.kind==='power'){
        hide(track,false);pushPos(track,x,y+8);pushSize(track,136,136);pushStyle(track,'S',{radius:64,borderWidth:0});pushStyle(track,'G',{gradientHorizontal:0,gradientVertical:0});pushColor(track,powerIsOn(entity)?dark?0x234936:0xdcf7e8:pal.track);
        detailIcon(title,'\uf011',x,y+28,136,52,3,powerIsOn(entity)?dark?0xc7ffdf:0x13744d:pal.muted);detailText(valueLabel,powerIsOn(entity)?tr('已开启'):tr('已关闭'),x,y+88,136,28,16,powerIsOn(entity)?dark?0xc7ffdf:0x13744d:pal.muted,true);
        row.plusAction=()=>toggleItem(lookupEntity(entity.entity_id));Object.assign(row.plus,{x,y:y+8,w:136,h:136});hide(row.plus,!enabled);buttonText(row.plus,'');return;
      }
      detailText(title,spec.title,x,y,w,grid?24:31,spec.compact?13:D.label,spec.kind==='read'?pal.muted:pal.ink);
      if(spec.kind==='read'){
        const numeric=spec.profileReading?spec.numeric:spec.numeric||entity.domain==='sensor'&&Number.isFinite(Number(entity.state)),unit=spec.unit||numeric&&entity.unit||'';
        const raw=spec.number===undefined?numeric?String(entity.state):String(spec.value):spec.number;
        const ry=y+(spec.compact?21:grid?30:40),font=spec.compact?22:grid?32:spec.status?29:D.metricFont;
        const shown=available(entity)?numeric?String(Number(raw)):String(spec.value):'--';
        detailText(valueLabel,shown+(spec.compact&&unit?' '+unit:''),x,ry,w,spec.compact?32:52,font,spec.alert?pal.warn:pal.ink);
        if(numeric&&unit&&!spec.compact&&available(entity)){
          const measured=typeof ui.measureText==='function'?measureTextCached(shown,font):null;
          const uw=measured?measured.width:shown.length*font*.6;
          const unitMetrics=profile.layout==='camera'&&measured?measureTextCached(unit,15):null;
          const offset=unitMetrics?Math.max(0,measured.height-measured.baseline-unitMetrics.height+unitMetrics.baseline):Math.max(0,font-18);
          detailText(detailExtra(row,'unitLabel'),unit,x+uw+7,ry+offset,Math.max(20,w-uw-7),24,15,pal.muted);
        }
        if(spec.key==='filter-life-level'&&numeric){const meter=detailExtra(row,'meter',pal.track),fill=detailExtra(row,'meterFill',pal.blue);hide(meter,false);pushPos(meter,x,ry+54);pushSize(meter,w,8);hide(fill,false);pushPos(fill,x,ry+54);pushSize(fill,Math.max(1,w*Math.max(0,Math.min(100,Number(raw)))/100),8,paint['s'+fill]!==undefined);hide(fill,Number(raw)<=0);}
        return;
      }
      const ry=y+40;
      if(spec.kind==='range'){
        const v=Number.isFinite(value)?value:spec.min,percent=Math.max(0,Math.min(100,(v-spec.min)*100/(spec.max-spec.min))),stepper=spec.key==='temperature';
        const caption=formatControl(spec,value);
        detailText(valueLabel,caption,x+w-104,y-1,104,32,D.value,pal.ink);pushStyle(valueLabel,'L',{fontSize:D.value,center:0,rightAlign:1});pushSize(title,w-112,31);
        let rect;
        if(stepper){
          const rx=x+55,rw=w-110;hide(track,false);pushPos(track,rx,ry);pushSize(track,rw,D.stepHeight);pushColor(track,pal.page);pushStyle(track,'S',{radius:14,borderWidth:1,borderColor:pal.line});pushStyle(track,'G',{gradientHorizontal:0,gradientVertical:0});
          detailText(detailExtra(row,'stepValue'),numberText(value),rx,ry+9,rw,42,D.stepFont,pal.ink,true);
          paintDetailButton(row.minus,'-',x,ry,48,D.stepHeight,false,enabled,23);paintDetailButton(row.plus,'+',x+w-48,ry,48,D.stepHeight,false,enabled,23);
          rect={x:rx,y:ry,w:rw,h:D.stepHeight,inputX:rx,inputW:rw,noFill:true,extraValue:row.stepValue,fill:row.fill,label:valueLabel,valueOnly:true,labelColor:pal.ink,spec,item:entity};
        }else rect=designSlider(row,track,valueLabel,entity,spec,x,ry,w,D.rangeHeight,percent,pal.blue);
        out.detailDragGroup.sliderRects.push(rect);if(enabled)out.detailDragGroup.controlRects.push(rect);return;
      }
      if(renderDirectOptions(row,entity,spec,x,ry,w,D.buttonHeight))return;
      const b=aux?row.button:row.plus;paintDetailButton(b,spec.kind==='options'?optionLabel(value===null||value===undefined?tr('选择'):value)+'  >':spec.kind==='text'?tr('编辑内容'):spec.title,x,ry,w,D.buttonHeight,false,enabled);
    };
    const draw=(rows,fields,x,w,aux)=>{let y=bodyY;rows.forEach((row,index)=>{const f=fields[index];paintRow(row,f,x,y,w,aux);if(f)y+=height(f)+(grid?18:D.fieldGap);});};
    draw(out.detailControlRows,controlPage<basePages?leftPages[controlPage]||[]:extraPages[controlPage-basePages]||[],left,width,false);
    draw(out.detailMembers,controlPage<basePages?rightPages[controlPage]||[]:[],right,auxWidth,true);
    updatePageIndicator(out.detailPager,controlPage,pages,true,box.x+box.w/2,Math.min(H-40,box.y+box.h+2),pal.page);
  }

  function renderDetail(selected,visible){
    if(lampWheelDrag&&visible&&lampWheelDrag.generation===generation&&lampWheelDrag.entity===selectedId&&connected&&available(selected))return;

    const key=[visible,selected&&selected.entity_id,generation,entityRevision,controlRevision,controlPage,connected,layoutEditing].join('|');
    if(detailRenderKey===key)return;
    detailRenderKey=key;
    const out=widgets;
    hide(out.detailMore,true);
    if(visible&&selected&&out.motionEntity!==selected.entity_id){
      out.motionEntity=selected.entity_id;
      out.detailWidgets.forEach(id=>{delete paint['p'+id];delete paint['s'+id];delete paint['c'+id];delete paint['r'+id];});
    }
    out.detailWidgets.forEach(id=>hide(id,!visible));
    (out.detailDecorations||[]).forEach(id=>hide(id,true));
    if(out.profileUi){[out.profileUi.icon,out.profileUi.glyph,out.profileUi.powerGlyph,out.profileUi.power].forEach(id=>hide(id,true));}
    if(out.lampUi){out.lampUi.wheelRect=null;hide(out.lampUi.wheel,true);hide(out.lampUi.marker,true);}
    if(out.lampUi){out.lampUi.buttons.forEach(b=>{hide(b,true);hide(b.face,true);hide(b.caption,true);});[out.lampUi.icon,out.lampUi.glyph,out.lampUi.paletteTitle,out.lampUi.effectTitle,out.lampUi.notice,out.lampUi.preview,out.lampUi.colorLabel].concat(out.lampUi.swatches.map(s=>s.face)).forEach(id=>hide(id,true));}
    if(out.lampUi&&out.lampUi.effectRow)(out.lampUi.effectRow.optionButtons||[]).forEach(b=>hide(b,true));
    out.detailMembers.concat(out.detailControlRows).forEach(row=>(row.optionButtons||[]).forEach(button=>hide(button,true)));
    out.detailMembers.forEach(row=>{row.minusAction=row.plusAction=null;hide(row.button,!visible);});
    out.detailControlRows.forEach(row=>{row.minusAction=row.plusAction=null;hide(row.minus,true);hide(row.plus,true);});
    hide(out.detailLampPowerFace,true);
    if(!visible)hidePageIndicator(out.detailPager);
    out.detailAuxRowHeight=0;out.activeDetailBox=out.detailBox;pushSize(out.detailPanel,out.detailBox.w,out.detailBox.h);
    out.detailDragGroup={item:selected,brightnessRect:null,sliderRects:[],controlRects:[]};
    if(!visible)return;
    const device=currentDeviceCard(),members=deviceFunctions();
    const primary=lookupEntity(device.entity_id)||selected;
    selected=primary;selectedId=primary.entity_id;
    hide(out.detailCrumb,true);
    pushText(out.detailName,fit(device.name,out.detailBox.w-100,24));
    const intent=controlTargets[selected.entity_id];
    const profile=commonCardProfile(device),readings=profileReadings(device);
    const profileState=readings.find(row=>row.key==='$state');
    const detailState=intent?(intent.sent?tr('正在同步状态'):tr('待执行')):profileState?profileState.display:selected.domain==='light'?friendlyState(selected):dashboardSubtitle(selected);
    pushText(out.detailState,detailState+(profile&&profile.notice?' · '+profile.notice:''));
    pushColor(out.detailState,readings.some(row=>row.alert)?pal.warn:pal.muted);
    out.detailMembers.forEach(row=>{[row.face,row.name,row.state,row.fill,row.thumb].forEach(id=>hide(id,true));hide(row.button,true);});
    hide(out.detailEntity,true);hide(out.detailAvailability,true);
    const specs=functionControls(selected);
    pushPos(out.detailName,out.detailBox.x+20,out.detailBox.y+16);pushStyle(out.detailName,'L',{fontSize:24,center:0});
    pushPos(out.detailState,out.detailBox.x+20,out.detailBox.y+50);
    out.detailAuxBox={x:out.detailBox.x+out.detailBox.w-Math.min(224,Math.round(out.detailBox.w*.29))-20,y:out.detailBox.y+84,w:Math.min(224,Math.round(out.detailBox.w*.29)),h:out.detailBox.h-148};
    out.detailAuxCount=out.detailControlCount;
    if(selected.domain==='light'&&specs.length){renderLampDetail(selected,specs);return;}
    if(profile){renderProfileDetail(selected,device,profile,members);return;}
    pushSize(out.detailName,out.detailBox.w-100,32);
    const allFields=deviceFields(members).map((field,index)=>({field,index})).sort((a,b)=>{
      const rank=entry=>(entry.field.item.entity_id===primary.entity_id?0:10)+(entry.field.spec.kind==='range'?0:entry.field.spec.kind==='action'?1:entry.field.spec.kind==='options'?2:3);
      return rank(a)-rank(b)||a.index-b.index;
    }).map(entry=>entry.field);
    const readOnlyProfile=profile&&profile.layout&&allFields.every(field=>field.spec.kind==='read');
    const mainCount=readOnlyProfile?Math.min(out.detailControlCount,Math.ceil(allFields.length/2)):out.detailControlCount;
    const perPage=mainCount+out.detailAuxCount,fields=detailExpanded?allFields:allFields.slice(0,perPage),pages=Math.max(1,Math.ceil(fields.length/perPage));
    hide(out.detailMore,allFields.length<=perPage);buttonText(out.detailMore,detailExpanded?tr('收起参数'):tr('更多参数'));
    if(allFields.length>perPage){pushSize(out.detailName,out.detailBox.w-180,32);pushSize(out.detailState,out.detailBox.w-180,24);}
    controlPage=Math.min(controlPage,pages-1);
    const offset=controlPage*perPage;
    renderAuxFields([],fields.slice(offset+mainCount,offset+perPage),pages);
    const pageFields=fields.slice(offset,offset+mainCount);
    controlPage=Math.min(controlPage,pages-1);
    pushText(out.detailAvailability,!connected?tr('未连接'):!available(selected)?tr('设备不可用'):specs.length?tr('拖动调节 · ')+(controlPage+1)+'/'+pages:tr('传感器读数 · 只读'));
    out.detailControlRows.forEach((row,i)=>{
      const field=i<out.detailControlCount?pageFields[i]:null,spec=field&&field.spec;row.item=field&&field.item;row.spec=spec;
      [row.title,row.track,row.fill,row.thumb,row.value,row.minusGlyph,row.plusGlyph].forEach(id=>hide(id,!spec));
      if(!spec){if(i===0&&!specs.length){hide(row.title,false);pushPos(row.title,row.x,row.y);pushSize(row.title,row.w,48);pushStyle(row.title,'L',{fontSize:28,center:0});pushText(row.title,dashboardValue(selected));}return;}
      pushPos(row.title,row.x,row.y);pushSize(row.title,row.w,24);pushStyle(row.title,'L',{fontSize:16,center:0});
      pushPos(row.track,row.x,row.y+30);pushSize(row.track,row.w,52);pushStyle(row.track,'S',{radius:18});
      pushSize(row.thumb,6,32);pushStyle(row.thumb,'S',{radius:3});
      pushPos(row.value,row.x+64,row.y+43);pushSize(row.value,row.w-128,28);pushStyle(row.value,'L',{fontSize:18,center:1});
      pushColor(row.value,pal.ink);pushColor(row.minusGlyph,pal.ink);
      Object.assign(row.minus,{x:row.x,y:row.y+35,w:58,h:42});
      pushPos(row.minusGlyph,row.x,row.y+43);pushSize(row.minusGlyph,58,28);pushStyle(row.minusGlyph,'L',{fontSize:20,center:1,icon:0});
      const item=row.item,usable=connected&&available(item)&&!layoutEditing&&spec.kind!=='read'&&(!isSceneCommand(item)||!controlTargets[item.entity_id]),value=controlValue(item,spec),range=spec.kind==='range';
      pushText(row.title,fit(item.name+' · '+spec.title,row.w,16)+(range?' '+formatControl(spec,value):''));
      const temperature=spec.key==='color_temp_kelvin';
      hide(row.fill,!range||temperature);hide(row.thumb,!range);hide(row.value,range);
      if(profile&&profile.layout==='camera'&&i===0){
        const fw=Math.min(400,row.w),fh=Math.round(fw*9/16);
        [row.fill,row.thumb,row.minusGlyph,row.plusGlyph,row.minus,row.plus].forEach(id=>hide(id,true));
        hide(row.track,false);pushPos(row.track,row.x,row.y);pushSize(row.track,fw,fh);pushColor(row.track,pal.page);pushStyle(row.track,'S',{radius:18,borderWidth:0});
        detailIcon(row.value,'\ue002',row.x,row.y+Math.round(fh/2)-48,fw,64,3,pal.muted);
        hide(row.title,false);pushPos(row.title,row.x,row.y+fh-40);pushSize(row.title,fw,28);pushStyle(row.title,'L',{fontSize:16,center:1,icon:0});pushText(row.title,tr('暂无预览画面'));
        if(typeof ui.front==='function'){ui.front(row.value);ui.front(row.title);}return;
      }
      if(spec.kind==='read'){
        [row.track,row.fill,row.thumb,row.minusGlyph,row.plusGlyph,row.minus,row.plus].forEach(id=>hide(id,true));
        pushText(row.title,spec.title);pushStyle(row.title,'L',{fontSize:16,center:0});pushColor(row.title,pal.muted);
        const hero=readOnlyProfile&&i===0&&fields.length===1,solo=hero;
        const rx=solo?out.detailBox.x+20:row.x,rw=solo?out.detailBox.w-40:row.w;
        const ry=hero?out.detailBox.y+Math.max(90,(out.detailBox.h-100)/2):row.y;
        pushPos(row.title,rx,ry);pushSize(row.title,rw,28);pushStyle(row.title,'L',{fontSize:hero?20:16,center:solo?1:0});
        pushPos(row.value,rx,ry+38);pushSize(row.value,rw,60);pushStyle(row.value,'L',{fontSize:hero?40:32,center:solo?1:0,ellipsis:1});
        pushColor(row.value,spec.alert?pal.warn:pal.ink);pushText(row.value,String(spec.value));return;
      }
      const numeric=typeof value==='number'&&Number.isFinite(value)?value:spec.min;
      if(range){
        const percent=Math.max(0,Math.min(100,(numeric-spec.min)*100/(spec.max-spec.min))),width=20+(row.w-20)*percent/100;
        pushColor(row.track,temperature?temperatureColor(spec.min):dark?0x303740:0xeef0f2);pushColor(row.fill,pal.blue);
        pushStyle(row.track,'G',{gradientEnd:temperature?temperatureColor(spec.max):0xffffff,gradientHorizontal:temperature?1:0,gradientVertical:0});
        pushStyle(row.thumb,'G',{borderWidth:temperature?1:0,borderColor:0x65717f});
        pushSize(row.fill,Math.round(width),52,!quickDrag&&paint['s'+row.fill]!==undefined);pushPos(row.thumb,Math.round(row.x+width-18),row.y+40,!quickDrag&&!lampWheelDrag&&paint['p'+row.thumb]!==undefined);
        // Range gestures own the full track, including disabled controls.
        const rect={x:row.x,y:row.y+30,w:row.w,h:52,inputX:row.x+5,inputW:row.w-20,minFill:20,
          fill:row.fill,thumb:row.thumb,label:row.title,labelColor:pal.ink,spec,item,gradient:temperature};
        out.detailDragGroup.sliderRects.push(rect);if(usable)out.detailDragGroup.controlRects.push(rect);
        // Steppers are absent on the track; the drag target remains uninterrupted.
        hide(row.minusGlyph,true);hide(row.plusGlyph,true);
      }else{
        if(renderDirectOptions(row,item,spec,row.x,row.y+30,row.w,52)){
          [row.track,row.value,row.minusGlyph,row.plusGlyph].forEach(id=>hide(id,true));return;
        }
        pushColor(row.track,dark?0x2b3542:pal.blueSoft);pushText(row.value,spec.kind==='options'?String(value===true?tr('开启'):value===false?tr('关闭'):value||tr('未选择')):spec.kind==='read'?String(spec.value):spec.kind==='text'?tr('编辑内容'):tr('点按执行'));
        pushStyle(row.track,'G',{gradientHorizontal:0,gradientVertical:0});
        const options=spec.kind==='options';hide(row.minusGlyph,!options);hide(row.plusGlyph,false);
        pushText(row.minusGlyph,'<');pushText(row.plusGlyph,options?'>':tr('执行'));
        Object.assign(row.plus,{x:options?row.x+row.w-58:row.x,y:row.y+30,w:options?58:row.w,h:52});
        pushPos(row.plusGlyph,options?row.x+row.w-58:row.x,row.y+43);pushSize(row.plusGlyph,options?58:row.w,28);
        hide(row.value,!options&&spec.kind!=='read');pushColor(row.plusGlyph,pal.blue);
        hide(row.plusGlyph,spec.kind==='read');buttonText(row.plus,options?'':spec.title);
        row.plus.style.fontSize=18;
        hide(row.minus,!usable||!options);hide(row.plus,!usable);
      }
    });
  }

  function ensureDetailUi() {
    if (!widgets || widgets.detailWidgets || detailBuilding || disposed) return;
    detailBuilding = true;
    setTimeout(() => {
      if (disposed) return;
      if(!widgets.detailWidgets)buildDetailUi(widgets);
      detailBuilding = false;
      render();
    }, 20);
  }

  function renderNow() {
    if (!widgets || !uiReady || disposed) return;
    if (!canPaint()) { paintDeferred = true; return; }
    paintDeferred = false;
    validateInputs();
    // A pointer update paints only its slider. Background state sync may
    // finish, but must not rebuild hit targets or overwrite the live preview.
    if((quickDrag||lampWheelDrag)&&!settingsOpen&&!layoutEditing)return;
    let selected = detailOpen ? current() : currentDeviceCard();
    if (detailOpen && !selected) detailOpen = false;
    const listView = !settingsOpen && !detailOpen;
    const motionView=settingsOpen?'settings':detailOpen?'detail:'+selectedId+':'+(optionPicker?'picker:'+optionPicker.page:controlPage):'list:'+nav+':'+roomPage;
    if(widgets.motionView!==motionView){
      if(widgets.motionView!==undefined){
        const paging=listView&&widgets.motionView.startsWith('list:'+nav+':')&&widgets.motionPage!==roomPage;
        widgets.motionHorizontal=paging;
        widgets.motionPending=paging?(roomPage>widgets.motionPage?12:-12):listView?-12:12;
      }
      widgets.motionView=motionView;widgets.motionPage=roomPage;
    }
    const detailRequested = !settingsOpen && detailOpen && !!selected;
    const detailView = detailRequested && !!widgets.detailWidgets;
    const list = dashboardEntities();
    const boardPages=dashboardPages();const pages=boardPages.length;
    if (roomPage >= pages) roomPage = pages - 1;
    if(WATERFALL){
      hide(widgets.boardViewport,!listView||browseOpen);boardScroll=Math.max(0,Math.round(ui.scrollInfo(widgets.boardViewport).y));ui.scrollExtent(widgets.boardViewport,boardPages[0].height);
      if(boardAnchorRestore!==null){const target=boardAnchorRestore;boardAnchorRestore=null;if(target!==boardScroll){boardScroll=target;ui.scrollTo(widgets.boardViewport,target,false);}}
      const max=Math.max(0,boardPages[0].height-VIEW_H);
      if(boardScroll>max){boardScroll=max;ui.scrollTo(widgets.boardViewport,max,false);}
    }
    const boardRows=WATERFALL?boardWindow(boardPages[0].items):boardPages[roomPage].items;
    const windowKey=WATERFALL?boardRows.map(row=>row?row.item.entity_id:'').join(','):'';
    if(WATERFALL)widgets.boardWindowKey=windowKey;
    const sectionNames = {
      overview: tr('\u603b\u89c8'), devices: tr('\u8bbe\u5907'), scenes: tr('场景'), logs: tr('日志')
    };
    const nextCardKey = discoveryRevision+'|'+windowKey+'|'+entityRevision + '|' + nav + '|' + roomPage + '|' +
      selectedId + '|' + connected + '|' + (pending ? pending.kind : '') + '|' +
      !!queued + '|' + nativeBusy + '|' + (nav==='logs'?logSequence:0)+'|'+sizeRevision+'|'+layoutEditing+'|'+
      controlRevision+'|'+
      (widgets.quickGroups?widgets.quickGroups.length:0)+'|'+(settingsOpen ? 'settings' : detailRequested ? 'detail' : 'cards');
    if (nextCardKey !== cardRenderKey) {
      cardRenderKey = nextCardKey;
      cardRenderCursor = 0;
      cardRenderOrder=Array.from({length:SLOTS},(_,index)=>index);
      if(WATERFALL){
        const rank=index=>{const row=boardRows[index];if(!row)return 4;
          const visible=row.y+row.h>HEADER_H+boardScroll&&row.y<HEADER_H+boardScroll+VIEW_H;
          const retained=widgets.cards[index].item&&widgets.cards[index].item.entity_id===row.item.entity_id;
          return (visible?0:2)+(retained?1:0);};
        cardRenderOrder.sort((a,b)=>rank(a)-rank(b)||a-b);
      }
    }

    hide(widgets.sidebar, false); hide(widgets.brand, false); hide(widgets.note, false);
    pushText(widgets.note,groupedDeviceCards().length+tr(' 台设备 · ')+(connected?tr('已同步'):tr('未连接')));
    hide(widgets.header, false); hide(widgets.title, false);
    widgets.menu.forEach(row => {
      const active = row.key === (detailRequested?'devices':nav);
      hide(row.panel, row!==widgets.menu[0]||settingsOpen); hide(row.icon, settingsOpen); hide(row.text, settingsOpen);
      hide(row.button, settingsOpen);
      /* One retained highlight moves beneath the fixed labels and touch targets. */
      pushColor(row.icon, active ? sidebarStyle.accent : pal.muted);
      pushColor(row.text, active ? sidebarStyle.accent : sidebarStyle.ink);
    });
    const selection=widgets.menu[0].panel,activeRow=widgets.menu.find(row=>row.key===(detailRequested?'devices':nav))||widgets.menu[0];
    hide(selection,settingsOpen);pushColor(selection,sidebarStyle.selected);
    pushPos(selection,12,menuDrag?menuDrag.y:activeRow.button.y,false);
    widgets.menuSelectionVisible=!settingsOpen;
    const statistics=dashboardStats();
    widgets.stats.forEach((entry, index) => {
      hide(entry.dot, settingsOpen); hide(entry.text, settingsOpen);
      pushText(entry.text,statistics[index]);
    });
    pushText(widgets.title, settingsOpen ? tr('\u8fde\u63a5\u8bbe\u7f6e') :
      detailRequested ? tr('更多功能') : !connected?(pending||queued||nativeBusy?tr('同步中 · '):tr('未连接 · '))+sectionNames[nav]:sectionNames[nav]);
    pushText(widgets.status, connected ? tr('\u5df2\u540c\u6b65') : tr('\u672a\u8fde\u63a5'));
    if(listView){pushSize(widgets.title,Math.max(80,GRID_W-250),30);hide(widgets.status,true);}
    else pushSize(widgets.title,detailView?Math.max(80,GRID_W-350):280,30);
    pairHidden(widgets.back, !settingsOpen && !detailRequested);
    ensureDesktopPins();
    hide(widgets.desktopPin,!detailView);
    hide(widgets.followButton,!detailView);
    if(detailView){const card=currentDeviceCard();buttonText(widgets.followButton,card&&card.device_entities.some(id=>favorites.includes(id))?tr('取消关注'):tr('关注设备'));}
    [widgets.desktopPinFace,widgets.desktopPinLabel].forEach(id=>hide(id,!detailView));
    if(detailView)pushText(widgets.desktopPinLabel,desktopPins.includes(deviceCardKey(currentDeviceCard()))?tr('取消桌面固定'):desktopPins.length>=2?tr('桌面已固定两台'):tr('固定到桌面'));
    pairHidden(widgets.refresh, !listView);
    pairHidden(widgets.settingsButton, !listView);
    pairHidden(widgets.searchButton,!listView||nav==='logs');pairHidden(widgets.browseButton,!listView||nav==='logs');
    pushColor(widgets.searchButton.icon,search?pal.blue:pal.muted);pushColor(widgets.browseButton.icon,selectedArea||favoritesOnly?pal.blue:pal.muted);
    const empty=listView&&!browseOpen&&list.length===0;
    hide(widgets.emptyTitle,!empty);hide(widgets.emptyNote,!empty);hide(widgets.emptyAction,!empty);
    if(empty){
      const filtered=!!(search||selectedArea||favoritesOnly),loading=!!(pending||queued||nativeBusy);
      pushText(widgets.emptyTitle,filtered?tr('没有匹配的设备'):!connected?(loading?tr('正在同步设备'):tr('尚未连接家庭')):nav==='logs'?tr('暂无操作记录'):nav==='scenes'?tr('暂无场景'):tr('暂无设备'));
      pushText(widgets.emptyNote,filtered?tr('调整搜索或房间筛选，也可以清除全部筛选。'):!connected?(phase==='error'?fit(message,GRID_W-48,15):tr('连接家庭中枢后，这里会显示已同步的设备。')):nav==='logs'?tr('设备操作及同步结果会显示在这里。'):tr('刷新设备列表，或在家庭中枢中添加设备。'));
      buttonText(widgets.emptyAction,filtered?tr('清除筛选'):!connected?tr('连接设置'):tr('刷新列表'));
      if(typeof ui.front==='function'){ui.front(widgets.emptyTitle);ui.front(widgets.emptyNote);}
    }
    hide(widgets.layoutEdit,true);buttonText(widgets.layoutEdit,'');
    [widgets.layoutEditFace,widgets.layoutEditIcon,widgets.layoutEditLabel].forEach(id=>hide(id,true));
    pushColor(widgets.layoutEditFace,layoutEditing?pal.blue:pal.blueSoft);
    pushColor(widgets.layoutEditLabel,layoutEditing?0xffffff:pal.blue);pushColor(widgets.layoutEditIcon,layoutEditing?0xffffff:pal.blue);
    pushText(widgets.layoutEditLabel,layoutEditing?tr('完成编辑'):tr('编辑卡片'));

    const cardsNeedUpdate = cardRenderCursor < SLOTS;
    const cardStart = cardRenderCursor;
    const cardEnd = Math.min(SLOTS, cardStart + (WATERFALL?CARD_COLS:1));
    const cardWork=new Set(cardRenderOrder.slice(cardStart,cardEnd));
    widgets.cards.forEach((card, index) => {
      if (!cardWork.has(index)) return;
      const entry=listView?boardRows[index]||null:null;
      const item=entry?entry.item:null;
      card.item = item;
      card.brightnessRect=null;
      card.controlRects=[];
      card.sliderRects=[];
      card.passiveRegion=null;
      const hidden = !item;
      [card.panel, card.iconBase, card.icon, card.powerHalo, card.powerGlyph, card.name, card.value, card.unit,
        card.state, card.track, card.fill].forEach(id => hide(id, hidden));
      hide(card.thumb,true);
      hide(card.switchFace,true);
      [card.moreLabel,card.minusGlyph,card.plusGlyph,card.minus,card.plus,card.cameraGlyph].forEach(id=>hide(id,true));
      card.button._cardVisible = !!item;
      card.iconButton._cardVisible = !!item && controllable(item);
      hide(card.button, hidden);
      hide(card.iconButton, hidden || !controllable(item));
      if (!item) return;
      if(nav==='logs')renderActivityTile(card,item,entry);else renderUnifiedTile(card,item,entry);
      attachCard(card);
    });
    cardRenderCursor = cardEnd;
    if (cardsNeedUpdate && cardRenderCursor < SLOTS) renderAgain = true;

    hide(widgets.footer, !listView); hide(widgets.footerLine, !listView);
    hide(widgets.selectedName, !listView); hide(widgets.selectedState, !listView);
    const footerTextWidth = renderPageIndicator(listView, pages);
    pushSize(widgets.selectedName, footerTextWidth, 24);
    pushSize(widgets.selectedState, footerTextWidth, 24);
    pushStyle(widgets.selectedName, 'L', { ellipsis: 1 });
    pushStyle(widgets.selectedState, 'L', { ellipsis: 1 });
    pushText(widgets.selectedName,fit(nav==='logs'?tr('本次会话日志 · 最多 32 条'):nav==='scenes'?tr('场景与脚本'):search||selectedArea||favoritesOnly?list.length+tr(' 台 · ')+[selectedArea,favoritesOnly?tr('关注设备'):'',search?tr('搜索 ')+search:''].filter(Boolean).join(' · '):layoutEditing?tr('编辑卡片布局'):selected?selected.name:(WATERFALL?tr('上下滑动浏览 · 轻点更多功能'):tr('轻点卡片 · 更多操作')),footerTextWidth,15));
    const selectedTarget=selected&&controlTargets[selected.entity_id];
    pushText(widgets.selectedState,layoutEditing?(layoutSaveError?tr('布局未保存，请重试'):tr('选择尺寸后，点击完成编辑查看组合')):
      nav==='logs'?tr('仅记录本页面 · 关闭后清除'):nav==='scenes'?sceneStatusText(list.length):phase==='error'?message:selectedTarget?(selectedTarget.sent?tr('正在同步状态'):tr('待执行 · 前一设备仍在执行')):selected?fit(quickCardModel(selected).subtitle,278,11):message);

    if(widgets.detailWidgets)renderDetail(selected,detailView);

    hide(widgets.settingsPanel, !settingsOpen);
    [widgets.url, widgets.secret, widgets.transport].forEach(id => hide(id, !settingsOpen));
    widgets.settings.forEach(button => hide(button, !settingsOpen));
    if (settingsOpen) {
      pushText(widgets.url, tr('\u5bb6\u5ead\u4e2d\u67a2  \u00b7  ') + url);
      pushText(widgets.secret, nativeManaged ? tr('\u4ee4\u724c\uff1a\u7531\u672c\u5730\u670d\u52a1\u7ba1\u7406') :
        tr('\u4ee4\u724c\uff1a\u672a\u8f93\u5165'));
      pushText(widgets.transport, nativeManaged ? tr('\u5df2\u4f7f\u7528\u5df2\u9a8c\u8bc1\u7684\u672c\u5730 Home Assistant \u670d\u52a1') :
        tr('\u8fde\u63a5\u524d\u9700\u786e\u8ba4 HTTP \u98ce\u9669'));
      buttonText(widgets.settings[2], httpAllowed ? tr('HTTP \u5df2\u5141\u8bb8') : tr('\u5141\u8bb8 HTTP'));
      buttonText(widgets.settings[3], scope === 'favorites' ? tr('\u4ec5\u5173\u6ce8') : tr('\u5168\u90e8\u5b9e\u4f53'));
      buttonText(widgets.settings[5], connected ? tr('\u91cd\u65b0\u540c\u6b65') : tr('\u8fde\u63a5'));
    }
    const largeRows=listView?boardRows.filter(row=>row&&row.large):[];
    if(largeRows.length)ensureQuickGroups(largeRows.length);
    if(widgets.quickGroups)widgets.quickGroups.forEach((q,index)=>{
      if(listView&&largeRows[index])renderQuickCard(largeRows[index].item,true,largeRows[index],index);
      else renderQuickCard(null,false,null,index);
    });
    if(quickDrag&&quickDrag.generation===generation&&quickDrag.group.item&&
       (quickDrag.rect.item||quickDrag.group.item).entity_id===quickDrag.item.entity_id&&(quickDrag.group.brightnessRect||quickDrag.rect.spec))
      previewQuickBrightness(quickDrag,quickDrag.percent);
    if(optionPickerUi)Object.values(optionPickerUi).flat().forEach(button=>{if(button&&button.slots){if(!optionPicker)hidePageIndicator(button);}else hide(button,true);});
    renderOptionPicker();
    renderBrowse();
    renderButtons();
    if(widgets.motionPending&&cardRenderCursor>=SLOTS&&(!detailRequested||detailView)){
      if(!WATERFALL||!listView)presentContent(widgets.motionPending,widgets.motionHorizontal);widgets.motionPending=0;
    }
  }

  function presentContent(offset,horizontal){
    // All views open and change pages at their final positions.
  }
  function createPageIndicator(section,select,remember){
    const keep=id=>{if(remember)remember(id);hide(id,true);return id;};
    const pager={face:keep(panel(0,0,40,28,dark?0x2e303d:0xf3f2f7,14)),indicator:keep(panel(0,0,20,6,dark?0xeeeef5:0x292c38,3)),slots:[],keep};
    for(let index=0;index<5;index++){
      const slot={page:-1,mark:null};
      slot.button=createButton('',0,0,24,40,()=>{
        if(!pager.visible||slot.page<0||slot.page>=pager.total||slot.page===pager.current)return;
        select(slot.page);
      },pal.card,{radius:0,borderWidth:0});
      slot.button.section=section;hide(slot.button,true);pager.slots.push(slot);
    }
    return pager;
  }
  function hidePageIndicator(pager){
    if(!pager)return;pager.visible=false;pager.frontKey='';hide(pager.face,true);hide(pager.indicator,true);
    pager.slots.forEach(slot=>{slot.page=-1;hide(slot.button,true);hide(slot.mark,true);});
  }
  function updatePageIndicator(pager,current,total,visible,centerX,y,background){
    if(!pager)return;
    const wasVisible=pager.visible;
    visible=visible&&total>1;pager.visible=visible;pager.current=current;pager.total=total;
    const count=Math.min(pager.slots.length,total);
    const first=Math.max(0,Math.min(current-Math.floor(count/2),total-count));
    const width=16+count*24,x=Math.floor(centerX-width/2),frontKey=visible+'|'+buttonPool.length;
    hide(pager.face,!visible);pushSize(pager.face,width,28);pushPos(pager.face,x,y+6);
    if(visible&&pager.frontKey!==frontKey&&typeof ui.front==='function')ui.front(pager.face);
    pager.slots.forEach((slot,index)=>{
      const shown=visible&&index<count;slot.page=shown?first+index:-1;
      hide(slot.button,!shown);hide(slot.mark,!shown);if(!shown)return;
      const created=!slot.mark;if(created)slot.mark=pager.keep(panel(0,0,6,6,pal.muted,3));
      Object.assign(slot.button,{x:x+8+index*24,y,color:background});
      hide(slot.mark,false);pushPos(slot.mark,slot.button.x+9,y+17);pushSize(slot.mark,6,6);
      pushColor(slot.mark,dark?0x5c606e:0xc5c6ce);
      if((created||pager.frontKey!==frontKey)&&typeof ui.front==='function')ui.front(slot.mark);
    });
    // One retained pill follows the selected page; its underlying dots stay still.
    hide(pager.indicator,!visible);
    pushPos(pager.indicator,x+10+Math.max(0,Math.min(count-1,current-first))*24,y+17,wasVisible&&visible);
    pushSize(pager.indicator,20,6);pushColor(pager.indicator,dark?0xeeeef5:0x292c38);
    if(visible&&typeof ui.front==='function')ui.front(pager.indicator);
    pager.frontKey=frontKey;
  }
  function renderPageIndicator(listView,total){
    if(WATERFALL){hidePageIndicator(widgets.pager);return GRID_W-32;}
    updatePageIndicator(widgets.pager,roomPage,total,listView,GRID_X+GRID_W/2,H-FOOTER_H+7,pal.card);
    const width=16+Math.min(5,total)*24,x=Math.floor(GRID_X+(GRID_W-width)/2);
    return listView&&total>1?Math.max(1,Math.min(278,x-GRID_X-32)):278;
  }

  function continueRender() {
    renderTimer = 0;
    if (disposed) { renderRunning = false; return; }
    renderAgain = false;
    renderNow();
    publishDesktop();
    if (renderAgain) {
      system.yield().then(continueRender, () => { renderRunning = false; });
    } else {
      renderRunning = false;
    }
  }

  function render() {
    if (!widgets || disposed) return;
    if (!canPaint()) { paintDeferred = true; return; }
    renderAgain = true;
    if (renderRunning) return;
    renderRunning = true;
    renderTimer = setTimeout(continueRender, 20);
  }

  function openDetail(item) {
    cancelInputs(false);
    browseOpen=false;detailExpanded=false;
    recordUsage(item);
    layoutEditing=false;
    optionPicker=null;quickDrag=null;quickGestureOwned=false;controlPage=0;
    selectedId = item.entity_id;
    settingsOpen = false;
    detailOpen = true;
    boardLock(false);
    ensureDetailUi();
    render();
  }

  function toggleItem(item) {
    if(layoutEditing)return;
    if(isSceneCommand(item))return;
    if(!controllable(item))return;
    item=indexedEntity(item.entity_id)||item;
    selectedId = item.entity_id;
    if (!controllable(item) || !connected || !available(item)) {
      render();
      return;
    }
    const on=powerIsOn(item);
    const service = on ? 'turn_off' : 'turn_on';
    if (!supports(item, service)) { render(); return; }
    requestControl(item,service);
  }
  function powerIsOn(item){
    const target=item&&controlTargets[item.entity_id];
    if(target&&['turn_on','turn_off'].includes(target.service))return target.service==='turn_on';
    return !!item&&(['climate','media_player','water_heater'].includes(item.domain)?!['off','unavailable','unknown'].includes(item.state):item.state==='on');
  }

  function snapshot() {
    return {
      phase, message, url, connected, hasToken: !!token, httpAllowed, scope,
      backend: sharedReady ? 'native-service' :
        nativeService ? 'unsupported-service' : 'legacy',
      nativeManaged,
      desktopPins:desktopPins.slice(),desktopCards:desktopCards().map(card=>({entity:card.entity_id,key:deviceCardKey(card),name:card.name,pinned:desktopPins.includes(deviceCardKey(card))})),
      favorites: favorites.slice(), filter, search, page, nav, roomPage, layoutStyle,
      sidebarCursor:widgets?{dragging:!!menuDrag,y:menuDrag?menuDrag.y:(widgets.menu.find(row=>row.key===(detailOpen?'devices':nav))||widgets.menu[0]).button.y}:null,
      discovery:{area:selectedArea,favoritesOnly,open:browseOpen,page:browsePage,rooms:browseOpen?roomChoices():[],count:dashboardEntities().length},detailExpanded,
      pageIndicator:widgets&&widgets.pager?{visible:!settingsOpen&&!detailOpen&&pageCount()>1,
        total:pageCount(),slots:widgets.pager.slots.filter(slot=>slot.page>=0).map(slot=>({
          page:slot.page,active:slot.page===roomPage,x:slot.button.x,y:slot.button.y,w:24,h:40}))}:null,
      detailPageIndicator:widgets&&widgets.detailPager?{visible:widgets.detailPager.visible,
        current:controlPage,total:widgets.detailPager.total,slots:widgets.detailPager.slots.filter(slot=>slot.page>=0).map(slot=>({page:slot.page,x:slot.button.x,y:slot.button.y,w:24,h:40}))}:null,
      optionPageIndicator:optionPickerUi&&optionPickerUi.pager?{visible:optionPickerUi.pager.visible,
        total:optionPickerUi.pager.total,slots:optionPickerUi.pager.slots.filter(slot=>slot.page>=0).map(slot=>({page:slot.page,x:slot.button.x,y:slot.button.y,w:24,h:40}))}:null,
      functionControls:detailOpen?deviceFields(deviceFunctions()).map(f=>({entity:f.item.entity_id,title:f.spec.title,key:f.spec.key,kind:f.spec.kind,service:f.spec.service,min:f.spec.min,max:f.spec.max,options:f.spec.options})):[],
      colorWheel:widgets&&detailOpen&&widgets.lampUi&&widgets.lampUi.wheelRect?Object.assign({},widgets.lampUi.wheelRect):null,
      parameterControls:widgets?([widgets.detailDragGroup].concat(widgets.quickGroups||[],widgets.cards||[])).filter(Boolean).flatMap(q=>(q.controlRects||[]).map(r=>({entity:(r.item||q.item).entity_id,key:r.spec.key,vertical:!!r.vertical,x:r.inputX||r.x,y:r.y,w:r.inputW||r.w,h:r.h,min:r.spec.min,max:r.spec.max}))):[],
      settingsOpen, detailOpen, detailAdvanced:detailOpen, layoutEditing, layoutSaveError,selectedId, limited, serviceDiscoveryLimited, lastSync, lastCached,
      optionPicker:optionPicker?{entity:optionPicker.entity,key:optionPicker.key,page:optionPicker.page}:null,
      nextRefresh, nativeBusy, disposed, uiReady,
      view: settingsOpen ? 'settings' : detailOpen ? 'detail' : 'list',
      pending: pending ? { kind: pending.kind, entityId: pending.entityId,
        abandoned: !!pending.abandoned } : null,
      registeredCardTypes: Object.keys(quickCardRegistry),
      registeredCommonCardProfiles:commonCardProfiles.map(profile=>({id:profile.id,name:profile.name,types:profile.types,quick:profile.quick,readonly:!!profile.readonly})),
      registeredCardVariants: Object.values(quickReferenceRules).reduce((n,rules)=>n+rules.length,0),
      deviceRegistryReady,
      sceneSyncReady,sceneTotal,sceneSyncError,sceneSyncAt,scenes:sceneEntities.map(row=>Object.assign({},row)),
      activityLog:activityLog.map(row=>Object.assign({},row)),
      brightnessInput:quickDrag?{dragging:true,entity:quickDrag.item.entity_id,percent:quickDrag.percent}:
        brightnessIntent?{dragging:false,entity:brightnessIntent.entity,percent:brightnessIntent.percent,pending:!brightnessIntent.sent}:null,
      controlQueue:controlQueue.map(command=>({entity:command.entity,service:command.service,brightness:command.brightness})),
      controlProgress:Object.keys(controlTargets).map(entity=>({entity,sent:!!controlTargets[entity].sent,slow:!!controlTargets[entity].slow})),
      controlFeedback:Object.keys(controlFeedback).map(entity=>({entity,kind:controlFeedback[entity].kind,text:controlFeedback[entity].text})),
      brightnessControls:[widgets&&widgets.detailDragGroup].concat(widgets&&widgets.quickGroups||[],widgets&&widgets.cards||[])
        .filter(q=>q&&q.item&&q.brightnessRect).map(q=>({entity:q.item.entity_id,
          x:q.brightnessRect.inputX===undefined?q.brightnessRect.x:q.brightnessRect.inputX,
          y:q.brightnessRect.y,w:q.brightnessRect.inputW===undefined?q.brightnessRect.w:q.brightnessRect.inputW,h:q.brightnessRect.h,
          value:displayBrightness(q.item)})),
      deviceCards:groupedDeviceCards().map(card=>({card_id:card.card_id,device_id:card.device_id,
        entity_id:card.entity_id,name:card.name,model:card.device_model,readonly:!!card.product_readonly,
        members:card.device_entities,quickReference:selectQuickReference(card),cardSize:cardSize(card),commonProfile:commonCardProfile(card)?.id||null,
        readings:profileReadings(card).map(row=>({key:row.key,title:row.title,display:row.display,source:row.source,alert:row.alert}))})),
      waterfall:WATERFALL?{viewport:widgets&&widgets.boardViewport,x:GRID_X,y:HEADER_H,w:GRID_W,h:VIEW_H,columns:CARD_COLS,scrollY:boardScroll,contentHeight:dashboardPages()[0].height,poolSlots:SLOTS,
        visibleCards:widgets?widgets.cards.filter(card=>card.item).map(card=>{const row=dashboardPages()[0].items.find(row=>row.item.entity_id===card.item.entity_id);return row?{entity_id:row.item.entity_id,x:row.x,y:row.y-boardScroll,w:row.w,h:row.h}:null;}).filter(Boolean):[]}:null,
      layoutPages:dashboardPages().map(p=>p.items.map(r=>({entity_id:r.item.entity_id,size:r.large?'large':'small',x:r.x,y:r.y,w:r.w,h:r.h}))),
      entities: entities.map(row => Object.assign({}, row, {cardSize:cardSize(row),cardStyle:quickCardModel(row).variant,
        quickReference:selectQuickReference(row)}))
    };
  }

  if (W < 640 || H < 400) {
    ui.background(pal.page);
    label(tr('米家 HA'), 12, 12, Math.max(1, W - 24), 28, 20, pal.ink);
    label(tr('显示区域至少需要 640 × 400'), 12, 56, Math.max(1, W - 24), 64, 16, pal.muted);
    return;
  }
  const uiCreation = buildUiReference();
  widgets = uiCreation.next().value;
  function resumeUiCreation() {
    if (disposed) return;
    if (!canPaint()) { creationDeferred = true; return; }
    creationDeferred = false;
    const next = uiCreation.next();
    widgets = next.value;
    if (next.done) { uiReady = true; render(); }
    else setTimeout(resumeUiCreation, 20);
  }
  setTimeout(resumeUiCreation, 20);
  if (!bridgeReady) message = tr('固件未提供 Home Assistant 接口');
  renderNow();
  if (nativeManaged && bridgeReady) connect();
  if(typeof ui.onBack==='function')ui.onBack(backOneLevel);
  ui.onSwipe(direction => {
    if(WATERFALL)return;
    if (disposed || settingsOpen || detailOpen || optionPicker || quickGestureOwned) return;
    const pages = pageCount();
    if (direction === 'left' && roomPage + 1 < pages) roomPage++;
    else if (direction === 'right' && roomPage > 0) roomPage--;
    render();
  });
  timer = setInterval(tick, 250);
  function previewQuickBrightness(drag,percent) {
    const r=drag.rect;drag.percent=percent;
    const width=(r.minFill||0)+(r.w-(r.minFill||0))*percent/100;
    const height=(r.minFill||0)+(r.h-(r.minFill||0))*percent/100;
    if(r.cssRange)paintSliderTrack(r.track,percent,r.accent,r.thumb);
    if(!r.gradient&&!r.noFill&&!r.cssRange){
      if(r.vertical){pushSize(r.fill,r.w,Math.round(height),!quickDrag&&paint['s'+r.fill]!==undefined);pushPos(r.fill,r.x,Math.round(r.y+r.h-height));}
      else pushSize(r.fill,Math.max(1,Math.round(width)),Math.round(r.h),!quickDrag&&paint['s'+r.fill]!==undefined);hide(r.fill,percent===0);
    }
    pushText(r.label,(r.spec?(r.valueOnly?'':(r.labelName||r.spec.title)+' ')+formatControl(r.spec,rangeValue(r,percent)):(r.labelPrefix===undefined?tr('亮度 '):r.labelPrefix)+percent+'%')+(r.labelSuffix||''));
    if(r.extraValue!==undefined)pushText(r.extraValue,numberText(rangeValue(r,percent)));
    pushColor(r.label,r.labelColor===undefined?(percent>30?0xffffff:quickPalette.muted):r.labelColor);
    if(r.thumb&&!(r.cssRange&&typeof ui.rangeLinked==='function'))pushPos(r.thumb,r.vertical?Math.round(r.x+(r.w-40)/2):Math.round(r.thumbTrack?r.x+12+(r.w-30)*percent/100:r.x+width-(r.thumbInset||18)),r.vertical?Math.round(r.y+r.h-height+10):Math.round(r.y+(r.h-(r.thumbH||32))/2),!quickDrag&&!lampWheelDrag&&paint['p'+r.thumb]!==undefined);
  }
  function canAdjustBrightness(item) {
    return !!item&&item.domain==='light'&&item.dimmable&&!item.product_readonly&&!layoutEditing&&
      connected&&available(item)&&supports(item,'turn_on');
  }
  function displayBrightness(item) {
    const target=controlTargets[item.entity_id];
    if(target&&target.generation===generation){
      if(target.brightness!==undefined)return target.brightness;
      if(target.service==='turn_off')return 0;
      if(target.service==='turn_on')return item.brightness===null?0:item.brightness;
    }
    return item.state==='off'||item.brightness===null?0:item.brightness;
  }
  function renderCardEditor(card,item,entry) {
    const {x,y,w,h}=entry,large=cardSize(item)==='large';
    [card.iconBase,card.powerHalo,card.powerGlyph,card.unit,card.fill,card.thumb,card.switchFace].forEach(id=>hide(id,true));
    pushPos(card.panel,x,y);pushSize(card.panel,w,h);pushColor(card.panel,pal.card);
    pushStyle(card.panel,'S',{radius:18,borderWidth:1,borderColor:pal.line});
    hide(card.icon,false);pushPos(card.icon,x+16,y+18);pushSize(card.icon,40,40);
    pushStyle(card.icon,'I',{fontSize:32,icon:1,center:1});pushText(card.icon,dashboardIcon(item));pushColor(card.icon,pal.device);
    hide(card.name,false);pushPos(card.name,x+16,y+66);pushSize(card.name,w-32,30);
    pushStyle(card.name,'L',{fontSize:22,ellipsis:1});pushText(card.name,fit(item.name,w-32,22));pushColor(card.name,pal.ink);
    hide(card.value,false);pushPos(card.value,x+16,y+100);pushSize(card.value,w-32,24);
    pushStyle(card.value,'L',{fontSize:14,ellipsis:1});pushText(card.value,large?tr('当前：大卡片'):tr('当前：小卡片'));pushColor(card.value,pal.muted);
    hide(card.track,false);pushPos(card.track,x+16,y+h-50);pushSize(card.track,w-32,36);
    pushColor(card.track,pal.blueSoft);pushStyle(card.track,'S',{radius:10,borderWidth:0});
    hide(card.state,false);pushPos(card.state,x+16,y+h-43);pushSize(card.state,w-32,24);
    pushStyle(card.state,'L',{fontSize:16,center:1});pushText(card.state,large?tr('设为小卡'):tr('设为大卡'));pushColor(card.state,pal.blue);
    if(typeof ui.front==='function')ui.front(card.state);
    // Reuse the card's hit target; layout editing never sends device commands.
    hide(card.button,true);card.button._cardVisible=false;
    Object.assign(card.iconButton,{x:x+16,y:y+h-50,w:w-32,h:36});
    card.iconButton.style.enabled=1;card.iconButton._cardVisible=true;hide(card.iconButton,false);
  }
  function renderActivityTile(card,item,entry){
    const {x,y,w,h}=entry;
    [card.iconBase,card.icon,card.powerHalo,card.powerGlyph,card.unit,card.track,card.fill,card.thumb,card.switchFace,card.moreLabel,card.minusGlyph,card.plusGlyph].forEach(id=>hide(id,true));
    card.button._cardVisible=card.iconButton._cardVisible=false;
    [card.button,card.iconButton,card.minus,card.plus].forEach(button=>hide(button,true));
    pushPos(card.panel,x,y);pushSize(card.panel,w,h);pushColor(card.panel,pal.card);pushStyle(card.panel,'S',{radius:20,borderWidth:0});
    hide(card.name,false);pushPos(card.name,x+16,y+18);pushSize(card.name,w-32,28);pushStyle(card.name,'L',{fontSize:20,ellipsis:1});pushText(card.name,item.name);
    hide(card.value,false);pushPos(card.value,x+16,y+60);pushSize(card.value,w-32,64);pushStyle(card.value,'L',{fontSize:16,ellipsis:1,center:0});pushColor(card.value,pal.ink);pushText(card.value,fit(item.detail,w-32,16));
    hide(card.state,false);pushPos(card.state,x+16,y+h-35);pushSize(card.state,w-32,26);pushStyle(card.state,'L',{fontSize:14,center:0});pushColor(card.state,pal.muted);pushText(card.state,item.time);
  }
  function bindCardMotion(card,item,entry){
    const layout=entry.x+','+entry.y+','+entry.w+','+entry.h;
    if(card.motionEntity===item.entity_id&&card.motionLayout===layout)return;
    card.motionEntity=item.entity_id;card.motionLayout=layout;
    [card.thumb,card.fill].forEach(id=>{if(id!==undefined){delete paint['p'+id];delete paint['s'+id];}});
    [card.track].forEach(id=>{if(id!==undefined)delete paint['r'+id];});
    [card.switchFace,card.powerHalo].forEach(id=>{if(id!==undefined)delete paint['c'+id];});
  }
  function renderUnifiedTile(card,item,entry) {
    bindCardMotion(card,item,entry);
    const {x,y,w,h}=entry,main=lookupEntity(item.entity_id)||item;
    const model=quickCardModel(item),light=item.domain==='light',candidate=profileQuickControl(item);
    const spec=light&&candidate&&candidate.key==='brightness_pct'?null:candidate;
    const brightness=light&&item.dimmable&&!item.product_readonly,range=!!spec||brightness;
    const stepper=spec&&spec.key==='temperature',usable=connected&&available(main);
    const service=powerIsOn(item)?'turn_off':'turn_on',power=!isSceneCommand(item)&&controllable(item)&&supports(item,service);
    const on=powerIsOn(item),body=y+82,rw=w-32,rh=48;
    if(range&&!card.thumb)card.thumb=panel(0,0,6,28,0xffffff,3);
    card.quickSpec=spec;
    hide(card.iconBase,true);hide(card.unit,true);hide(card.switchFace,true);
    pushPos(card.panel,x,y);pushSize(card.panel,w,h);pushColor(card.panel,pal.card);
    pushStyle(card.panel,'S',{radius:20,borderWidth:0});
    hide(card.powerHalo,false);pushPos(card.powerHalo,x+16,y+18);pushSize(card.powerHalo,40,40);
    pushStyle(card.powerHalo,'S',{radius:20});pushColor(card.powerHalo,pal.blueSoft,!quickDrag&&!lampWheelDrag&&paint['c'+card.powerHalo]!==undefined);
    hide(card.icon,false);pushPos(card.icon,x+24,y+26);pushSize(card.icon,24,28);
    pushStyle(card.icon,'I',{fontSize:24,icon:1,center:1});pushText(card.icon,commonCardProfile(item)?.readonly?'\uf071':dashboardIcon(item));pushColor(card.icon,pal.blue);if(typeof ui.front==='function')ui.front(card.icon);
    card.passiveRegion={x:x+16,y:y+18,w:40,h:40};
    hide(card.name,false);pushPos(card.name,x+66,y+17);pushSize(card.name,w-(power?126:82),26);
    pushStyle(card.name,'L',{fontSize:18,ellipsis:1});pushText(card.name,fit(item.name,w-(power?126:82),18));pushColor(card.name,pal.ink);
    hide(card.state,false);pushPos(card.state,x+66,y+44);pushSize(card.state,w-(power?126:82),24);
    pushStyle(card.state,'L',{fontSize:14,ellipsis:1,center:0});pushColor(card.state,pal.muted);
    const status=range?spec?formatControl(spec,controlValue(main,spec)):displayBrightness(item)+'%':model.subtitle;
    const intent=controlTargets[item.entity_id],feedback=controlFeedback[item.entity_id];
    const progress=intent?(intent.slow?(intent.sent?'等待设备回报':'等待前序操作'):intent.sent?(pending&&pending.kind==='verify'&&pending.entityId===item.entity_id?'正在核对':'正在提交'):'等待执行'):feedback&&feedback.text;
    const powerText=on?tr('已开启'):tr('已关闭');
    const stateText=!connected?'缓存 · 待同步':!available(item)?(item.state==='unknown'?tr('状态未知'):tr('设备离线')):status+(power&&status.indexOf(powerText)<0?' · '+powerText:'');
    pushText(card.state,fit(progress||stateText,w-(power?126:82),14));
    pushColor(card.state,feedback&&!intent?(feedback.kind==='error'?pal.warn:pal.good):pal.muted);
    hide(card.powerGlyph,!power);pushPos(card.powerGlyph,x+w-46,y+26);pushSize(card.powerGlyph,24,28);
    pushStyle(card.powerGlyph,'I',{fontSize:24,icon:1,center:1});pushText(card.powerGlyph,'\uf011');pushColor(card.powerGlyph,on?(dark?0xc7ffdf:0x13744d):pal.muted);
    if(power){if(!card.switchFace)card.switchFace=panel(0,0,36,36,pal.track,18);hide(card.switchFace,false);
      pushPos(card.switchFace,x+w-52,y+20);pushSize(card.switchFace,36,36);pushStyle(card.switchFace,'S',{radius:18});pushColor(card.switchFace,on?(dark?0x234936:0xdcf7e8):pal.track,!quickDrag&&!lampWheelDrag&&paint['c'+card.switchFace]!==undefined);
      if(typeof ui.front==='function')ui.front(card.powerGlyph);}
    Object.assign(card.iconButton,{x:x+w-60,y:y+14,w:48,h:48});card.iconButton._cardVisible=power;
    card.iconButton.style.enabled=usable?1:0;hide(card.iconButton,!power);
    Object.assign(card.button,{x,y:y+h-42,w,h:38});card.button._cardVisible=true;hide(card.button,false);
    hide(card.moreLabel,false);pushPos(card.moreLabel,x+16,y+h-35);pushSize(card.moreLabel,w-32,26);
    pushStyle(card.moreLabel,'L',{fontSize:14,center:0});pushText(card.moreLabel,tr('更多功能  >'));pushColor(card.moreLabel,pal.muted);
    if(typeof ui.front==='function')ui.front(card.moreLabel);
    hide(card.value,false);pushPos(card.value,x+16,body);pushSize(card.value,rw,44);
    pushStyle(card.value,'L',{fontSize:28,center:0,ellipsis:1});pushColor(card.value,model.alert?pal.warn:pal.ink);
    hide(card.track,!range);hide(card.fill,!range);hide(card.thumb,!range);
    if(isSceneCommand(item)){
      const target=controlTargets[item.entity_id],allowed=supports(main,'turn_on');
      pushText(card.state,item.stale?'等待重新同步':friendlyState(item));
      if(allowed){
        if(!card.minusGlyph)card.minusGlyph=label('',0,0,rw,28,18,pal.blue);
        hide(card.track,false);pushPos(card.track,x+16,body);pushSize(card.track,rw,40);pushColor(card.track,usable&&!target?pal.blueSoft:pal.track);pushStyle(card.track,'S',{radius:14,borderWidth:0});
        hide(card.minusGlyph,false);pushPos(card.minusGlyph,x+16,body+7);pushSize(card.minusGlyph,rw,28);pushStyle(card.minusGlyph,'L',{fontSize:18,center:1});
        pushColor(card.minusGlyph,usable&&!target?pal.blue:pal.muted);pushText(card.minusGlyph,target?'正在提交…':item.domain==='scene'?'执行场景':'运行脚本');
        if(typeof ui.front==='function')ui.front(card.minusGlyph);
        Object.assign(card.minus,{x:x+16,y:body,w:rw,h:40});card.minus.style.fontSize=18;
        card.minus.style.enabled=usable&&!target?1:0;buttonText(card.minus,'');hide(card.minus,false);
        pushPos(card.value,x+16,body+44);pushSize(card.value,rw,20);pushStyle(card.value,'L',{fontSize:12,center:0});pushColor(card.value,pal.muted);
        pushText(card.value,fit(sceneTimestamp(item),rw,12));
      }else{pushStyle(card.value,'L',{fontSize:20});pushText(card.value,'暂不可执行');}
      return;
    }
    if(commonCardProfile(item)?.layout==='camera'){
      const previewH=WATERFALL?Math.max(48,h-136):48;
      pushText(card.state,fit(available(item)?friendlyState(item):tr('设备离线'),w-82,14));
      hide(card.track,false);pushPos(card.track,x+16,body);pushSize(card.track,rw,previewH);pushColor(card.track,pal.page);
      pushStyle(card.track,'S',{radius:12,borderWidth:0});pushStyle(card.track,'G',{gradientHorizontal:0,gradientVertical:0});
      if(!card.cameraGlyph)card.cameraGlyph=label('',0,0,32,36,32,pal.muted,3);
      detailIcon(card.cameraGlyph,'\ue002',x+24,body+(previewH-48)/2,32,48,3,pal.muted);
      pushPos(card.value,x+66,body+(previewH-24)/2);pushSize(card.value,rw-58,24);pushStyle(card.value,'L',{fontSize:13,center:0,icon:0,ellipsis:1});pushColor(card.value,pal.muted);pushText(card.value,tr('暂无预览画面'));
      if(typeof ui.front==='function'){ui.front(card.track);ui.front(card.cameraGlyph);ui.front(card.value);}return;
    }
    if(!range){
      const text=model.value||friendlyState(item);
      const widthAt=font=>typeof ui.measureText==='function'?measureTextCached(text,font).width:Array.from(text).reduce((width,char)=>width+(char.charCodeAt(0)>127?font:font*.66),0);
      const font=widthAt(28)>rw?widthAt(24)>rw?20:24:28;
      pushStyle(card.value,'L',{fontSize:font,center:0,ellipsis:1});pushText(card.value,widthAt(font)<=rw?text:fit(text,rw,font));return;
    }
    if(stepper){
      const bw=40,center=rw-2*bw-12;pushPos(card.track,x+16+bw+6,body);pushSize(card.track,center,44);
      pushColor(card.track,pal.page);pushStyle(card.track,'S',{radius:14,borderWidth:1,borderColor:pal.line});
      pushPos(card.fill,x+16,body);pushSize(card.fill,bw,44,!quickDrag&&paint['s'+card.fill]!==undefined);pushColor(card.fill,pal.track);pushStyle(card.fill,'S',{radius:14});
      pushPos(card.thumb,x+w-16-bw,body,!quickDrag&&!lampWheelDrag&&paint['p'+card.thumb]!==undefined);pushSize(card.thumb,bw,44);pushColor(card.thumb,pal.track);pushStyle(card.thumb,'S',{radius:14});
      pushPos(card.value,x+16+bw+6,body+5);pushSize(card.value,center,34);pushStyle(card.value,'L',{fontSize:26,center:1});pushText(card.value,numberText(controlValue(main,spec)));
      buttonText(card.minus,'-');buttonText(card.plus,'+');card.minus.style.fontSize=card.plus.style.fontSize=26;
      Object.assign(card.minus,{x:x+16,y:body,w:bw,h:44});Object.assign(card.plus,{x:x+w-16-bw,y:body,w:bw,h:44});
      card.minus.style.enabled=card.plus.style.enabled=usable?1:0;hide(card.minus,!usable);hide(card.plus,!usable);return;
    }
    hide(card.value,true);
    const value=spec?controlValue(main,spec):displayBrightness(item),percent=spec?(value-spec.min)*100/(spec.max-spec.min):value;
    const pct=Math.max(0,Math.min(100,Number.isFinite(percent)?percent:0));
    const rect=designSlider(card,card.track,card.state,main,spec,x+16,body,rw,rh,pct,light?0xffa64e:pal.blue);
    Object.assign(rect,{valueOnly:true,labelPrefix:'',labelColor:pal.muted,labelSuffix:power?' · '+(on?tr('已开启'):tr('已关闭')):''});
    card.sliderRects=[rect];if(spec){if(usable)card.controlRects=[rect];}else if(canAdjustBrightness(item))card.brightnessRect=rect;
  }

  function renderLightTile(card,item,entry) {
    const x=entry.x,y=entry.y,w=entry.w,h=entry.h;
    const active=available(item)&&item.state==='on',accent=dark?0xffaa60:0xee9748;
    const allowed=connected&&available(item)&&supports(item,item.state==='on'?'turn_off':'turn_on')&&
      !layoutEditing;
    const percent=displayBrightness(item),hasSlider=item.dimmable;
    if(hasSlider&&!card.thumb)card.thumb=panel(0,0,6,32,0xffffff,3);
    if(!card.switchFace){card.switchFace=panel(0,0,36,36,pal.track,18);if(typeof ui.front==='function')ui.front(card.icon);}
    hide(card.iconBase,true);hide(card.unit,true);hide(card.state,!layoutEditing);hide(card.icon,true);hide(card.powerGlyph,false);
    pushPos(card.panel,x,y);pushSize(card.panel,w,h);pushColor(card.panel,dark?0x202020:0xffffff);
    pushStyle(card.panel,'S',{radius:18,borderWidth:1,borderColor:dark?0x303030:0xe7e7e7});
    Object.assign(card.button,{x,y,w,h});card.button._cardVisible=true;
    pushPos(card.powerHalo,x+16,y+20);pushSize(card.powerHalo,48,48);hide(card.powerHalo,false);
    pushColor(card.powerHalo,active?(dark?0x4c3825:0xffead6):pal.track,!quickDrag&&!lampWheelDrag&&paint['c'+card.powerHalo]!==undefined);
    pushPos(card.powerGlyph,x+24,y+28);pushSize(card.powerGlyph,32,32);pushStyle(card.powerGlyph,'I',{fontSize:32,icon:3,center:1});
    pushText(card.powerGlyph,'\uf0eb');pushColor(card.powerGlyph,active?accent:pal.muted);
    card.passiveRegion={x:x+16,y:y+20,w:48,h:48};
    pushPos(card.name,x+78,y+20);pushSize(card.name,w-142,28);pushStyle(card.name,'L',{fontSize:22,ellipsis:1});
    pushText(card.name,fit(item.name,w-142,22));pushColor(card.name,available(item)?pal.ink:pal.muted);
    const command=controlTargets[item.entity_id];
    hide(card.state,!command);pushPos(card.state,x+78,y+77);pushSize(card.state,w-94,22);
    pushStyle(card.state,'L',{center:0,fontSize:12});pushColor(card.state,pal.muted);
    if(command)pushText(card.state,command.sent?tr('正在同步状态'):tr('待执行'));
    pushPos(card.value,x+78,y+49);pushSize(card.value,w-94,26);pushStyle(card.value,'L',{fontSize:18,ellipsis:1});
    hide(card.value,false);pushText(card.value,available(item)?hasSlider?percent+'%':friendlyState(item):
      item.state==='unavailable'?tr('设备离线'):tr('状态未知'));
    pushColor(card.value,available(item)?(dark?0xb7d9ed:0x667888):pal.muted);
    hide(card.switchFace,false);pushPos(card.switchFace,x+w-54,y+20);pushColor(card.switchFace,active?quickPalette.green:pal.track,!quickDrag&&!lampWheelDrag&&paint['c'+card.switchFace]!==undefined);
    hide(card.icon,false);pushPos(card.icon,x+w-48,y+26);pushSize(card.icon,24,24);
    pushStyle(card.icon,'I',{fontSize:24,icon:1,center:1});pushText(card.icon,'\uf011');pushColor(card.icon,active?0xffffff:pal.muted);
    Object.assign(card.iconButton,{x:x+w-60,y:y+14,w:48,h:48});
    card.iconButton._cardVisible=!layoutEditing&&controllable(item);card.iconButton.style.enabled=allowed?1:0;
    hide(card.iconButton,!card.iconButton._cardVisible);
    hide(card.track,!hasSlider);hide(card.fill,!hasSlider||percent===0);hide(card.thumb,!hasSlider||!available(item)||layoutEditing);
    const rx=x+16,ry=y+h-80,rw=w-32,rh=64,fillWidth=20+(rw-20)*percent/100;
    if(hasSlider) {
      pushPos(card.track,rx,ry);pushSize(card.track,rw,rh);pushColor(card.track,dark?0x4b3928:0xf5e9dd);
      pushStyle(card.track,'S',{radius:18,borderWidth:0});pushPos(card.fill,rx,ry);
      pushSize(card.fill,Math.max(1,Math.round(fillWidth)),rh,!quickDrag&&paint['s'+card.fill]!==undefined);pushColor(card.fill,available(item)?accent:pal.muted);
      pushStyle(card.fill,'S',{radius:18,borderWidth:0});
      pushPos(card.thumb,Math.round(rx+fillWidth-15-3),ry+16,!quickDrag&&!lampWheelDrag&&paint['p'+card.thumb]!==undefined);
      card.sliderRects=[{x:rx,y:ry,w:rw,h:rh}];
      if(canAdjustBrightness(item))card.brightnessRect={x:rx,y:ry,w:rw,h:rh,
        inputX:rx+5,inputW:rw-20,minFill:20,
        fill:card.fill,label:card.value,labelPrefix:'',labelColor:dark?0xb7d9ed:0x667888,thumb:card.thumb};
    }
    if(layoutEditing){pushPos(card.state,x+16,y+h-28);pushSize(card.state,w-32,22);pushText(card.state,'小卡 · 点按改为大卡');}
  }
  function renderCommonRangeTile(card,item,entry) {
    const {x,y,w,h}=entry,main=indexedEntity(item.entity_id)||item,spec=profileQuickControl(item);
    const value=controlValue(main,spec),percent=Number.isFinite(value)?Math.max(0,Math.min(100,(value-spec.min)*100/(spec.max-spec.min))):0;
    const powerService=item.state==='off'?'turn_on':'turn_off',hasPower=supports(item,powerService),usable=connected&&available(main)&&!layoutEditing;
    const on=!['off','closed','unavailable','unknown'].includes(item.state);
    hide(card.unit,true);hide(card.state,true);hide(card.switchFace,true);hide(card.iconBase,false);
    pushPos(card.panel,x,y);pushSize(card.panel,w,h);pushColor(card.panel,pal.card);pushStyle(card.panel,'S',{radius:18,borderWidth:1,borderColor:pal.line});
    Object.assign(card.button,{x,y,w,h});card.button._cardVisible=true;
    pushPos(card.iconBase,x+16,y+16);pushSize(card.iconBase,48,50);pushText(card.iconBase,thumbnailBase(item));pushColor(card.iconBase,pal.deviceBase);
    pushPos(card.icon,x+16,y+16);pushSize(card.icon,48,50);pushStyle(card.icon,'I',{fontSize:32,icon:2});pushText(card.icon,thumbnail(item));pushColor(card.icon,available(item)?pal.device:pal.muted);
    card.passiveRegion={x:x+16,y:y+16,w:48,h:50};
    pushPos(card.name,x+76,y+18);pushSize(card.name,w-(hasPower?138:92),26);pushStyle(card.name,'L',{fontSize:20,ellipsis:1});pushText(card.name,fit(item.name,w-(hasPower?138:92),20));pushColor(card.name,pal.ink);
    hide(card.value,false);pushPos(card.value,x+76,y+48);pushSize(card.value,w-92,25);pushStyle(card.value,'L',{fontSize:16,ellipsis:1});pushColor(card.value,pal.muted);pushText(card.value,spec.title+' '+formatControl(spec,value));
    hide(card.powerHalo,!hasPower);hide(card.powerGlyph,!hasPower);pushPos(card.powerHalo,x+w-54,y+18);pushSize(card.powerHalo,36,36);pushStyle(card.powerHalo,'S',{radius:18});pushColor(card.powerHalo,on?quickPalette.green:pal.track,!quickDrag&&!lampWheelDrag&&paint['c'+card.powerHalo]!==undefined);
    pushPos(card.powerGlyph,x+w-48,y+24);pushSize(card.powerGlyph,24,24);pushText(card.powerGlyph,'\uf011');pushStyle(card.powerGlyph,'I',{fontSize:24,icon:1,center:1});pushColor(card.powerGlyph,on?0xffffff:pal.muted);
    Object.assign(card.iconButton,{x:x+w-60,y:y+12,w:48,h:48});card.iconButton._cardVisible=hasPower;card.iconButton.style.enabled=usable?1:0;hide(card.iconButton,!hasPower);
    const rx=x+16,ry=y+h-80,rw=w-32,rh=64,width=20+(rw-20)*percent/100;
    if(!card.thumb)card.thumb=panel(0,0,6,32,0xffffff,3);
    hide(card.track,false);hide(card.fill,false);hide(card.thumb,!available(main));pushPos(card.track,rx,ry);pushSize(card.track,rw,rh);pushColor(card.track,pal.track);pushStyle(card.track,'S',{radius:18,borderWidth:0});
    pushPos(card.fill,rx,ry);pushSize(card.fill,width,rh,!quickDrag&&paint['s'+card.fill]!==undefined);pushColor(card.fill,pal.blue);pushStyle(card.fill,'S',{radius:18,borderWidth:0});pushPos(card.thumb,rx+width-18,ry+16,!quickDrag&&!lampWheelDrag&&paint['p'+card.thumb]!==undefined);pushSize(card.thumb,6,32);
    const rect={x:rx,y:ry,w:rw,h:rh,inputX:rx+5,inputW:rw-20,minFill:20,fill:card.fill,thumb:card.thumb,label:card.value,labelColor:pal.muted,spec,item:main};
    card.sliderRects=[rect];card.controlRects=usable?[rect]:[];
  }
  function rangeValue(rect,percent){const spec=rect.spec;return spec.min+(spec.max-spec.min)*percent/100;}
  function pointerBrightness(rect,x,y) {
    if(rect.vertical)return Math.max(0,Math.min(100,Math.round((rect.y+rect.h-y)*100/rect.h)));
    const start=rect.inputX===undefined?rect.x:rect.inputX,width=rect.inputW===undefined?rect.w:rect.inputW;
    return Math.max(0,Math.min(100,Math.round((x-start)*100/width)));
  }
  function handleSidebarTouch(state,nx,ny){
    if(!widgets||settingsOpen||browseOpen||optionPicker||disposed)return false;
    const x=nx*W,y=ny*H,rows=widgets.menu,first=rows[0].button.y,last=rows[rows.length-1].button.y;
    const selection=rows[0].panel;
    if(state==='down'){
      menuClickUntil=0;
      if(x<12||x>152||y<first||y>last+44)return false;
      menuDrag={generation,start:y,y:Math.round(Math.max(first,Math.min(last,y-22))),moved:false};
      return true;
    }
    if(!menuDrag)return false;
    if(menuDrag.generation!==generation||!Number.isFinite(x)||!Number.isFinite(y)||state==='cancel'){
      menuDrag=null;menuClickUntil=steadyNow()+300;render();return true;
    }
    if(Math.abs(y-menuDrag.start)>=5)menuDrag.moved=true;
    menuDrag.y=Math.round(Math.max(first,Math.min(last,y-22)));
    if(state==='move'&&menuDrag.moved)pushPos(selection,12,menuDrag.y,false);
    if(state==='up'){
      const drag=menuDrag;menuDrag=null;
      if(drag.moved){
        menuClickUntil=steadyNow()+300;
        if(x>=0&&x<SIDEBAR_W){
          const nearest=rows.reduce((a,b)=>Math.abs(b.button.y+22-y)<Math.abs(a.button.y+22-y)?b:a);
          pushPos(selection,12,nearest.button.y,false);selectSection(nearest.key);
        }else render();
      }
    }
    return true;
  }
  ui.onTouch((state,nx,ny) => {
    if(browseOpen)return;
    if(!Number.isFinite(nx)||!Number.isFinite(ny)){cancelInputs();render();return;}
    if(handleSidebarTouch(state,nx,ny))return;
    validateInputs();
    if(WATERFALL&&widgets&&widgets.boardViewport){
      boardScroll=Math.max(0,Math.round(ui.scrollInfo(widgets.boardViewport).y));
      if(state==='up'||state==='cancel'){boardTouchActive=false;boardLock(false);}
      else if(state==='down')boardTouchActive=!detailOpen&&!settingsOpen&&nx*W>=GRID_X&&ny*H>=HEADER_H&&ny*H<H-FOOTER_H;
    }
    if(handleLampWheel(state,nx,ny))return;
    if(state==='down')quickGestureOwned=false;
    if(WATERFALL&&state==='down'){
      boardPress=null;
      if(boardTouchActive){
        const px=nx*W,py=ny*H+boardScroll;
        const row=dashboardPages()[0].items.find(row=>px>=row.x&&px<=row.x+row.w&&py>=row.y&&py<=row.y+row.h);
        boardPress={entity:row?row.item.entity_id:'',scroll:boardScroll,generation};
      }
    }
    if(WATERFALL&&state==='cancel')quickGestureOwned=true;
    if(WATERFALL&&!detailOpen&&!settingsOpen&&state==='down'&&(nx*W<GRID_X||ny*H<HEADER_H||ny*H>=H-FOOTER_H)){quickDrag=null;return;}
    if(disposed||settingsOpen||optionPicker||!Number.isFinite(nx)||!Number.isFinite(ny)){quickDrag=null;return;}
    const px=nx*W,py=ny*H+(WATERFALL&&!detailOpen&&!settingsOpen?boardScroll:0);
    if(state==='down'){
      (widgets.cards||[]).forEach(card=>{const r=card.passiveRegion;card.passivePressed=!!r&&px>=r.x&&px<=r.x+r.w&&py>=r.y&&py<=r.y+r.h;});
      const groups=detailOpen?[widgets.detailDragGroup]:(widgets.quickGroups||[]).concat(widgets.cards||[]);
      let group=null,rect=null;
      groups.some(q=>{if(!q)return false;return (q.controlRects||[]).concat(q.brightnessRect?[q.brightnessRect]:[]).some(r=>{
        if(px<r.x||px>r.x+r.w||py<(r.hitY===undefined?r.y:r.hitY)||py>(r.hitY===undefined?r.y:r.hitY)+(r.hitH||r.h))return false;
        if(r.spec?connected&&available(r.item||q.item):canAdjustBrightness(q.item)){group=q;rect=r;return true;}return false;});});
      quickDrag=group?{rect,item:rect.item||group.item,group,generation,percent:rect.spec?Math.max(0,Math.min(100,(Number(controlValue(rect.item||group.item,rect.spec))-rect.spec.min)*100/(rect.spec.max-rect.spec.min))):group.item.brightness}:null;
      if(quickDrag&&WATERFALL&&!detailOpen&&!rect.vertical){
        quickDrag.pendingAxis=true;quickDrag.startX=nx*W;quickDrag.startY=ny*H;
      }
      quickGestureOwned=!!quickDrag||groups.some(q=>(q.sliderRects||[]).some(r=>px>=r.x&&px<=r.x+r.w&&py>=r.y&&py<=r.y+r.h));
    }
    if(layoutEditing){quickDrag=null;return;}
    if(quickDrag&&quickDrag.generation!==generation){quickDrag=null;boardLock(false);render();return;}
    if((state==='move'||state==='up')&&quickDrag&&quickDrag.pendingAxis){
      const dx=Math.abs(nx*W-quickDrag.startX),dy=Math.abs(ny*H-quickDrag.startY);
      if(dy>=8&&dy>dx*1.2){
        // Let LVGL continue the vertical gesture without submitting a value.
        quickDrag=null;quickGestureOwned=true;boardLock(false);return;
      }
      if(dx>=5&&dx>=dy)quickDrag.pendingAxis=false;
    }
    if(WATERFALL&&quickDrag&&(state==='down'||state==='move'))boardLock(true);
    if((state==='down'||state==='move')&&quickDrag&&!quickDrag.pendingAxis)
      previewQuickBrightness(quickDrag,pointerBrightness(quickDrag.rect,px,py));
    if(state==='up'&&quickDrag){const active=quickDrag;quickDrag=null;if(!detailOpen)selectedId=active.item.entity_id;
      const target=indexedEntity(active.item.entity_id);
      if(!connected||!available(target)){render();return;}
      if(active.rect.spec)requestFunction(target,active.rect.spec,rangeValue(active.rect,pointerBrightness(active.rect,px,py)));
      else if(canAdjustBrightness(target))control('turn_on',pointerBrightness(active.rect,px,py));
      render();}
    if(state==='cancel'){if(quickDrag){cardRenderKey='';detailRenderKey='';quickDrag.group.renderKey='';}quickDrag=null;render();}
  });
  globalThis.ESPHomeHA = {
    refresh, disconnect, openDevice:openDesktopDevice,
    settings: openSettings,
    snapshot,
    dispose: () => {
      flushUsage();
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
