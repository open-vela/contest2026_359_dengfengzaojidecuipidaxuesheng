'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname,
  '../overlay/apps/system/desktop/homeassistant/app.js'), 'utf8');
const englishCatalog=JSON.parse(fs.readFileSync(path.join(__dirname,'../i18n/en.json'),'utf8'));

assert.equal(source.includes('\\u2212'), false,
  'frontend text must not use the unsupported Unicode minus sign');

function boot(width = 1024, height = 600, options = {}) {
  let serviceUrl = options.url || 'http://192.168.1.20:8123';
  let configured = true, configurationRevision = 1;
  let handle = 1;
  let requestId = 1;
  let timer;
  let touch;
  let swipe;
  let back;
  let nextTimeout = 2;
  const deferred = [];
  const pending = [];
  const buttons = [];
  const controls = [];
  const requests = [];
  const geometry = new Map();
  const hidden = new Map();
  const textValues = new Map();
  const styles = new Map();
  const nativeWrites=[],desktopSnapshots=[];
  const saves = new Map();
  const prompts=[];
  const responses = {
    devices: options.devices || [],
    config: { location_name: 'Lab' },
    services: options.services || [{ domain: 'light', services: { turn_on: {}, turn_off: {} } }],
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
    status: () => ({ configured, busy: false, configurationRevision,
      deviceGroups: options.grouping !== false,sceneSync:options.sceneSync===true,
      url: serviceUrl, canControl: options.canControl !== false, canConfigure: true }),
    get(resource) {
      requests.push(resource);
      const id = requestId++;
      const body=resource==='scenes'?{items:(options.sceneStates||[]).slice(0,128),total:(options.sceneStates||[]).length}:responses[resource];
      pending.push({ requestId: id, busy: false, done: true,
        cached: resource === 'states', status: resource==='scenes'&&options.sceneStatus||resource==='states'&&options.stateStatus||resource==='devices'&&options.deviceStatus||200, error: 0,
        body: JSON.stringify(body) });
      return id;
    },
    getState(entity) {
      if(!options.acceptControls)return -22;
      const id=requestId++,state=responses.states.find(row=>row.entity_id===entity);
      pending.push({requestId:id,busy:false,done:true,status:state?200:404,error:0,body:JSON.stringify(state||{})});return id;
    },
    control(entity, on, brightness) {
      controls.push({ entity, on, brightness });
      if(options.busyAdmissions){options.busyAdmissions--;return -16;}
      if(options.acceptControls){
        const state=responses.states.find(row=>row.entity_id===entity);state.state=on?'on':'off';
        if(brightness>=0)state.attributes.brightness=Math.round(brightness*255/100);
        const id=requestId++;pending.push({requestId:id,busy:false,done:true,status:options.controlStatus||200,error:0,body:'[]'});return id;
      }
      return -22;
    },
    poll: () => options.holdRequests||options.sharedBusy ? { busy: true, done: false } :
      pending.shift() || { busy: false, done: false },
    close: () => { pending.length = 0; }
  };
  if(options.nativeActions)service.callService=(entity,action,data)=>{
    const body=JSON.parse(data);controls.push({entity,action,body});
    if(options.busyAdmissions){options.busyAdmissions--;return -16;}
    if(!options.acceptControls)return -22;
    const state=responses.states.find(row=>row.entity_id===entity)||(options.sceneStates||[]).find(row=>row.entity_id===entity);
    if((action==='turn_on'||action==='turn_off')&&!/^(scene|script)\./.test(entity))state.state=action==='turn_off'?'off':entity.startsWith('climate.')?'cool':entity.startsWith('water_heater.')?'electric':'on';
    if(body.brightness_pct!==undefined)state.attributes.brightness=Math.round(body.brightness_pct*255/100);
    if(body.color_temp_kelvin!==undefined)state.attributes.color_temp_kelvin=body.color_temp_kelvin;
    if(body.rgb_color!==undefined){state.attributes.rgb_color=body.rgb_color.slice();state.attributes.color_mode='rgb';}
    if(body.value!==undefined||body.option!==undefined)state.state=String(body.value===undefined?body.option:body.value);
    if(body.position!==undefined)state.attributes.current_position=body.position;
    if(body.tilt_position!==undefined)state.attributes.current_tilt_position=body.tilt_position;
    if(action==='open_cover'){state.state='open';state.attributes.current_position=100;}
    if(action==='close_cover'){state.state='closed';state.attributes.current_position=0;}
    if(action==='stop_cover'&&['opening','closing'].includes(state.state))state.state='open';
    if(action==='open_cover_tilt')state.attributes.current_tilt_position=100;
    if(action==='close_cover_tilt')state.attributes.current_tilt_position=0;
    if(action==='media_play')state.state='playing';if(action==='media_pause')state.state='paused';if(action==='media_stop')state.state='idle';
    if(body.hvac_mode!==undefined)state.state=body.hvac_mode;
    for(const key of ['percentage','preset_mode','oscillating','temperature','humidity','volume_level','is_volume_muted','effect','fan_mode','swing_mode','source','fan_speed','operation_mode','direction','swing_horizontal_mode','target_temp_low','target_temp_high','away_mode'])
      if(body[key]!==undefined)state.attributes[key]=body[key];
    const id=requestId++;pending.push({requestId:id,busy:false,done:true,status:options.controlStatus||200,error:0,body:'[]'});return id;
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
    primary: options.dark ? 0xffffff : 0x252d46,
    nowMs: options.nowMs,
    getSize: () => ({ width, height }),
    isVisible: () => options.visible !== false,
    background: () => {},
    button(text, x, y, w, h, callback) {
      const id = create(x, y, w, h);
      buttons.push({ id, callback });
      return id;
    },
    colorWheel:(x,y,d)=>create(x,y,d,d),
    panel: (x, y, w, h) => create(x, y, w, h),
    rect: (x, y, w, h) => create(x, y, w, h),
    arc: (x, y, w, h) => create(x, y, w, h),
    arcSet: id => requireHandle(id, 'arcSet'),
    line: (points, x, y, w, h) => create(x, y, w, h),
    lineSet: id => requireHandle(id, 'lineSet'),
    text: (text, x, y, w = 1, h = 1) => {const id=create(x,y,w,h);textValues.set(id,text);return id;},
    setColor: (id,value,smooth=false) => {requireHandle(id, 'setColor');nativeWrites.push({method:'color',id,value,smooth});},
    setHidden: (id,value) => {requireHandle(id, 'setHidden');hidden.set(id,!!value);nativeWrites.push({method:'hidden',id,value:!!value});},
    setStyle: (id,style) => {requireHandle(id,'setStyle');if(style.center!==undefined)assert([0,1].includes(style.center),'native center is boolean');if(style.fontSize!==undefined)assert(style.fontSize>=0&&style.fontSize<=48,'native style fontSize contract');styles.set(id,{...styles.get(id),...style});nativeWrites.push({method:'style',id,style});},
    setText(id, value) {
      requireHandle(id, 'setText');
      textValues.set(id, value);
      nativeWrites.push({method:'text',id,value});
    },
    setOpacity: id => requireHandle(id, 'setOpacity'),
    onSwipe: handler => { swipe = handler; },
    onBack: handler => { back = handler; },
    onTouch: handler => { touch = handler; },
    setPos(id, x, y, smooth=false) {
      requireHandle(id, 'setPos');
      Object.assign(geometry.get(id), { x, y });
      nativeWrites.push({method:'position',id,x,y,smooth});
    },
    setSize(id, w, h, smooth=false) {
      requireHandle(id, 'setSize');
      Object.assign(geometry.get(id), { w, h });
      nativeWrites.push({method:'size',id,w,h,smooth});
    }
  };
  const viewports=new Map(),parents=new Map();
  if(options.waterfall){
    ui.viewport=(x,y,w,h,callback)=>{const id=create(x,y,w,h);viewports.set(id,{x,y,w,h,height:h,yScroll:0,callback,locked:false});return id;};
    ui.attach=(id,parent)=>{requireHandle(id,'attach');assert(parent===0||viewports.has(parent));parents.set(id,parent);};
    ui.scrollInfo=id=>{const v=viewports.get(id);assert(v);return {y:v.yScroll,max:Math.max(0,v.height-v.h),moving:false};};
    ui.scrollTo=(id,y)=>{const v=viewports.get(id);assert(v);v.yScroll=Math.max(0,Math.min(y,v.height-v.h));v.callback(v.yScroll);};
    ui.scrollExtent=(id,height)=>{const v=viewports.get(id);assert(v);v.height=height;};
    ui.scrollLock=(id,locked)=>{const v=viewports.get(id);assert(v);v.locked=locked;};
  }
  if(options.frontOrder)ui.front=id=>{requireHandle(id,'front');nativeWrites.push({method:'front',id});};
  if(options.measureMetrics)ui.measureText=options.measureMetrics;
  const context = vm.createContext({
    system: { homeAssistantService: service, homeAssistant: {},
      language:options.language||'zh-CN',
      translate:text=>options.language==='en'?(englishCatalog[text]||text):text,
      desktop:{devices:json=>{desktopSnapshots.push(JSON.parse(json));return 0;}},
      yield: () => ({ then: resolve => { deferred.push(resolve); } }),
      storage: { get: key => key==='eh_favs'?JSON.stringify(options.favorites||[]):key==='eh_scope'?(options.savedScope||''):key==='eh_card_sizes'?(options.savedLayout||''):key==='eh_card_sizes_2'?(options.savedLayout2||''):key==='eh_card_usage'?(options.savedUsage||''):key==='eh_desktop_pins'?(options.savedPins||''):'', set: (key,value) => saves.set(key,value) } },
    ui, prompt: { input: (options,callback) => prompts.push({options,callback}) }, console,
    setInterval(callback) { timer = callback; return 1; },
    clearInterval: () => {},
    setTimeout(callback) { deferred.push(callback); return nextTimeout++; },
    clearTimeout: () => {}
  });
  vm.runInContext(options.source||source, context, { filename: 'homeassistant/app.js' });
  // Creation deliberately shares several native event-loop turns.
  for(let i=0;i<30&&deferred.length;i++)deferred.shift()();
  return { context, buttons, controls, requests, geometry, hidden, textValues, styles, saves,nativeWrites,desktopSnapshots,viewports,parents,prompts,
    remoteConfigure(endpoint) {
      serviceUrl = endpoint;
      configured = !!endpoint;
      configurationRevision++;
    }, tick: () => {
    timer();
    const callback = deferred.shift();
    if (callback) callback();
  },
    touch: (state, nx, ny) => touch(state, nx, ny),
    swipe: direction => swipe(direction), back: () => back() };
}

test('system back dismisses the option picker, detail and section one at a time',()=>{
  const r=lampFixture({effect_list:Array.from({length:9},(_,i)=>'effect-'+i),effect:'effect-0'});
  const state=()=>r.context.ESPHomeHA.snapshot();
  lampClickLabel(r,'灯光效果');assert(state().optionPicker);
  assert.equal(r.back(),true);assert.equal(state().optionPicker,null);assert(state().detailOpen);
  assert.equal(r.back(),true);assert.equal(state().detailOpen,false);
  assert.equal(state().nav,'overview');
  assert.equal(r.back(),false);assert.equal(r.controls.length,0);
  r.context.ESPHomeHA.settings();assert(state().settingsOpen);
  assert.equal(r.back(),true);assert.equal(state().settingsOpen,false);
  assert.equal(r.back(),false);
});

test('system back closes discovery before leaving the waterfall board',()=>{
  const r=boot(1024,600,{waterfall:true});r.context.ESPHomeHA.refresh();for(let i=0;i<60;i++)r.tick();
  discoveryHeader(r,1024,136);assert(r.context.ESPHomeHA.snapshot().discovery.open);
  assert.equal(r.back(),true);assert(!r.context.ESPHomeHA.snapshot().discovery.open);
  assert.equal(r.back(),false);assert.equal(r.controls.length,0);
});

test('covered frontend completes requests without painting and catches up after reveal', () => {
  const options={states:[{entity_id:'light.desk',state:'on',attributes:{friendly_name:'Desk',supported_color_modes:['brightness']}}]};
  const r=boot(1024,536,options);
  for(let i=0;i<30;i++)r.tick();
  options.visible=false;r.nativeWrites.length=0;
  options.states[0].attributes.friendly_name='Changed while covered';
  r.context.ESPHomeHA.refresh();for(let i=0;i<30;i++)r.tick();
  assert.equal(r.context.ESPHomeHA.snapshot().phase,'ready');
  assert.equal(r.context.ESPHomeHA.snapshot().entities.length,1);
  assert.equal(r.nativeWrites.length,0,'covered refresh must not update native widgets');
  options.visible=true;for(let i=0;i<10;i++)r.tick();
  assert(r.nativeWrites.length>0,'latest state paints after reveal');
});

test('covered frontend defers construction but continues its connection', () => {
  const options={visible:false};const r=boot(1024,536,options);
  const created=r.geometry.size;r.nativeWrites.length=0;
  for(let i=0;i<30;i++)r.tick();
  assert.equal(r.geometry.size,created);
  assert.equal(r.nativeWrites.length,0);
  assert(r.requests.includes('states'),'connection must continue before UI construction completes');
  options.visible=true;for(let i=0;i<30;i++)r.tick();
  assert.equal(r.context.ESPHomeHA.snapshot().uiReady,true);
  assert(r.geometry.size>created);
});

test('large reading snapshots avoid repeated full-list scans and follow committed states', () => {
  const states=Array.from({length:512},(_,i)=>({entity_id:'sensor.monitor_'+i,state:String(i),
    attributes:{friendly_name:'Monitor '+i,device_class:'temperature',unit_of_measurement:'C',
      device_type:'air-monitor'}}));
  const r=boot(1024,536,{states});
  r.context.ESPHomeHA.refresh();for(let i=0;i<30;i++)r.tick();
  vm.runInContext(`globalThis.fullListVisits=0;
    const originalFind=Array.prototype.find;
    Array.prototype.find=function(predicate,thisArg){
      const measured=this.length>=128&&this[0]&&this[0].entity_id;
      return originalFind.call(this,function(value,index,array){
        if(measured)fullListVisits++;return predicate.call(thisArg,value,index,array);
      });
    };`,r.context);
  let state=r.context.ESPHomeHA.snapshot();
  assert.equal(state.entities.length,512);
  assert(r.context.fullListVisits<512*10,'snapshot must not scan all entities for every sensor');
  states[511].state='27';
  r.context.ESPHomeHA.refresh();for(let i=0;i<30;i++)r.tick();
  state=r.context.ESPHomeHA.snapshot();
  const card=state.deviceCards.find(card=>card.entity_id==='sensor.monitor_511');
  assert(card.readings.some(row=>row.display==='27.0 C'&&row.source==='sensor.monitor_511'),JSON.stringify(card));
  states.splice(511,1);
  r.context.ESPHomeHA.refresh();for(let i=0;i<30;i++)r.tick();
  assert(!r.context.ESPHomeHA.snapshot().deviceCards.some(card=>card.entity_id==='sensor.monitor_511'));
  r.remoteConfigure('');r.tick();
  assert.equal(r.context.ESPHomeHA.snapshot().entities.length,0);
  assert.equal(r.controls.length,0);
});

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

test('a real-home sized state response exceeding 64 KiB still synchronizes', () => {
  const states=Array.from({length:100},(_,i)=>({entity_id:'sensor.large_'+i,state:String(i),
    attributes:{friendly_name:'Sensor '+i,unit_of_measurement:'C',extra:'x'.repeat(900)}}));
  assert.ok(JSON.stringify(states).length>65536);
  const r=boot(1024,600,{url:'http://124.223.30.246:8123',states});
  r.context.ESPHomeHA.refresh();for(let i=0;i<30;i++)r.tick();
  const result=r.context.ESPHomeHA.snapshot();
  assert.equal(result.connected,true);assert.equal(result.entities.length,100);
});

test('HA scheduling follows monotonic time across wall-clock changes', () => {
  let monotonic=1000;
  const runtime=boot(1024,536,{nowMs:()=>monotonic});
  runtime.context.ESPHomeHA.refresh();for(let i=0;i<20;i++)runtime.tick();
  const before=runtime.requests.length,deadline=runtime.context.ESPHomeHA.snapshot().nextRefresh;
  assert.equal(deadline,11000);
  vm.runInContext('Date.now=()=>9999999999999',runtime.context);
  for(let i=0;i<20;i++)runtime.tick();assert.equal(runtime.requests.length,before,'NTP forward jump must not expire HA requests');
  vm.runInContext('Date.now=()=>0',runtime.context);monotonic=deadline+1;
  for(let i=0;i<20;i++)runtime.tick();assert(runtime.requests.length>before,'wall-clock rollback must not stall refresh');
});

test('frontend keeps its fixed controls inside supported displays', () => {
  for (const [width, height] of [[1024, 536], [1024, 600]]) {
    const runtime = boot(width, height);
    assert.equal(runtime.buttons.length, 32);
    assert.ok(runtime.geometry.size <= 148, // Fixed search/filter icons and contextual empty-state labels.
      `${width}x${height}: ${runtime.geometry.size} native widgets exceeds the UI budget`);
    for (const box of runtime.geometry.values()) {
      assert.ok(box.x >= 0 && box.y >= 0 && box.w > 0 && box.h > 0);
      assert.ok(box.x + box.w <= width, `${width} clips ${JSON.stringify(box)}`);
      assert.ok(box.y + box.h <= height, `${height} clips ${JSON.stringify(box)}`);
    }
  }
});

test('More opens details while only the separate power icon toggles the entity', () => {
  for (const [width, height] of [[1024, 536], [1024, 600]]) {
    const runtime = boot(width, height);
    runtime.context.ESPHomeHA.refresh();
    for (let i = 0; i < 12; i++) runtime.tick();
    const card = runtime.buttons.find(entry => {
      const box = runtime.geometry.get(entry.id);
      return !runtime.hidden.get(entry.id) && box && box.x>=164 && box.w >= 120 && box.h === 38;
    });
    assert.ok(card, `${width}x${height}: a card sized button exists`);
    card.callback();
    assert.equal(runtime.context.ESPHomeHA.snapshot().view, 'detail');
    assert.equal(runtime.context.ESPHomeHA.snapshot().detailOpen, true);
    assert.equal(runtime.controls.length, 0, `${width}x${height}: card does not toggle`);
    for (let i = 0; i < 12; i++) runtime.tick();
    assert.equal(runtime.buttons.length, 64, `${width}x${height}: detail pool expands lazily`);
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
    return !runtime.hidden.get(entry.id) && box && box.x>=164 && box.w >= 120 && box.h === 38;
  });
  card.callback();
  for (let i = 0; i < 12; i++) runtime.tick();
  assertUnifiedDetail(runtime);
  const power=runtime.buttons.find(entry=>{const g=runtime.geometry.get(entry.id);return !runtime.hidden.get(entry.id)&&g.w===48&&g.h===48&&g.x>700&&g.y<150;});
  assert(power,'More exposes a power action');
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
    return !sensorRuntime.hidden.get(entry.id) && box && box.x>=164 && box.w >= 120 && box.h === 38;
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
    return box && box.x>=164 && box.w >= 120 && box.h === 38;
  });
  card.callback();
  for (let i = 0; i < 12; i++) runtime.tick();
  assertUnifiedDetail(runtime);
  const rail=runtime.context.ESPHomeHA.snapshot().parameterControls.find(r=>r.key==='brightness_pct');
  assert(rail);const nx=(rail.x+rail.w*(rail.vertical?.5:.6))/1024,ny=(rail.y+rail.h*(rail.vertical?.4:.5))/600;
  runtime.touch('down',nx,ny);runtime.touch('up',nx,ny);
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

test('remote configuration updates the open home address and restores controls', () => {
  const runtime = boot(1024, 536);
  runtime.context.ESPHomeHA.refresh();
  for (let i = 0; i < 20; i++) runtime.tick();
  const before = runtime.requests.length;
  runtime.remoteConfigure('http://ha.example.com:8123');
  for (let i = 0; i < 20; i++) runtime.tick();
  const state = runtime.context.ESPHomeHA.snapshot();
  assert.equal(state.url, 'http://ha.example.com:8123');
  assert.equal(state.nativeManaged, true);
  assert.equal(state.connected, true);
  assert.deepEqual(runtime.requests.slice(before), ['config', 'services', 'devices', 'states']);
  const power = runtime.buttons.find(b => {
    const box = runtime.geometry.get(b.id);
    return box.w === 48 && box.h === 48;
  });
  power.callback();
  assert.equal(runtime.controls.length, 1);
});

test('remote token replacement discards an in-flight request even at the same address', () => {
  const options = { holdRequests: true };
  const runtime = boot(1024, 536, options);
  runtime.context.ESPHomeHA.refresh();
  runtime.tick();
  runtime.remoteConfigure('http://192.168.1.20:8123');
  runtime.tick();
  options.holdRequests = false;
  for (let i = 0; i < 20; i++) runtime.tick();
  assert.equal(runtime.context.ESPHomeHA.snapshot().connected, true);
  assert.deepEqual(runtime.requests, ['config', 'config', 'services', 'devices', 'states']);
});

test('clearing remote configuration removes the old home and opens settings', () => {
  const runtime = boot();
  runtime.context.ESPHomeHA.refresh();
  for (let i = 0; i < 20; i++) runtime.tick();
  const before = runtime.requests.length;
  runtime.remoteConfigure('');
  for (let i = 0; i < 20; i++) runtime.tick();
  const state = runtime.context.ESPHomeHA.snapshot();
  assert.equal(state.nativeManaged, false);
  assert.equal(state.connected, false);
  assert.equal(state.settingsOpen, true);
  assert.equal(state.entities.length, 0);
  assert.equal(runtime.requests.length, before);
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

test('Mi quick cards keep the More and power targets separate in both themes', () => {
  for(const dark of [false,true]) {
    const runtime=boot(1024,536,{dark});runtime.context.ESPHomeHA.refresh();
    for(let i=0;i<20;i++)runtime.tick();
    const targets=runtime.buttons.map(button=>({button,box:runtime.geometry.get(button.id)}));
    const card=targets.find(t=>t.box.w>200&&t.box.h===38);
    const power=targets.find(t=>t.box.w===48&&t.box.h===48);
    assert(card&&power);
    assert(power.box.x>card.box.x+card.box.w/2,'the independent power switch sits at the upper right');
    assert(power.box.y<card.box.y);
    power.button.callback();
    assert.equal(runtime.controls.length,1);
    assert.deepEqual(runtime.controls[0],{entity:'light.desk',on:false,brightness:-1});
    assert(!runtime.context.ESPHomeHA.snapshot().detailOpen);
  }
});

test('offline, unknown and read-only quick cards never send control requests', () => {
  for(const options of [
    {canControl:false},
    {states:[{entity_id:'light.offline',state:'unavailable',attributes:{friendly_name:'Offline'}}]},
    {states:[{entity_id:'light.unknown',state:'unknown',attributes:{friendly_name:'Unknown'}}]}
  ]) {
    const runtime=boot(1024,536,options);runtime.context.ESPHomeHA.refresh();
    for(let i=0;i<20;i++)runtime.tick();
    const power=runtime.buttons.find(b=>{const box=runtime.geometry.get(b.id);return box.w===48&&box.h===48;});
    if(power)power.callback();assert.equal(runtime.controls.length,0);
    if(options.states)assert([...runtime.textValues.values()].includes(options.states[0].state==='unknown'?'状态未知':'设备离线'));
  }
});

test('quick-card registry covers the published category catalog', () => {
  const catalog=JSON.parse(fs.readFileSync(path.join(__dirname,'../../homeassistant/quick-card-catalog.json'),'utf8'));
  assert.deepEqual([...boot().context.ESPHomeHA.snapshot().registeredCardTypes].sort(),catalog.types.slice().sort());
  assert.equal(boot().context.ESPHomeHA.snapshot().registeredCardVariants,127);
});

test('every published property combination resolves to its registered reference layout', () => {
  const catalog=JSON.parse(fs.readFileSync(path.join(__dirname,'../../homeassistant/quick-card-catalog.json'),'utf8'));
  const fixtureContext=vm.createContext({});vm.runInContext(fs.readFileSync(path.join(__dirname,'mijia_card_fixture.js'),'utf8'),fixtureContext);
  catalog.variants.forEach((spec,i)=>{
    const runtime=boot(1024,536,{states:[fixtureContext.__miExamples[i]]});runtime.context.ESPHomeHA.refresh();for(let n=0;n<20;n++)runtime.tick();
    const match=runtime.context.ESPHomeHA.snapshot().entities[0].quickReference;
    assert.equal(match.layout,spec.layout,spec.type+' variant '+spec.variant);
    assert(match.variant>=1,spec.type+' resolves through the property registry');
    const card=runtime.buttons.find(b=>{const box=runtime.geometry.get(b.id);return box.w>200&&box.h===38;});
    card.callback();for(let n=0;n<25;n++)runtime.tick();
    for(const [id,box] of runtime.geometry)if(!runtime.hidden.get(id)) {
      assert(box.x>=0&&box.y>=0&&box.x+box.w<=1024&&box.y+box.h<=536,spec.type+' clips '+JSON.stringify(box));
    }
  });
});

test('quick-card variants depend on real properties and retain read-only domains', () => {
  const examples=[
    ['light','on',{brightness:160,color_temp_kelvin:4000},'light-color-temperature','63%'],
    ['light','off',{},'light-status',null],
    ['fan','on',{percentage:40,oscillating:true},'fan-speed','风速 40%'],
    ['fan','on',{device_type:'air-purifier',pm25:12},'air-quality','PM2.5 12.0'],
    ['fan','on',{percentage:NaN},'fan-status',null],
    ['cover','open',{current_position:72},'curtain-position','开合 72%'],
    ['media_player','playing',{volume_level:.35,media_title:'Evening radio'},'speaker-volume','音量 35%'],
    ['vacuum','cleaning',{battery_level:68},'vacuum-battery','68%'],
    ['humidifier','on',{current_humidity:48},'humidifier-humidity','48%'],
    ['climate','heat',{current_temperature:20.5,temperature:22},'temperature-reading','20.5 °C'],
    ['sensor','24.5',{device_class:'temperature',unit_of_measurement:'C'},'sensor-reading','24.5 °C'],
    ['camera','idle',{miot_type:'urn:miot-spec-v2:device:camera:0000A01C:example:1'},'profile-status','空闲'],
    ['switch','unavailable',{device_type:'outlet'},'switch-status',null],
    ['sensor','idle',{device_type:'not-a-registered-category'},'sensor-reading','空闲']
  ];
  for(const [domain,state,attributes,style,value] of examples) {
    const runtime=boot(1024,536,{states:[{entity_id:domain+'.example',state,attributes:{friendly_name:'Example',...attributes}}]});
    runtime.context.ESPHomeHA.refresh();for(let i=0;i<20;i++)runtime.tick();
    assert.equal(runtime.context.ESPHomeHA.snapshot().entities[0].cardStyle,style,domain);
    if(value){const display=style==='air-quality'?'12.0':/^(?:sensor-reading|temperature-reading|humidifier-humidity|vacuum-battery)$/.test(style)?value.replace(/^(\d+(?:\.\d+)?).*$/, '$1'):value;assert([...runtime.textValues.values()].some(text=>String(text).includes(display)),value);}
    if(!['light','switch','input_boolean','fan'].includes(domain)) {
      const card=runtime.buttons.find(b=>{const box=runtime.geometry.get(b.id);return box.w>200&&box.h===38;});
      card.callback();for(let i=0;i<20;i++)runtime.tick();
      runtime.buttons.forEach(b=>{const box=runtime.geometry.get(b.id);if(box.w===116&&box.h===116)b.callback();});
      assert.equal(runtime.controls.length,0,domain+' retains the existing read-only service scope');
    }
  }
  const runtime=boot(1024,536);runtime.context.ESPHomeHA.refresh();for(let i=0;i<20;i++)runtime.tick();
  assert([...runtime.geometry.values()].some(box=>box.w>200&&box.h===48),'small lamp brightness track follows the thick reference style');
});

test('unified small readings retain their values, units and readable text boxes',()=>{
  for(const height of [536,600]){
    const r=boot(1024,height,{states:[
      {entity_id:'fan.purifier',state:'on',attributes:{friendly_name:'Air purifier',device_type:'air-purifier',pm25:12}},
      {entity_id:'sensor.temperature',state:'24.5',attributes:{friendly_name:'Temperature',device_class:'temperature',unit_of_measurement:'°C'}},
      {entity_id:'sensor.humidity',state:'48.0',attributes:{friendly_name:'Humidity',device_class:'humidity',unit_of_measurement:'%'}}]});
    r.context.ESPHomeHA.refresh();for(let i=0;i<35;i++)r.tick();
    for(const part of ['12.0','24.5','48.0','°C','%'])assert([...r.textValues].some(([id,text])=>!r.hidden.get(id)&&String(text).includes(part)),part);
    for(const [id,text] of r.textValues)if(!r.hidden.get(id)&&/24.5|48.0/.test(String(text)))assert(r.geometry.get(id).h>=32);
    assert.equal(r.controls.length,0);
  }
});

function assertUnifiedDetail(runtime) {
  assert(runtime.context.ESPHomeHA.snapshot().detailOpen);
  assert(runtime.context.ESPHomeHA.snapshot().detailAdvanced,'small card opens the full detailed card in one step');
}

test('small lamp directly opens the detailed card and commits brightness through the original service', () => {
  const runtime=boot(1024,536);runtime.context.ESPHomeHA.refresh();for(let i=0;i<20;i++)runtime.tick();
  const card=runtime.buttons.find(b=>{const box=runtime.geometry.get(b.id);return box.w>200&&box.h===38;});
  card.callback();for(let i=0;i<20;i++)runtime.tick();
  assert.equal(runtime.context.ESPHomeHA.snapshot().entities[0].quickReference.layout,18);
  assert(runtime.context.ESPHomeHA.snapshot().detailAdvanced);
  assert(![...runtime.textValues].some(([id,text])=>!runtime.hidden.get(id)&&text==='更多操作'),'no intermediate card footer');
  const rect=runtime.context.ESPHomeHA.snapshot().parameterControls.find(r=>r.entity==='light.desk'&&r.key==='brightness_pct');
  assert(rect);const nx=(rect.x+rect.w*.75)/1024,ny=(rect.y+rect.h/2)/536;
  runtime.touch('down',nx,ny);assert.equal(runtime.controls.length,0);
  runtime.touch('up',nx,ny);assert.deepEqual(runtime.controls[0],{entity:'light.desk',on:true,brightness:75});
});

function layoutButton(runtime) {
  return runtime.buttons.find(b=>!runtime.hidden.get(b.id)&&['编辑卡片','完成编辑'].includes(runtime.textValues.get(b.id)));
}
function deviceBody(runtime,entityId) {
  let snapshot=runtime.context.ESPHomeHA.snapshot();const page=snapshot.layoutPages.findIndex(p=>p.some(r=>r.entity_id===entityId));
  while(snapshot.roomPage!==page){runtime.swipe(snapshot.roomPage<page?'left':'right');for(let i=0;i<20;i++)runtime.tick();snapshot=runtime.context.ESPHomeHA.snapshot();}
  const row=snapshot.layoutPages[page].find(r=>r.entity_id===entityId);
  const button=runtime.buttons.find(b=>{const box=runtime.geometry.get(b.id);return !runtime.hidden.get(b.id)&&(snapshot.layoutEditing?
    box.x===row.x+16&&box.y===row.y+row.h-50&&box.w===row.w-32&&box.h===36:
    box.x===row.x&&box.y===row.y+row.h-42&&box.w===row.w&&box.h===38);});
  assert(button,entityId+' is editable');return button;
}

function largeBrightnessRail(runtime,entityId) {
  const state=runtime.context.ESPHomeHA.snapshot();
  const row=state.layoutPages[state.roomPage].find(r=>r.entity_id===entityId);
  assert(row&&row.size==='small');
  const label=[...runtime.textValues].find(([id,value])=>!runtime.hidden.get(id)&&
    String(value).includes('%')&&runtime.geometry.get(id).y>=row.y&&runtime.geometry.get(id).y<row.y+row.h);
  assert(label,'brightness label');const box=runtime.geometry.get(label[0]);
  const rail=state.brightnessControls.find(r=>r.entity===entityId);
  assert(rail,'brightness track');return {rect:rail,labelId:label[0]};
}

test('mixed-board brightness drag owns the gesture and previews before one release commit', () => {
  const savedLayout=JSON.stringify({server:'http://192.168.1.20:8123',sizes:{'light.desk':'large'}});
  const r=boot(1024,536,{savedLayout});r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
  const rail=largeBrightnessRail(r,'light.desk'),nx=v=>(rail.rect.x+rail.rect.w*v)/1024,ny=(rail.rect.y+rail.rect.h/2)/536;
  r.touch('down',nx(.5),ny);r.touch('move',nx(.3),ny);r.swipe('left');for(let i=0;i<20;i++)r.tick();
  assert.equal(r.context.ESPHomeHA.snapshot().roomPage,0,'a brightness drag must not turn the board page');
  assert.equal(r.textValues.get(rail.labelId),'30% · 已开启');assert.equal(r.controls.length,0);
  r.touch('up',nx(.3),ny);r.swipe('left');
  assert.deepEqual(r.controls,[{entity:'light.desk',on:true,brightness:30}]);
  assert.equal(r.context.ESPHomeHA.snapshot().roomPage,0,'release gesture belongs to the same slider');
  r.touch('down',.98,.85);r.swipe('left');assert.equal(r.context.ESPHomeHA.snapshot().roomPage,1,'ordinary board swiping still works');
});

test('switching from More to overview does not leave mixed brightness input blocked', () => {
  const savedLayout=JSON.stringify({server:'http://192.168.1.20:8123',sizes:{'light.desk':'large'}});
  const r=boot(1024,536,{savedLayout});r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
  const state=r.context.ESPHomeHA.snapshot(),row=state.layoutPages[0].find(v=>v.entity_id==='light.desk');
  const more=r.buttons.find(b=>{const v=r.geometry.get(b.id);return !r.hidden.get(b.id)&&v.x===row.x&&v.w===row.w&&v.h<80;});
  assert(more);more.callback();for(let i=0;i<30;i++)r.tick();assert(r.context.ESPHomeHA.snapshot().detailAdvanced);
  const overview=r.buttons.find(b=>{const v=r.geometry.get(b.id);return !r.hidden.get(b.id)&&v.x===12&&v.y===104;});
  assert(overview);overview.callback();for(let i=0;i<30;i++)r.tick();
  const rail=largeBrightnessRail(r,'light.desk'),nx=(rail.rect.x+rail.rect.w*.6)/1024,ny=(rail.rect.y+rail.rect.h/2)/536;
  r.touch('down',nx,ny);r.touch('up',nx,ny);
  assert.deepEqual(r.controls,[{entity:'light.desk',on:true,brightness:60}]);
});

test('cancelled, editing and invalidated brightness gestures never submit', () => {
  const savedLayout=JSON.stringify({server:'http://192.168.1.20:8123',sizes:{'light.desk':'large'}});
  for(const mode of ['cancel','remote']) {
    const r=boot(1024,536,{savedLayout});r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
    const rail=largeBrightnessRail(r,'light.desk'),nx=(rail.rect.x+rail.rect.w*.75)/1024,ny=(rail.rect.y+rail.rect.h/2)/536;
    if(mode==='editing'){layoutButton(r).callback();for(let i=0;i<20;i++)r.tick();}
    r.touch('down',nx,ny);r.touch('move',nx,ny);
    if(mode==='cancel')r.touch('cancel',nx,ny);
    if(mode==='remote'){r.remoteConfigure('http://192.168.1.21:8123');for(let i=0;i<20;i++)r.tick();}
    r.touch('up',nx,ny);assert.equal(r.controls.length,0,mode+' must not submit');
  }
});

test('registry device IDs group functions while identical names do not merge devices', () => {
  const states=[
    {entity_id:'light.main',state:'on',attributes:{friendly_name:'灯',brightness:128,supported_color_modes:['brightness']}},
    {entity_id:'sensor.temp_a',state:'24.5',attributes:{friendly_name:'温度',device_class:'temperature',unit_of_measurement:'°C'}},
    {entity_id:'sensor.hum_a',state:'48',attributes:{friendly_name:'湿度',device_class:'humidity',unit_of_measurement:'%'}},
    {entity_id:'sensor.temp_b',state:'22',attributes:{friendly_name:'温度',device_class:'temperature',unit_of_measurement:'°C'}},
    {entity_id:'sensor.hum_b',state:'60',attributes:{friendly_name:'湿度',device_class:'humidity',unit_of_measurement:'%'}},
    {entity_id:'sensor.temp_unrelated',state:'10',attributes:{friendly_name:'温度',device_class:'temperature',unit_of_measurement:'°C'}}
  ];
  const devices=[['dev_a','设备 A','客厅',['light.main','sensor.temp_a','sensor.hum_a']],
    ['dev_b','设备 B','客厅',['sensor.temp_b','sensor.hum_b']]];
  const r=boot(1024,536,{states,devices,savedLayout:JSON.stringify({server:'http://192.168.1.20:8123',sizes:{'device.dev_a':'large'}})});
  r.context.ESPHomeHA.refresh();for(let i=0;i<50;i++)r.tick();const s=r.context.ESPHomeHA.snapshot();
  assert.equal(s.entities.length,6);assert.equal(s.deviceCards.length,3);assert.equal(s.layoutPages.flat().length,3);
  const a=s.deviceCards.find(c=>c.device_id==='dev_a'),b=s.deviceCards.find(c=>c.device_id==='dev_b');
  assert.equal(a.entity_id,'light.main');assert.equal(a.cardSize,'small');assert.equal(a.members.length,3);
  assert.equal(b.quickReference.layout,7,'temperature and humidity share their registered two-metric card');
  assert.deepEqual(Array.from(b.members),['sensor.temp_b','sensor.hum_b']);
  const rail=largeBrightnessRail(r,'light.main'),nx=(rail.rect.x+rail.rect.w*.75)/1024,ny=(rail.rect.y+rail.rect.h/2)/536;
  r.touch('down',nx,ny);r.touch('up',nx,ny);assert.deepEqual(r.controls,[{entity:'light.main',on:true,brightness:75}]);
});

test('grouped device More exposes every member and keeps read-only sensors separate from controls', () => {
  const states=[{entity_id:'light.main',state:'on',attributes:{friendly_name:'主灯',brightness:128,supported_color_modes:['brightness']}},
    {entity_id:'sensor.temperature',state:'24.5',attributes:{friendly_name:'温度',device_class:'temperature',unit_of_measurement:'°C'}}];
  const r=boot(1024,536,{states,devices:[['dev_a','组合设备','',['light.main','sensor.temperature']]],
    savedLayout:JSON.stringify({server:'http://192.168.1.20:8123',sizes:{'device.dev_a':'large'}})});
  r.context.ESPHomeHA.refresh();for(let i=0;i<45;i++)r.tick();
  const row=r.context.ESPHomeHA.snapshot().layoutPages[0][0];
  const more=r.buttons.find(b=>{const v=r.geometry.get(b.id);return !r.hidden.get(b.id)&&v.x===row.x&&v.w===row.w&&v.h<80;});
  assert(more);more.callback();for(let i=0;i<30;i++)r.tick();
  assert(r.context.ESPHomeHA.snapshot().functionControls.some(f=>f.entity==='sensor.temperature'&&f.kind==='read'));
  assert(!r.context.ESPHomeHA.snapshot().parameterControls.some(f=>f.entity==='sensor.temperature'));
  assert(![...r.textValues].some(([id,text])=>!r.hidden.get(id)&&String(text).includes('项功能')),'no separate function navigation');
  assert([...r.textValues].some(([id,text])=>!r.hidden.get(id)&&String(text).includes('24.5')));assert.equal(r.controls.length,0);
  const rail=r.context.ESPHomeHA.snapshot().parameterControls.find(v=>v.key==='brightness_pct');
  assert(rail);const nx=(rail.x+rail.w*(rail.vertical?.5:.6))/1024,ny=(rail.y+rail.h*(rail.vertical?.4:.5))/536;
  r.touch('down',nx,ny);r.touch('move',nx,ny);assert.equal(r.controls.length,0);
  r.touch('up',nx,ny);assert.deepEqual(r.controls,[{entity:'light.main',on:true,brightness:60}]);
});

test('device association failure falls back to state sync without guessed grouping', () => {
  const r=boot(1024,536,{deviceStatus:404});r.context.ESPHomeHA.refresh();for(let i=0;i<35;i++)r.tick();
  const s=r.context.ESPHomeHA.snapshot();assert.equal(s.connected,true);assert.equal(s.deviceCards.length,s.entities.length);
  assert(s.deviceCards.every(c=>!c.device_id));
});

test('registered product model prevents camera indicators and lock settings from replacing the main device', () => {
  const states=[
    {entity_id:'light.cam_indicator',state:'on',attributes:{friendly_name:'摄像机 指示灯',supported_color_modes:['onoff']}},
    {entity_id:'switch.cam_motion_detection_p_5_1',state:'off',attributes:{friendly_name:'摄像机 移动侦测'}},
    {entity_id:'switch.cam_on_p_2_1',state:'on',attributes:{friendly_name:'摄像机控制 开关'}},
    {entity_id:'switch.lock_motion_detection_p_8_1',state:'on',attributes:{friendly_name:'门锁 移动侦测'}},
    {entity_id:'sensor.lock_door_state_p_10_1021',state:'正常上锁',attributes:{friendly_name:'门状态',device_class:'enum'}},
    {entity_id:'sensor.lock_battery',state:'22',attributes:{device_class:'battery',unit_of_measurement:'%'}}];
  const devices=[['camera_id','摄像机','',states.slice(0,3).map(s=>s.entity_id),'chuangmi.camera.079ac1','小白'],
    ['lock_id','门锁','',states.slice(3).map(s=>s.entity_id),'ynsur.lock.v12m','因硕']];
  const r=boot(1024,536,{states,devices});r.context.ESPHomeHA.refresh();for(let i=0;i<50;i++)r.tick();
  const cards=r.context.ESPHomeHA.snapshot().deviceCards;
  assert.equal(cards.length,2);
  assert.equal(cards[0].quickReference.type,'camera');assert.equal(cards[0].quickReference.layout,4);
  assert.equal(cards[0].entity_id,'switch.cam_on_p_2_1');assert.equal(cards[0].members.length,3);
  assert.equal(cards[1].quickReference.type,'lock');assert.equal(cards[1].quickReference.layout,22);
  assert.equal(cards[1].entity_id,'sensor.lock_door_state_p_10_1021');assert.equal(cards[1].members.length,3);
  assert.equal(r.controls.length,0);
});

test('camera without its primary power entity cannot toggle its indicator as device power', () => {
  const states=[{entity_id:'light.cam_indicator',state:'on',attributes:{friendly_name:'指示灯'}}];
  const r=boot(1024,536,{states,devices:[['camera_id','摄像机','',['light.cam_indicator'],'chuangmi.camera.079ac1']]});
  r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
  const card=r.context.ESPHomeHA.snapshot().deviceCards[0];assert.equal(card.quickReference.type,'camera');assert(card.readonly);
  for(const b of r.buttons) {const v=r.geometry.get(b.id);if(!r.hidden.get(b.id)&&v.w===48&&v.h===48)b.callback();}
  assert.equal(r.controls.length,0);
});

test('all 127 registered variants survive grouping instead of collapsing into a generic card', () => {
  const context=vm.createContext({});vm.runInContext(fs.readFileSync(path.join(__dirname,'mijia_card_fixture.js'),'utf8'),context);
  for(const sample of context.__miExamples) {
    const expected=sample.attributes.review_layout;
    const auxiliary={entity_id:'switch.auxiliary',state:'on',attributes:{friendly_name:'辅助开关'}};
    const r=boot(1024,536,{states:[auxiliary,sample],devices:[['product','设备','',[auxiliary.entity_id,sample.entity_id]]]});
    r.context.ESPHomeHA.refresh();for(let i=0;i<35;i++)r.tick();
    const card=r.context.ESPHomeHA.snapshot().deviceCards[0];
    assert.equal(card.quickReference.type,sample.attributes.device_type);
    assert.equal(card.entity_id,sample.entity_id,'declared product capability wins over auxiliary switch');
    if(expected!==undefined)assert.equal(card.quickReference.layout,expected);
    const single=boot(1024,536,{states:[sample]});single.context.ESPHomeHA.refresh();for(let i=0;i<30;i++)single.tick();
    assert.equal(card.quickReference.layout,single.context.ESPHomeHA.snapshot().deviceCards[0].quickReference.layout);
    assert.equal(card.quickReference.variant,single.context.ESPHomeHA.snapshot().deviceCards[0].quickReference.variant);
  }
});

test('real HA device models bind to their registered card categories', () => {
  const evidence=path.resolve(__dirname,'../../../../output/mijia-mixed-fix-20261014');
  if(!fs.existsSync(path.join(evidence,'real-states-ui-fixture.json')))return;
  const states=JSON.parse(fs.readFileSync(path.join(evidence,'real-states-ui-fixture.json'),'utf8'));
  const registry=JSON.parse(fs.readFileSync(path.join(evidence,'real-device-registry.json'),'utf8'));
  const devices=registry.devices.map(d=>[d.device_id,d.name,d.area,d.entities,d.model,d.manufacturer]);
  const r=boot(1024,536,{states,devices});r.context.ESPHomeHA.refresh();for(let i=0;i<50;i++)r.tick();
  const cards=r.context.ESPHomeHA.snapshot().deviceCards;
  assert.equal(cards.length,4,'real-home snapshot has four appliances, no Backup/Sun/TTS/software cards');
  assert([...r.textValues].some(([id,text])=>!r.hidden.get(id)&&text==='4 台设备 · 已同步'));
  assert(![...r.textValues].some(([id,text])=>!r.hidden.get(id)&&String(text).includes('200 个实体')),'entity count belongs in logs');
  assert(r.context.ESPHomeHA.snapshot().activityLog.some(row=>row.detail.includes('200 个实体 · 4 台设备')));
  assert.equal(r.context.ESPHomeHA.snapshot().entities.length,states.length,'raw HA entities remain synchronized');
  const camera=cards.find(c=>c.model==='chuangmi.camera.079ac1');assert(camera);assert.equal(camera.quickReference.type,'camera');
  assert.equal(camera.entity_id,'switch.chuangmi_cn_1184483380_079ac1_on_p_2_1');assert.equal(camera.members.length,100);
  const lock=cards.find(c=>c.model==='ynsur.lock.v12m');assert(lock);assert.equal(lock.quickReference.type,'lock');assert.equal(lock.members.length,39);
  assert.equal(lock.entity_id,'sensor.ynsur_cn_1212516643_v12m_door_state_p_10_1021');
  const lamp=cards.find(c=>c.model==='yeelink.light.lamp28');assert(lamp);assert.equal(lamp.quickReference.layout,21);assert.equal(lamp.members.length,37);
  assert.equal(r.controls.length,0);
});

test('a device with more than 128 entities keeps its full function list and stable size', () => {
  const states=Array.from({length:180},(_,i)=>({entity_id:'sensor.feature_'+i,state:String(i),
    attributes:{friendly_name:'功能 '+i}}));
  const devices=[['dev_many','多功能设备','',states.map(s=>s.entity_id)]];
  const savedLayout=JSON.stringify({server:'http://192.168.1.20:8123',sizes:{'device.dev_many':'large'}});
  const r=boot(1024,536,{states,devices,savedLayout});r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
  const s=r.context.ESPHomeHA.snapshot();assert.equal(s.entities.length,180);
  assert.equal(s.deviceCards.length,1);assert.equal(s.deviceCards[0].members.length,180);assert.equal(s.limited,false);
  const changed=boot(1024,536,{states:states.slice(1),devices,savedLayout});changed.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)changed.tick();
  assert.equal(changed.context.ESPHomeHA.snapshot().deviceCards[0].cardSize,'small','size follows device ID even if its representative entity changes');
});







test('reference lamp small tile tracks every pointer move without repainting the rest of the page', () => {
  for(const dark of [false,true]) {
    const r=boot(1024,536,{dark});r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
    const rail=r.context.ESPHomeHA.snapshot().brightnessControls.find(v=>v.entity==='light.desk');assert(rail);
    assert.equal(rail.h,48);const nx=v=>(rail.x+rail.w*v)/1024,ny=(rail.y+rail.h/2)/536;
    r.touch('down',nx(.2),ny);
    for(let i=0;i<60;i++) {
      const percent=20+i,from=r.nativeWrites.length;r.touch('move',nx(percent/100),ny);r.tick();
      assert.equal(r.context.ESPHomeHA.snapshot().brightnessInput.percent,percent);
      assert([...r.textValues].some(([id,v])=>!r.hidden.get(id)&&v===percent+'% · 已开启'));
      const writes=r.nativeWrites.slice(from);assert(writes.length<=3,'only fill, thumb and percentage change per pointer move');
      assert(!writes.some(v=>v.method==='position'&&v.y!==Math.round(rail.y+10)),'other cards do not move or reflow while dragging');
    }
    assert.equal(r.controls.length,0);r.touch('up',nx(.79),ny);
    assert.deepEqual(r.controls,[{entity:'light.desk',on:true,brightness:79}]);
    deviceBody(r,'light.desk').callback();assert.equal(r.context.ESPHomeHA.snapshot().detailOpen,false,'release click belongs to the slider');
    r.touch('down',.98,.8);deviceBody(r,'light.desk').callback();assert(r.context.ESPHomeHA.snapshot().detailOpen,'ordinary card click still opens details');
  }
});

test('brightness preview stays responsive during background sync and commits once after it finishes', () => {
  const options={};const r=boot(1024,536,options);r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
  options.holdRequests=true;r.context.ESPHomeHA.refresh();for(let i=0;i<12;i++)r.tick();
  const rail=r.context.ESPHomeHA.snapshot().brightnessControls.find(v=>v.entity==='light.desk');assert(rail,'read-only state sync cannot disable the slider');
  const nx=(rail.x+rail.w*.68)/1024,ny=(rail.y+rail.h/2)/536;
  r.touch('down',nx,ny);r.touch('move',nx,ny);r.touch('up',nx,ny);
  assert.equal(r.controls.length,0,'keep the single native request lane');
  assert.equal(r.context.ESPHomeHA.snapshot().brightnessInput.percent,68);
  options.holdRequests=false;for(let i=0;i<30;i++)r.tick();
  assert.deepEqual(r.controls,[{entity:'light.desk',on:true,brightness:68}]);
});

test('an unsubmitted brightness intent never survives a remote home change', () => {
  const options={};const r=boot(1024,536,options);r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
  options.holdRequests=true;r.context.ESPHomeHA.refresh();for(let i=0;i<12;i++)r.tick();
  const rail=r.context.ESPHomeHA.snapshot().brightnessControls.find(v=>v.entity==='light.desk');assert(rail);
  const nx=(rail.x+rail.w*.68)/1024,ny=(rail.y+rail.h/2)/536;
  r.touch('down',nx,ny);r.touch('up',nx,ny);r.remoteConfigure('http://192.168.1.21:8123');r.tick();
  options.holdRequests=false;for(let i=0;i<30;i++)r.tick();assert.equal(r.controls.length,0);
});

test('low brightness retains a rounded cap and the three-pixel inset handle agrees with the touch axis', () => {
  const r=boot(1024,536,{dark:true,states:[{entity_id:'light.desk',state:'on',attributes:{friendly_name:'台灯',brightness:41,supported_color_modes:['brightness']}}]});
  r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
  const rail=r.context.ESPHomeHA.snapshot().brightnessControls[0];
  const handle=[...r.geometry].find(([id,v])=>!r.hidden.get(id)&&v.w===6&&v.h===28);assert(handle);
  assert.equal(handle[1].x,Math.round(rail.x+rail.w*.16-3),'white bar center is the finger coordinate');
  const nx=(rail.x+rail.w*.76)/1024,ny=(rail.y+rail.h/2)/536;r.touch('down',nx,ny);
  assert.equal(r.geometry.get(handle[0]).x,Math.round(rail.x+rail.w*.76-3));
  r.touch('cancel',nx,ny);assert.equal(r.controls.length,0);
});

test('a failed refresh or removed lamp during a drag cannot leave a stuck unsubmitted target', () => {
  for(const mode of ['failed','removed']) {
    const states=[{entity_id:'light.desk',state:'on',attributes:{friendly_name:'台灯',brightness:128,supported_color_modes:['brightness']}}];
    const options={states};const r=boot(1024,536,options);r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
    const rail=r.context.ESPHomeHA.snapshot().brightnessControls[0],nx=(rail.x+rail.w*.6)/1024,ny=(rail.y+rail.h/2)/536;
    if(mode==='failed')options.stateStatus=503;else states.length=0;
    options.holdRequests=true;r.context.ESPHomeHA.refresh();for(let i=0;i<12;i++)r.tick();
    r.touch('down',nx,ny);r.touch('move',nx,ny);options.holdRequests=false;for(let i=0;i<10;i++)r.tick();
    r.touch('up',nx,ny);assert.equal(r.controls.length,0);assert.equal(r.context.ESPHomeHA.snapshot().brightnessInput,null);
  }
});

test('slider gestures never page the board when disabled or on read-only color temperature', () => {
  for(const mode of ['disabled']) {
    const states=[{entity_id:'light.desk',state:'on',attributes:{friendly_name:'灯',brightness:160,color_temp_kelvin:2800,supported_color_modes:['color_temp']}}];
    for(let i=0;i<8;i++)states.push({entity_id:'sensor.extra_'+i,state:'1',attributes:{friendly_name:'读数 '+i}});
    const options={states,canControl:mode!=='disabled'};
    if(mode==='color')options.savedLayout=JSON.stringify({server:'http://192.168.1.20:8123',sizes:{'light.desk':'large'}});
    const r=boot(1024,536,options);r.context.ESPHomeHA.refresh();for(let i=0;i<50;i++)r.tick();
    if(mode==='editing'){layoutButton(r).callback();for(let i=0;i<25;i++)r.tick();}
    let track;
    if(mode==='color') {
      const caption=[...r.textValues].find(([id,v])=>!r.hidden.get(id)&&String(v).startsWith('色温 '));assert(caption);
      const box=r.geometry.get(caption[0]);track={x:box.x,y:box.y+30,w:box.w,h:64};
    } else track=[...r.geometry].find(([id,b])=>!r.hidden.get(id)&&b.h===48&&b.w>200&&b.y>100)[1];
    const nx=(track.x+track.w*.5)/1024,ny=(track.y+track.h/2)/536;
    r.touch('down',nx,ny);r.touch('move',nx-.1,ny);r.swipe('left');r.touch('up',nx-.1,ny);r.swipe('left');
    assert.equal(r.context.ESPHomeHA.snapshot().roomPage,0,mode);assert.equal(r.controls.length,0);
    r.touch('down',.98,.88);r.swipe('left');assert.equal(r.context.ESPHomeHA.snapshot().roomPage,1,'ordinary swipes outside controls remain available');
  }
});

test('left bulb is a passive indicator while the separate right switch controls power', () => {
  const r=boot(1024,536);r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
  const row=r.context.ESPHomeHA.snapshot().layoutPages[0].find(v=>v.entity_id==='light.desk');
  r.touch('down',(row.x+40)/1024,(row.y+44)/536);r.touch('up',(row.x+40)/1024,(row.y+44)/536);
  deviceBody(r,'light.desk').callback();assert(!r.context.ESPHomeHA.snapshot().detailOpen);assert.equal(r.controls.length,0);
  r.touch('down',(row.x+row.w-36)/1024,(row.y+38)/536);
  const power=r.buttons.find(b=>{const box=r.geometry.get(b.id);return !r.hidden.get(b.id)&&box.w===48&&box.h===48&&box.x===row.x+row.w-60;});
  assert(power);power.callback();assert.deepEqual(r.controls,[{entity:'light.desk',on:false,brightness:-1}]);
});



test('an unchanged refresh reuses grouped cards but changed readings still reach the UI', () => {
  const states=[{entity_id:'sensor.room',state:'24.5',attributes:{friendly_name:'温度',device_class:'temperature',unit_of_measurement:'°C'}}];
  const r=boot(1024,536,{states});r.context.ESPHomeHA.refresh();for(let i=0;i<35;i++)r.tick();
  const visible=()=>[...r.textValues].filter(([id])=>!r.hidden.get(id)).map(([,text])=>text);
  r.nativeWrites.length=0;r.context.ESPHomeHA.refresh();for(let i=0;i<35;i++)r.tick();
  assert(!r.nativeWrites.some(w=>w.method==='text'&&w.value==='24.5'),'same reading should not be rewritten');
  states[0].state='26.7';r.context.ESPHomeHA.refresh();for(let i=0;i<35;i++)r.tick();
  assert(visible().some(text=>String(text).includes('26.7')));
  r.context.ESPHomeHA.disconnect();for(let i=0;i<35;i++)r.tick();
  assert(visible().includes('在线 0'),'cached statistics must invalidate on disconnect');
});



test('an in-flight control keeps navigation and other sliders interactive and serializes device commands', () => {
  const options={acceptControls:true},r=boot(1024,536,options);
  r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
  function drag(entity,percent){
    const rail=r.context.ESPHomeHA.snapshot().brightnessControls.find(v=>v.entity===entity);assert(rail);
    const nx=(rail.x+rail.w*percent/100)/1024,ny=(rail.y+rail.h/2)/536;
    r.touch('down',nx,ny);r.touch('move',nx,ny);r.touch('up',nx,ny);
  }
  options.holdRequests=true;drag('light.desk',60);for(let i=0;i<15;i++)r.tick();
  assert.equal(r.controls.length,1);
  drag('light.spotlights',30);drag('light.spotlights',80);for(let i=0;i<15;i++)r.tick();
  assert.equal(r.controls.length,1,'the native lane receives only one request at a time');
  const waiting=r.context.ESPHomeHA.snapshot();assert.equal(waiting.controlQueue.length,1);
  assert.equal(waiting.controlQueue[0].brightness,80,'latest unsent level replaces the previous level for this device');
  assert(!layoutButton(r),'wide-card editor is removed');
  options.holdRequests=false;for(let i=0;i<60;i++)r.tick();
  assert.deepEqual(r.controls,[{entity:'light.desk',on:true,brightness:60},{entity:'light.spotlights',on:true,brightness:80}]);
  assert.equal(r.context.ESPHomeHA.snapshot().controlQueue.length,0);
  assert.equal(r.context.ESPHomeHA.snapshot().selectedId,'light.spotlights','verification must not steal the selection');
});

test('switching the home discards pending device intents without replaying them', () => {
  const options={acceptControls:true},r=boot(1024,536,options);r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
  const drag=(entity,value)=>{const rail=r.context.ESPHomeHA.snapshot().brightnessControls.find(v=>v.entity===entity);
    const x=(rail.x+rail.w*value/100)/1024,y=(rail.y+rail.h/2)/536;r.touch('down',x,y);r.touch('up',x,y);};
  options.holdRequests=true;drag('light.desk',55);for(let i=0;i<15;i++)r.tick();drag('light.spotlights',75);
  assert.equal(r.context.ESPHomeHA.snapshot().controlQueue.length,1);
  r.remoteConfigure('http://192.168.1.21:8123');for(let i=0;i<15;i++)r.tick();
  options.holdRequests=false;for(let i=0;i<60;i++)r.tick();
  assert.equal(r.controls.length,1);assert.equal(r.context.ESPHomeHA.snapshot().controlQueue.length,0);
});

test('frequently used small devices precede unused large cards on the home page', () => {
  const savedLayout=JSON.stringify({server:'http://192.168.1.20:8123',sizes:{'light.desk':'large','light.spotlights':'large'}});
  const savedUsage=JSON.stringify({server:'http://192.168.1.20:8123',counts:{'sensor.temperature':8,'light.c':3}});
  const r=boot(1024,536,{savedLayout,savedUsage});r.context.ESPHomeHA.refresh();for(let i=0;i<45;i++)r.tick();
  const home=r.context.ESPHomeHA.snapshot().layoutPages;
  assert.equal(home[0][0].entity_id,'sensor.temperature');
  assert(home[0].some(row=>row.entity_id==='light.c'),'a common small card must not be pushed behind unused large cards');
});

test('usage is recorded per device, batched in storage and applied only on a home visit', () => {
  let now=1000;
  const states=[{entity_id:'sensor.a',state:'10',attributes:{friendly_name:'A'}},{entity_id:'sensor.b',state:'20',attributes:{friendly_name:'B'}}];
  const devices=[['dev_a','设备 A','',['sensor.a']],['dev_b','设备 B','',['sensor.b']]];
  const r=boot(1024,536,{states,devices,nowMs:()=>now});r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
  const before=r.context.ESPHomeHA.snapshot().layoutPages;
  deviceBody(r,'sensor.b').callback();for(let i=0;i<35;i++)r.tick();
  assert(!r.saves.has('eh_card_usage'),'opening a card must not immediately write flash');
  assert.deepEqual(r.context.ESPHomeHA.snapshot().layoutPages,before,'recording use must not move the current cards');
  const overview=r.buttons.find(b=>{const box=r.geometry.get(b.id);return !r.hidden.get(b.id)&&box.x===12&&box.y===104&&box.h===44;});
  overview.callback();for(let i=0;i<30;i++)r.tick();
  assert.equal(r.context.ESPHomeHA.snapshot().layoutPages[0][0].entity_id,'sensor.b');
  now=12000;for(let i=0;i<25;i++)r.tick();
  const saved=JSON.parse(r.saves.get('eh_card_usage'));assert.equal(saved.counts['device.dev_b'],1);
  const other=boot(1024,536,{states,devices,url:'http://192.168.1.21:8123',savedUsage:JSON.stringify(saved)});
  other.context.ESPHomeHA.refresh();for(let i=0;i<35;i++)other.tick();assert.equal(other.context.ESPHomeHA.snapshot().layoutPages[0][0].entity_id,'sensor.a');
});

test('an unconfirmed write drops queued controls and still allows card editing', () => {
  const options={acceptControls:true,controlStatus:503},r=boot(1024,536,options);r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
  const drag=(entity,value)=>{const rail=r.context.ESPHomeHA.snapshot().brightnessControls.find(v=>v.entity===entity);
    const x=(rail.x+rail.w*value/100)/1024,y=(rail.y+rail.h/2)/536;r.touch('down',x,y);r.touch('up',x,y);};
  options.holdRequests=true;drag('light.desk',60);for(let i=0;i<15;i++)r.tick();drag('light.spotlights',80);
  options.holdRequests=false;for(let i=0;i<45;i++)r.tick();
  assert.equal(r.controls.length,1,'uncertain writes and queued follow-ups must not be replayed');
  assert.equal(r.context.ESPHomeHA.snapshot().controlQueue.length,0);
  assert(!layoutButton(r),'wide editor remains absent after a failed command');
});

test('registry changes update grouped cards even when all state values are unchanged', () => {
  let now=1000;
  const states=[{entity_id:'sensor.room',state:'24.5',attributes:{friendly_name:'温度',device_class:'temperature'}}];
  const devices=[['dev_room','原房间','',['sensor.room']]];
  const r=boot(1024,536,{states,devices,nowMs:()=>now});r.context.ESPHomeHA.refresh();for(let i=0;i<35;i++)r.tick();
  devices[0][1]='新房间';now=310000;r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
  assert.equal(r.context.ESPHomeHA.snapshot().deviceCards[0].name,'新房间');
});

test('native busy admission keeps the latest unsent level and does not invalidate the whole home', () => {
  const options={acceptControls:true},r=boot(1024,536,options);r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
  const drag=value=>{const rail=r.context.ESPHomeHA.snapshot().brightnessControls.find(v=>v.entity==='light.desk');assert(rail);
    const x=(rail.x+rail.w*value/100)/1024,y=(rail.y+rail.h/2)/536;r.touch('down',x,y);r.touch('up',x,y);};
  options.busyAdmissions=1;options.sharedBusy=true;drag(30);for(let i=0;i<20;i++)r.tick();
  assert.equal(r.controls.length,1);assert(r.context.ESPHomeHA.snapshot().connected);
  assert(r.context.ESPHomeHA.snapshot().brightnessInput.pending,'not accepted means waiting, not sent');
  drag(85);for(let i=0;i<15;i++)r.tick();assert.equal(r.controls.length,1);
  options.sharedBusy=false;for(let i=0;i<60;i++)r.tick();
  assert.deepEqual(r.controls.map(v=>v.brightness),[30,85],'only the newest admitted target is sent');
  assert(r.context.ESPHomeHA.snapshot().connected);assert.equal(r.context.ESPHomeHA.snapshot().brightnessInput,null);
});

function parameterDrag(r,key,fraction,height=536){
  if(!r.context.ESPHomeHA.snapshot().parameterControls.some(v=>v.key===key)){deviceBody(r,r.context.ESPHomeHA.snapshot().deviceCards[0].entity_id).callback();for(let i=0;i<30;i++)r.tick();}
  const rail=r.context.ESPHomeHA.snapshot().parameterControls.find(v=>v.key===key);assert(rail,key);
  const nx=(rail.x+rail.w*(rail.vertical?.5:fraction))/1024,ny=(rail.y+rail.h*(rail.vertical?1-fraction:.5))/height;
  r.touch('down',nx,ny);for(let i=0;i<30;i++)r.touch('move',nx,ny);
  assert.equal(r.controls.length,0,'no service while dragging');r.swipe('left');r.touch('up',nx,ny);
}
test('color temperature uses device bounds, follows gestures, and verifies its own attribute',()=>{
  for(const dark of [false,true]){
    const states=[{entity_id:'light.desk',state:'on',attributes:{friendly_name:'台灯',brightness:128,color_temp_kelvin:2700,min_color_temp_kelvin:2700,max_color_temp_kelvin:6500,supported_color_modes:['color_temp']}}];
    const r=boot(1024,536,{dark,states,nativeActions:true,acceptControls:true,savedLayout:JSON.stringify({server:'http://192.168.1.20:8123',sizes:{'light.desk':'large'}})});
    r.context.ESPHomeHA.refresh();for(let i=0;i<50;i++)r.tick();
    if(!r.context.ESPHomeHA.snapshot().detailOpen){deviceBody(r,'light.desk').callback();for(let i=0;i<30;i++)r.tick();}
    assert([...r.styles.values()].some(style=>style.gradientHorizontal===1&&style.gradientEnd===0xfffdfa),'Kelvin rail is a full warm-to-white gradient');
    parameterDrag(r,'color_temp_kelvin',.5);assert.equal(r.controls.length,1);
    assert.deepEqual(r.controls[0],{entity:'light.desk',action:'turn_on',body:{entity_id:'light.desk',color_temp_kelvin:4600}});
    for(let i=0;i<30;i++)r.tick();assert.equal(r.context.ESPHomeHA.snapshot().phase,'ready');
    assert.equal(r.context.ESPHomeHA.snapshot().entities[0].color_temp_kelvin,4600);
    assert.equal(r.context.ESPHomeHA.snapshot().roomPage,0);
    const row=r.context.ESPHomeHA.snapshot().layoutPages[0][0];
    assert(r.context.ESPHomeHA.snapshot().detailOpen);
    assert(r.context.ESPHomeHA.snapshot().functionControls.some(c=>c.key==='color_temp_kelvin'));
    assert(r.context.ESPHomeHA.snapshot().parameterControls.filter(c=>c.entity==='light.desk').length===2,'brightness and Kelvin are both visible without switching tabs');
    assert(r.context.ESPHomeHA.snapshot().parameterControls.every(c=>!c.vertical),'reference layout keeps horizontal controls');
    assert([...r.styles.values()].some(style=>style.gradientHorizontal===1),'More draws warm left to white right');
  }
});
test('More groups typed number, option and button actions while sensors stay read-only',()=>{
  const states=[{entity_id:'light.main',state:'on',attributes:{friendly_name:'主灯',brightness:128,supported_color_modes:['brightness']}},
    {entity_id:'number.limit',state:'2',attributes:{friendly_name:'限值',min:0,max:10,step:.5}},
    {entity_id:'select.mode',state:'auto',attributes:{friendly_name:'模式',options:['auto','quiet','boost']}},
    {entity_id:'button.test',state:'unknown',attributes:{friendly_name:'测试按钮'}},
    {entity_id:'sensor.temp',state:'24',attributes:{friendly_name:'温度'}}];
  const services=[{domain:'light',services:{turn_on:{},turn_off:{}}},{domain:'number',services:{set_value:{}}},{domain:'select',services:{select_option:{}}},{domain:'button',services:{press:{}}}];
  const r=boot(1024,536,{nativeActions:true,acceptControls:true,states,services,devices:[['dev','灯具','',states.map(s=>s.entity_id)]]});
  r.context.ESPHomeHA.refresh();for(let i=0;i<50;i++)r.tick();deviceBody(r,'light.main').callback();for(let i=0;i<20;i++)r.tick();assertUnifiedDetail(r);
  assert.equal(r.context.ESPHomeHA.snapshot().selectedId,'light.main');
  parameterDrag(r,'value',.75);assert.deepEqual(r.controls[0].body,{entity_id:'number.limit',value:7.5});
  for(let i=0;i<20;i++)r.tick();assert.equal(r.context.ESPHomeHA.snapshot().phase,'ready');
  assert.equal(r.context.ESPHomeHA.snapshot().selectedId,'light.main','auxiliary controls retain the physical device view');
  function clickValue(text){
    const label=[...r.textValues].find(([id,t])=>!r.hidden.get(id)&&t===text);assert(label,text+' '+JSON.stringify([...r.textValues].filter(([id])=>!r.hidden.get(id)).map(([,value])=>value)));
    const g=r.geometry.get(label[0]),x=g.x+g.w/2,y=g.y+g.h/2;
    const b=r.buttons.find(b=>{const q=r.geometry.get(b.id);return !r.hidden.get(b.id)&&x>=q.x&&x<=q.x+q.w&&y>=q.y&&y<=q.y+q.h;});assert(b);b.callback();
  }
  clickValue('安静');assert.deepEqual(r.controls.at(-1).body,{entity_id:'select.mode',option:'quiet'});
  for(let i=0;i<20;i++)r.tick();assert.equal(r.context.ESPHomeHA.snapshot().phase,'ready');
  clickValue('执行');assert.equal(r.controls.at(-1).action,'press');
  for(let i=0;i<20;i++)r.tick();assert.equal(r.context.ESPHomeHA.snapshot().phase,'ready','stateless action accepts unchanged state');
  clickIndicator(r,1,'detailPageIndicator');
  assert(r.context.ESPHomeHA.snapshot().functionControls.some(f=>f.entity==='sensor.temp'&&f.kind==='read'));
  assert(!r.context.ESPHomeHA.snapshot().parameterControls.some(f=>f.entity==='sensor.temp'));
  assert.equal(r.context.ESPHomeHA.snapshot().selectedId,'light.main');

});
test('brightness and temperature intents on one lamp do not replace each other',()=>{
  const states=[{entity_id:'light.desk',state:'on',attributes:{friendly_name:'灯',brightness:128,color_temp_kelvin:2700,supported_color_modes:['color_temp']}}];
  const opts={nativeActions:true,acceptControls:true,states,savedLayout:JSON.stringify({server:'http://192.168.1.20:8123',sizes:{'light.desk':'large'}})};
  const r=boot(1024,536,opts);r.context.ESPHomeHA.refresh();for(let i=0;i<50;i++)r.tick();opts.sharedBusy=true;opts.busyAdmissions=1;
  const b=r.context.ESPHomeHA.snapshot().brightnessControls[0],ny=(b.y+b.h/2)/536,nx=(b.x+b.w*.7)/1024;
  r.touch('down',nx,ny);r.touch('up',nx,ny);assert.equal(r.controls.length,1);
  r.touch('down',.98,.8);deviceBody(r,'light.desk').callback();for(let i=0;i<30;i++)r.tick();
  const t=r.context.ESPHomeHA.snapshot().parameterControls.find(row=>row.key==='color_temp_kelvin'),ty=(t.y+t.h/2)/536,tx=(t.x+t.w*.6)/1024;
  r.touch('down',tx,ty);r.touch('up',tx,ty);assert.equal(r.context.ESPHomeHA.snapshot().controlQueue.length,1);
  opts.sharedBusy=false;for(let i=0;i<60;i++)r.tick();assert.equal(r.controls.length,3);
  assert.equal(r.controls[1].body.brightness_pct,70);assert(r.controls[2].body.color_temp_kelvin);
  assert.equal(r.context.ESPHomeHA.snapshot().phase,'ready');
});

function commonProfileSamples(){
  return [
    {entity_id:'light.bedside',state:'on',attributes:{friendly_name:'床头灯',brightness:200,supported_color_modes:['color_temp'],color_temp_kelvin:2700,min_color_temp_kelvin:2700,max_color_temp_kelvin:6500}},
    {entity_id:'switch.plug',state:'on',attributes:{friendly_name:'插座'}},
    {entity_id:'fan.standard',state:'on',attributes:{friendly_name:'风扇',supported_features:59,percentage:40,percentage_step:10,preset_mode:'normal',preset_modes:['normal','quiet','sleep'],oscillating:false}},
    {entity_id:'climate.room',state:'cool',attributes:{friendly_name:'空调',supported_features:425,temperature:24,min_temp:16,max_temp:30,target_temp_step:.5,hvac_modes:['cool','heat','off'],fan_mode:'auto',fan_modes:['auto','low','high'],swing_modes:['off','vertical']}},
    {entity_id:'cover.curtain',state:'open',attributes:{friendly_name:'窗帘',supported_features:15,current_position:60}},
    {entity_id:'fan.purifier',state:'on',attributes:{friendly_name:'净化器',device_type:'air-purifier',supported_features:57,percentage:40,preset_modes:['auto','quiet'],pm25:12}},
    {entity_id:'humidifier.room',state:'on',attributes:{friendly_name:'加湿器',supported_features:1,humidity:55,min_humidity:30,max_humidity:80,current_humidity:48,available_modes:['auto','sleep']}},
    {entity_id:'media_player.speaker',state:'playing',attributes:{friendly_name:'音箱',supported_features:4|8|16|32|128|256|16384|2048|4096|1,volume_level:.35,source_list:['Bluetooth','TV'],source:'Bluetooth'}},
    {entity_id:'vacuum.robot',state:'idle',attributes:{friendly_name:'扫地机',supported_features:8192|4|8|16|32|64,battery_level:86,fan_speed:'standard',fan_speed_list:['quiet','standard','max']}},
    {entity_id:'water_heater.bath',state:'on',attributes:{friendly_name:'热水器',supported_features:3,temperature:50,min_temp:35,max_temp:75,target_temp_step:1,operation_mode:'electric',operation_list:['electric','eco','off']}},
    {entity_id:'sensor.temp',state:'24.5',attributes:{friendly_name:'温度',device_class:'temperature',unit_of_measurement:'°C'}},
    {entity_id:'sensor.humidity',state:'48',attributes:{friendly_name:'湿度',device_class:'humidity',unit_of_measurement:'%'}}
  ];
}
function commonServices(){
  const catalog={light:['turn_on','turn_off'],switch:['turn_on','turn_off'],fan:['turn_on','turn_off','set_percentage','set_preset_mode','oscillate'],climate:['turn_on','turn_off','set_temperature','set_hvac_mode','set_fan_mode','set_swing_mode'],cover:['set_cover_position','open_cover','close_cover','stop_cover'],humidifier:['turn_on','turn_off','set_humidity','set_mode'],media_player:['turn_on','turn_off','volume_set','volume_mute','select_source','media_play','media_pause','media_stop','media_next_track','media_previous_track'],vacuum:['start','pause','stop','return_to_base','set_fan_speed'],water_heater:['set_temperature','set_operation_mode','turn_on','turn_off']};
  return Object.entries(catalog).map(([domain,services])=>({domain,services:Object.fromEntries(services.map(service=>[service,{}]))}));
}
test('all eleven common profiles are registered and match grouped physical devices without replacing the 127 references',()=>{
  const states=commonProfileSamples(),devices=states.slice(0,11).map((row,i)=>['dev'+i,row.attributes.friendly_name,'',i===10?['sensor.temp','sensor.humidity']:[row.entity_id]]);
  const r=boot(1024,536,{states,devices,services:commonServices(),nativeActions:true});r.context.ESPHomeHA.refresh();for(let i=0;i<50;i++)r.tick();
  const snapshot=r.context.ESPHomeHA.snapshot(),catalog=JSON.parse(fs.readFileSync(path.join(__dirname,'../../homeassistant/common-card-profiles.json'),'utf8'));
  assert.deepEqual(Array.from(snapshot.registeredCommonCardProfiles,p=>p.id),catalog.profiles.map(p=>p.id));
  assert.equal(snapshot.deviceCards.length,11);assert.equal(new Set(snapshot.deviceCards.map(card=>card.commonProfile)).size,11);
  assert.equal(snapshot.registeredCardVariants,127);assert.equal(snapshot.registeredCardTypes.length,59);
  const sensor=snapshot.deviceCards.find(card=>card.commonProfile==='temperature-humidity');assert.equal(sensor.members.length,2);
  assert.equal(r.controls.length,0,'registering and drawing profiles never issues control commands');
});
test('common small and large ranges own gestures and send exact typed parameters through the existing service',()=>{
  const cases=[['fan.standard','percentage','set_percentage',50],['climate.room','temperature','set_temperature',23],['cover.curtain','position','set_cover_position',50],['humidifier.room','humidity','set_humidity',55],['media_player.speaker','volume_level','volume_set',.5],['water_heater.bath','temperature','set_temperature',55]];
  for(const size of ['small'])for(const [id,key,action,value] of cases){
    const states=commonProfileSamples().filter(row=>row.entity_id===id),savedLayout=JSON.stringify({server:'http://192.168.1.20:8123',sizes:{[id]:size}});
    const r=boot(1024,536,{states,services:commonServices(),nativeActions:true,acceptControls:true,savedLayout});r.context.ESPHomeHA.refresh();for(let i=0;i<50;i++)r.tick();
    if(key==='temperature'){deviceBody(r,id).callback();for(let i=0;i<30;i++)r.tick();}
    const rect=r.context.ESPHomeHA.snapshot().parameterControls.find(row=>row.entity===id&&row.key===key);assert(rect,id+' '+size);
    const nx=(rect.x+rect.w*.5)/1024,ny=(rect.y+rect.h/2)/536;
    r.touch('down',nx,ny);r.touch('move',nx,ny);r.swipe('left');assert.equal(r.controls.length,0);assert.equal(r.context.ESPHomeHA.snapshot().roomPage,0);
    r.touch('up',nx,ny);assert.equal(r.controls.length,1);assert.equal(r.controls[0].action,action);assert.deepEqual(r.controls[0].body,{entity_id:id,[key]:value});
    for(let i=0;i<20;i++)r.tick();assert.equal(r.context.ESPHomeHA.snapshot().phase,'ready',id+' confirms the right attribute');
  }
});
test('advertised feature limits hide unsupported media actions and cover controls',()=>{
  const states=[{entity_id:'media_player.volume_only',state:'on',attributes:{friendly_name:'只调音量',supported_features:4,volume_level:.2,source_list:['TV']}},
    {entity_id:'cover.position_only',state:'open',attributes:{friendly_name:'只调位置',supported_features:4,current_position:30}}];
  for(const state of states){const r=boot(1024,536,{states:[state],services:commonServices(),nativeActions:true});r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();deviceBody(r,state.entity_id).callback();for(let i=0;i<20;i++)r.tick();assertUnifiedDetail(r);
    const specs=r.context.ESPHomeHA.snapshot().functionControls;assert.equal(specs.length,1);assert.equal(specs[0].key,state.entity_id.startsWith('cover')?'position':'volume_level');assert.equal(r.controls.length,0);
  }
});
test('long option lists choose an exact raw value without cycling or leaking input to the page',()=>{
  const states=[{entity_id:'light.main',state:'on',attributes:{friendly_name:'主灯',brightness:128,supported_color_modes:['brightness']}},{entity_id:'select.long_mode',state:'m0',attributes:{friendly_name:'模式',options:['m0','m1','m2','m3','m4','m5']}}];
  const r=boot(1024,536,{nativeActions:true,acceptControls:true,states,services:[...commonServices(),{domain:'select',services:{select_option:{}}}],devices:[['dev','灯具','',states.map(row=>row.entity_id)]]});r.context.ESPHomeHA.refresh();for(let i=0;i<50;i++)r.tick();deviceBody(r,'light.main').callback();for(let i=0;i<20;i++)r.tick();assertUnifiedDetail(r);
  function click(text){const button=r.buttons.find(button=>!r.hidden.get(button.id)&&r.textValues.get(button.id)===text);if(button){button.callback();return;}const label=[...r.textValues].find(([id,value])=>!r.hidden.get(id)&&value===text);assert(label,text);const g=r.geometry.get(label[0]);const target=r.buttons.find(button=>{const b=r.geometry.get(button.id);return !r.hidden.get(button.id)&&g.x+g.w/2>=b.x&&g.x+g.w/2<=b.x+b.w&&g.y+g.h/2>=b.y&&g.y+g.h/2<=b.y+b.h;});assert(target);target.callback();}
  click('m0  >');for(let i=0;i<10;i++)r.tick();assert(r.context.ESPHomeHA.snapshot().optionPicker);assert.equal(r.controls.length,0);
  r.swipe('left');r.touch('down',.5,.5);r.touch('up',.8,.5);assert.equal(r.controls.length,0);
  clickIndicator(r,1,'optionPageIndicator');for(let i=0;i<10;i++)r.tick();click('m5');assert.deepEqual(r.controls[0].body,{entity_id:'select.long_mode',option:'m5'});for(let i=0;i<20;i++)r.tick();assert.equal(r.context.ESPHomeHA.snapshot().optionPicker,null);assert.equal(r.context.ESPHomeHA.snapshot().selectedId,'light.main');
  assert.equal(r.buttons.length,64,'profile options reuse the native pool rather than adding native controls per device');
});

test('climate power turns off an active HVAC mode while a temperature intent is pending',()=>{
  const state=commonProfileSamples().find(row=>row.entity_id==='climate.room');
  const r=boot(1024,536,{states:[state],services:commonServices(),nativeActions:true,acceptControls:true});r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
  const plus=r.buttons.find(button=>!r.hidden.get(button.id)&&r.textValues.get(button.id)==='+');assert(plus);plus.callback();assert.equal(r.controls[0].action,'set_temperature');
  const glyph=[...r.textValues].find(([id,text])=>!r.hidden.get(id)&&text==='\uf011');assert(glyph);
  const g=r.geometry.get(glyph[0]),x=g.x+g.w/2,y=g.y+g.h/2;
  const power=r.buttons.find(button=>{const b=r.geometry.get(button.id);return !r.hidden.get(button.id)&&b.w===48&&b.h===48&&x>=b.x&&x<=b.x+b.w&&y>=b.y&&y<=b.y+b.h;});assert(power);r.touch('down',x/1024,y/536);r.touch('up',x/1024,y/536);power.callback();
  for(let i=0;i<40;i++)r.tick();assert.equal(r.controls.at(-1).action,'turn_off');assert.equal(r.controls.at(-1).entity,'climate.room');
});

test('every common small card opens the unified detailed page without the intermediate quick card',()=>{
  for(const state of commonProfileSamples().slice(0,11)){
    const r=boot(1024,536,{states:[state],services:commonServices(),nativeActions:true});r.context.ESPHomeHA.refresh();for(let i=0;i<35;i++)r.tick();
    deviceBody(r,state.entity_id).callback();for(let i=0;i<25;i++)r.tick();const snapshot=r.context.ESPHomeHA.snapshot();
    assert.equal(snapshot.view,'detail');assert(snapshot.detailAdvanced,state.entity_id);assert.equal(snapshot.selectedId,state.entity_id);
    assert(![...r.textValues].some(([id,text])=>!r.hidden.get(id)&&text==='更多操作'),state.entity_id+' has no duplicated middle page');
    assert.equal(r.controls.length,0,'opening details does not issue commands');
  }
});

test('one back action from direct details restores the same board page and saved card combination',()=>{
  const r=boot(1024,536);r.context.ESPHomeHA.refresh();for(let i=0;i<35;i++)r.tick();r.swipe('left');for(let i=0;i<20;i++)r.tick();
  const before=r.context.ESPHomeHA.snapshot();assert.equal(before.roomPage,1);const entry=before.layoutPages[1][0];
  deviceBody(r,entry.entity_id).callback();for(let i=0;i<25;i++)r.tick();assert(r.context.ESPHomeHA.snapshot().detailAdvanced);
  const back=r.buttons.find(button=>{const box=r.geometry.get(button.id);return !r.hidden.get(button.id)&&box.x===176&&box.w===32&&box.h===30;});assert(back);back.callback();for(let i=0;i<25;i++)r.tick();
  const after=r.context.ESPHomeHA.snapshot();assert.equal(after.view,'list');assert.equal(after.roomPage,before.roomPage);assert.equal(after.nav,before.nav);
  assert.equal(JSON.stringify(after.layoutPages),JSON.stringify(before.layoutPages));assert.equal(r.controls.length,0);
});

test('all 59 device types have a unique profile and the additional 42 render both card sizes and direct details',()=>{
  const catalog=JSON.parse(fs.readFileSync(path.join(__dirname,'../../homeassistant/quick-card-catalog.json'),'utf8'));
  const registry=JSON.parse(fs.readFileSync(path.join(__dirname,'../../homeassistant/common-card-profiles.json'),'utf8'));
  const covered=registry.profiles.flatMap(profile=>profile.types);
  assert.equal(new Set(covered).size,covered.length);assert.deepEqual([...covered].sort(),catalog.types.slice().sort());
  const fixtures=vm.createContext({});vm.runInContext(fs.readFileSync(path.join(__dirname,'mijia_card_fixture.js'),'utf8'),fixtures);
  const additional=registry.profiles.filter(profile=>profile.layout);assert.equal(additional.length,42);
  for(const profile of additional)for(const size of ['small'])for(const dark of [false,true]){
    const sample=fixtures.__miExamples.find(row=>profile.types.includes(row.attributes.device_type));assert(sample,profile.id);
    const savedLayout=JSON.stringify({server:'http://192.168.1.20:8123',sizes:{[sample.entity_id]:size}});
    const r=boot(1024,536,{states:[sample],nativeActions:true,services:commonServices(),savedLayout,dark});
    r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
    const card=r.context.ESPHomeHA.snapshot().deviceCards[0];assert.equal(card.commonProfile,profile.id);
    for(const [id,box] of r.geometry)if(!r.hidden.get(id))assert(box.x>=0&&box.y>=0&&box.x+box.w<=1024&&box.y+box.h<=536,profile.id+' '+size+' clips '+JSON.stringify(box));
    if(size==='small'){
      deviceBody(r,sample.entity_id).callback();for(let i=0;i<25;i++)r.tick();assertUnifiedDetail(r);
      const details=r.context.ESPHomeHA.snapshot();assert.equal(details.view,'detail');
      if(profile.readonly)assert(details.functionControls.every(spec=>spec.kind==='read'),profile.id+' is read-only');
      for(const [id,box] of r.geometry)if(!r.hidden.get(id))assert(box.x>=0&&box.y>=0&&box.x+box.w<=1024&&box.y+box.h<=536,profile.id+' detail clips '+JSON.stringify(box));
      assert.equal(r.buttons.length,64,'new profiles share the existing button pool');
    }
    assert.equal(r.controls.length,0,'rendering never operates a device');
  }
});

test('air monitor preserves zero, small decimals, source units and grouped sensor readings without duplicates',()=>{
  const states=[
    {entity_id:'sensor.pm',state:'0',attributes:{friendly_name:'PM2.5',device_type:'air-monitor',device_class:'pm25',unit_of_measurement:'µg/m³',hcho:.023}},
    {entity_id:'sensor.temp_f',state:'75.2',attributes:{friendly_name:'温度',device_class:'temperature',unit_of_measurement:'°F'}},
    {entity_id:'sensor.hum',state:'0',attributes:{friendly_name:'湿度',device_class:'humidity',unit_of_measurement:'%'}}];
  const r=boot(1024,536,{states,devices:[['air','空气监测','',states.map(row=>row.entity_id)]]});
  r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
  const card=r.context.ESPHomeHA.snapshot().deviceCards[0],readings=Array.from(card.readings);
  assert.equal(readings.find(row=>row.key==='pm2.5-density').display,'0.0 ug/m3');
  assert.equal(readings.find(row=>row.key==='hcho-density').display,'0.023 mg/m3');
  assert.equal(readings.find(row=>row.key==='temperature').display,'75.2 °F');
  assert.equal(readings.find(row=>row.key==='relative-humidity').display,'0.0%');
  deviceBody(r,card.entity_id).callback();for(let i=0;i<25;i++)r.tick();
  const fields=r.context.ESPHomeHA.snapshot().functionControls;assert.equal(fields.length,4);
  assert.equal(new Set(fields.map(spec=>spec.title)).size,4);assert(fields.every(spec=>spec.kind==='read'));
});

test('alarms distinguish triggered, untriggered and unavailable without fabricating normal readings',()=>{
  for(const [type,active] of [['smoke-sensor','检测到烟雾'],['gas-sensor','检测到燃气'],['submersion-sensor','检测到漏水']]){
    for(const state of ['on','off','unknown','unavailable']){
      const sample={entity_id:'binary_sensor.alarm',state,attributes:{device_type:type,friendly_name:'报警传感器'}};
      const r=boot(1024,536,{states:[sample]});r.context.ESPHomeHA.refresh();for(let i=0;i<35;i++)r.tick();
      const reading=r.context.ESPHomeHA.snapshot().deviceCards[0].readings[0];
      assert.equal(reading.alert,state==='on');
      if(state==='on')assert.equal(reading.display,active);
      if(['unknown','unavailable'].includes(state))assert.equal(reading.display,'--');
      assert.equal(r.controls.length,0);
    }
  }
});

test('heater binds existing temperature controls and camera details remain explicit about unavailable video',()=>{
  const heater={entity_id:'climate.heater',state:'heat',attributes:{device_type:'heater',friendly_name:'取暖器',supported_features:385,temperature:23,current_temperature:20,min_temp:16,max_temp:30,hvac_modes:['heat','off']}};
  const r=boot(1024,536,{states:[heater],services:commonServices(),nativeActions:true,acceptControls:true});
  r.context.ESPHomeHA.refresh();for(let i=0;i<35;i++)r.tick();parameterDrag(r,'temperature',.5);
  assert.deepEqual(r.controls[0].body,{entity_id:'climate.heater',temperature:23});
  const camera=boot(1024,536,{states:[{entity_id:'camera.entry',state:'idle',attributes:{friendly_name:'门口摄像头',device_type:'camera'}}]});
  camera.context.ESPHomeHA.refresh();for(let i=0;i<35;i++)camera.tick();deviceBody(camera,'camera.entry').callback();for(let i=0;i<25;i++)camera.tick();
  assert([...camera.textValues].some(([id,text])=>!camera.hidden.get(id)&&text.includes('暂无预览画面')));
  assert(camera.context.ESPHomeHA.snapshot().functionControls.every(spec=>spec.kind==='read'));
  assert.equal(camera.controls.length,0);
});

test('a declared read-only sensor never acquires a power button from a switch-shaped entity',()=>{
  const r=boot(1024,536,{states:[{entity_id:'switch.smoke',state:'on',attributes:{friendly_name:'烟雾状态',device_type:'smoke-sensor'}}],
    nativeActions:true,services:[{domain:'switch',services:{turn_on:{},turn_off:{}}}]});
  r.context.ESPHomeHA.refresh();for(let i=0;i<35;i++)r.tick();
  assert(![...r.textValues].some(([id,text])=>!r.hidden.get(id)&&text==='\uf011'));
  deviceBody(r,'switch.smoke').callback();for(let i=0;i<25;i++)r.tick();
  assert(r.context.ESPHomeHA.snapshot().functionControls.every(spec=>spec.kind==='read'));
  for(const button of r.buttons)if(!r.hidden.get(button.id))button.callback();
  assert.equal(r.controls.length,0);
});

test('unified firmware ignores historical wide sizes and only More opens all 59 categories',()=>{
  const fixture=JSON.parse(fs.readFileSync(path.join(__dirname,'all_device_preview.json'),'utf8'));
  for(const sample of fixture.cases){
    const entity=sample.states[0].entity_id;
    const r=boot(1024,536,{states:sample.states,services:fixture.services,nativeActions:true,
      savedLayout:JSON.stringify({server:'http://192.168.1.20:8123',sizes:{[entity]:'large'}})});
    r.context.ESPHomeHA.refresh();for(let i=0;i<45;i++)r.tick();
    const state=r.context.ESPHomeHA.snapshot(),row=state.layoutPages[0][0];
    assert.equal(row.size,'small',sample.type);assert(!state.layoutEditing);assert(!layoutButton(r));
    const x=row.x+row.w/2,y=row.y+22;
    assert(!r.buttons.some(button=>{const b=r.geometry.get(button.id);return !r.hidden.get(button.id)&&x>=b.x&&x<=b.x+b.w&&y>=b.y&&y<=b.y+b.h;}),'body has no action: '+sample.type);
    deviceBody(r,entity).callback();for(let i=0;i<30;i++)r.tick();assertUnifiedDetail(r);
    assert.equal(r.controls.length,0,sample.type+' opens without sending a device command');
    for(const [id,b] of r.geometry)if(!r.hidden.get(id))assert(b.x>=0&&b.y>=0&&b.x+b.w<=1024&&b.y+b.h<=536,sample.type+' visible geometry');
  }
});

test('temperature small-card stepper uses advertised steps and never opens More',()=>{
  for(const domain of ['climate','water_heater']){
    const state={entity_id:domain+'.target',state:domain==='climate'?'heat':'on',attributes:{friendly_name:'Temperature',temperature:24,min_temp:16,max_temp:30,target_temp_step:.5,
      supported_features:domain==='climate'?425:3,hvac_modes:['heat','off'],operation_list:['electric','off']}};
    const r=boot(1024,536,{states:[state],services:commonServices(),nativeActions:true,acceptControls:true});r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
    const plus=r.buttons.find(button=>!r.hidden.get(button.id)&&r.textValues.get(button.id)==='+');assert(plus);plus.callback();
    assert.equal(r.controls[0].action,'set_temperature');assert.equal(r.controls[0].body.temperature,24.5);
    assert(!r.context.ESPHomeHA.snapshot().detailOpen);
  }
});

test('scene and log navigation replace environment/system and retain sensors under devices',()=>{
  const states=[{entity_id:'scene.evening',state:'2026-10-15T00:00:00',attributes:{friendly_name:'晚间场景'}},
    {entity_id:'script.sleep',state:'off',attributes:{friendly_name:'睡眠脚本'}},
    {entity_id:'sensor.room',state:'24.5',attributes:{friendly_name:'室温',device_class:'temperature',unit_of_measurement:'°C'}}];
  const r=boot(1024,536,{states});r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
  const select=y=>{const button=r.buttons.find(button=>{const box=r.geometry.get(button.id);return !r.hidden.get(button.id)&&box.x===12&&box.y===104+(y-70)/46*52;});assert(button);button.callback();for(let i=0;i<25;i++)r.tick();};
  assert([...r.textValues.values()].includes('场景'));assert([...r.textValues.values()].includes('日志'));
  assert(![...r.textValues.values()].includes('环境'));assert(![...r.textValues.values()].includes('系统'));
  select(162);let snapshot=r.context.ESPHomeHA.snapshot();assert.equal(snapshot.nav,'scenes');
  assert.deepEqual([...snapshot.layoutPages.flat().map(row=>row.entity_id)].sort(),['scene.evening','script.sleep']);
  select(116);snapshot=r.context.ESPHomeHA.snapshot();assert(snapshot.layoutPages.flat().some(row=>row.entity_id==='sensor.room'));
  select(208);snapshot=r.context.ESPHomeHA.snapshot();assert.equal(snapshot.nav,'logs');assert(snapshot.activityLog.length>0);
  assert(snapshot.layoutPages.flat().every(row=>row.entity_id.startsWith('local.log_')));
  for(const button of r.buttons){const box=r.geometry.get(button.id);if(!r.hidden.get(button.id)&&box.x>=180&&box.y>=76&&box.y<450)button.callback();}
  assert.equal(r.controls.length,0,'log entries are never device controls');
});

test('session logs stay bounded and do not repeat unchanged periodic synchronization',()=>{
  const states=[{entity_id:'sensor.room',state:'0',attributes:{friendly_name:'Room'}}];
  const r=boot(1024,536,{states});r.context.ESPHomeHA.refresh();for(let i=0;i<35;i++)r.tick();
  const count=r.context.ESPHomeHA.snapshot().activityLog.length;
  r.context.ESPHomeHA.refresh();for(let i=0;i<35;i++)r.tick();assert.equal(r.context.ESPHomeHA.snapshot().activityLog.length,count);
  for(let i=1;i<=40;i++){states[0].state=String(i);r.context.ESPHomeHA.refresh();for(let j=0;j<30;j++)r.tick();}
  assert.equal(r.context.ESPHomeHA.snapshot().activityLog.length,32);
});

function chooseSection(r,y){
  const button=r.buttons.find(button=>{const box=r.geometry.get(button.id);return !r.hidden.get(button.id)&&box.x===12&&box.y===104+(y-70)/46*52;});
  assert(button);button.callback();for(let i=0;i<45;i++)r.tick();
}
function clickIndicator(r,page,kind="pageIndicator"){
  const slot=r.context.ESPHomeHA.snapshot()[kind].slots.find(slot=>slot.page===page);
  assert(slot,'requested page has a visible dot');
  const button=r.buttons.find(button=>{const box=r.geometry.get(button.id);return !r.hidden.get(button.id)&&box.x===slot.x&&box.y===slot.y&&box.w===slot.w&&box.h===slot.h;});
  assert(button);button.callback();for(let i=0;i<25;i++)r.tick();
}
test('capsule indicators jump directly, track swipes and stay centered without overlapping footer text',()=>{
  for(const [width,height] of [[1024,568],[1024,536],[640,400]])for(const dark of [false,true]){
    const states=Array.from({length:18},(_,i)=>({entity_id:'light.pager_'+i,state:'on',attributes:{friendly_name:'灯 '+i,brightness:128,supported_color_modes:['brightness']}}));
    const r=boot(width,height,{states,dark});r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
    const initial=r.context.ESPHomeHA.snapshot().pageIndicator;assert(initial.visible);assert(initial.total>=3);
    clickIndicator(r,2);assert.equal(r.context.ESPHomeHA.snapshot().roomPage,2);
    r.touch('down',.98,.12);r.swipe('right');for(let i=0;i<25;i++)r.tick();
    const indicator=r.context.ESPHomeHA.snapshot().pageIndicator;assert.equal(r.context.ESPHomeHA.snapshot().roomPage,1);
    assert.equal(indicator.slots.filter(slot=>slot.active).length,1);
    const first=indicator.slots[0],last=indicator.slots.at(-1);
    assert(Math.abs((first.x+last.x+last.w)/2-(164+(width-164)/2))<=1);
    for(const slot of indicator.slots){
      assert(slot.x>=164&&slot.x+slot.w<=width&&slot.y>=0&&slot.y+slot.h<=height);
      const mark=[...r.geometry].find(([id,box])=>!r.hidden.get(id)&&box.h===6&&box.w===(slot.active?20:6)&&box.x+box.w/2===slot.x+12&&box.y+3===slot.y+20);
      assert(mark,'every touch target has its centered dot or selected pill');
    }
    for(const [id,box] of r.geometry)if(!r.hidden.get(id)&&r.textValues.get(id)&&box.x===180&&box.y>=height-54)
      assert(box.x+box.w<=first.x-12,'footer text ends before the capsule');
    const allocated=r.geometry.size;clickIndicator(r,1);assert.equal(r.geometry.size,allocated);assert.equal(r.controls.length,0);
  }
});
test('bounded capsule slots reach every page, survive detail pooling and clamp after device removal',()=>{
  const states=Array.from({length:59},(_,i)=>({entity_id:'light.pager_'+i,state:'on',attributes:{brightness:128,supported_color_modes:['brightness']}}));
  const r=boot(1024,568,{states,frontOrder:true});r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
  assert.equal(r.context.ESPHomeHA.snapshot().pageIndicator.total,10);
  for(let page=1;page<10;page++)clickIndicator(r,page);
  const last=r.context.ESPHomeHA.snapshot();assert.equal(last.roomPage,9);assert.equal(last.pageIndicator.slots.length,5);assert(last.pageIndicator.slots.at(-1).active);
  deviceBody(r,last.layoutPages[9][0].entity_id).callback();for(let i=0;i<30;i++)r.tick();assert(!r.context.ESPHomeHA.snapshot().pageIndicator.visible);
  const back=r.buttons.find(button=>{const box=r.geometry.get(button.id);return !r.hidden.get(button.id)&&box.x===176&&box.w===32&&box.h===30;});back.callback();for(let i=0;i<30;i++)r.tick();
  assert(r.context.ESPHomeHA.snapshot().pageIndicator.visible);assert.equal(r.context.ESPHomeHA.snapshot().roomPage,9);
  const allocated=r.geometry.size;
  for(let pass=0;pass<20;pass++)for(let page=pass%2?1:8;pass%2?page<=9:page>=0;pass%2?page++:page--)clickIndicator(r,page);
  assert.equal(r.geometry.size,allocated,'native widgets are reused throughout repeated paging');assert.equal(r.controls.length,0);
  states.splice(1);r.context.ESPHomeHA.refresh();for(let i=0;i<45;i++)r.tick();
  const shrunk=r.context.ESPHomeHA.snapshot();assert.equal(shrunk.roomPage,0);assert(!shrunk.pageIndicator.visible);assert.equal(shrunk.pageIndicator.slots.length,0);
});
test('scene grids share capsule navigation without executing a scene',()=>{
  const sceneStates=Array.from({length:18},(_,i)=>({entity_id:'scene.pager_'+i,state:'unknown',attributes:{friendly_name:'场景 '+i}}));
  const r=boot(1024,568,{sceneSync:true,sceneStates});r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();chooseSection(r,162);
  assert.equal(r.context.ESPHomeHA.snapshot().pageIndicator.total,3);clickIndicator(r,2);assert.equal(r.context.ESPHomeHA.snapshot().roomPage,2);
  assert.equal(r.controls.length,0);chooseSection(r,208);assert.equal(r.context.ESPHomeHA.snapshot().roomPage,0);
});
function sceneExecuteButton(r,id){
  const row=r.context.ESPHomeHA.snapshot().layoutPages.flat().find(row=>row.entity_id===id);assert(row);
  return r.buttons.find(button=>{const box=r.geometry.get(button.id);return !r.hidden.get(button.id)&&box.x===row.x+16&&box.y===row.y+82&&box.w===row.w-32;});
}
const sceneServices=[{domain:'scene',services:{turn_on:{}}},{domain:'script',services:{turn_on:{}}}];
test('HA scene sync independently discovers scenes beyond 512 devices and refreshes renamed/removed presets',()=>{
  const sceneStates=[{entity_id:'scene.evening',state:'unknown',attributes:{friendly_name:'晚间回家'}},
    {entity_id:'script.sleep',state:'off',attributes:{friendly_name:'一键睡眠',last_triggered:'2026-10-15T12:00:00Z'}}];
  const options={sceneSync:true,sceneStates,nativeActions:true,services:sceneServices,
    states:Array.from({length:600},(_,i)=>({entity_id:'sensor.room_'+i,state:'0',attributes:{}}))};
  const r=boot(1024,536,options);r.context.ESPHomeHA.refresh();for(let i=0;i<45;i++)r.tick();
  assert.equal(r.context.ESPHomeHA.snapshot().entities.length,512);assert(!r.requests.includes('scenes'));
  chooseSection(r,162);let snapshot=r.context.ESPHomeHA.snapshot();
  assert.equal(snapshot.sceneSyncReady,true);assert.equal(snapshot.sceneTotal,2);
  assert.deepEqual([...snapshot.layoutPages.flat().map(row=>row.entity_id)].sort(),['scene.evening','script.sleep']);
  assert.equal(snapshot.entities.length,512,'independent scenes never replace device state');
  assert([...r.textValues.values()].includes('尚未触发'));assert(sceneExecuteButton(r,'scene.evening'));
  const count=snapshot.activityLog.length;r.context.ESPHomeHA.refresh();for(let i=0;i<45;i++)r.tick();
  assert.equal(r.context.ESPHomeHA.snapshot().activityLog.length,count,'unchanged scene sync does not add logs');
  sceneStates[0].attributes.friendly_name='回家';sceneStates.splice(1,1);
  r.context.ESPHomeHA.refresh();for(let i=0;i<45;i++)r.tick();snapshot=r.context.ESPHomeHA.snapshot();
  assert.equal(snapshot.scenes[0].name,'回家');assert.equal(snapshot.sceneTotal,1);
  assert(!snapshot.layoutPages.flat().some(row=>row.entity_id==='script.sleep'));
});
test('HA scenes remain discoverable with favorite-only device sync and bounded to 128 entries',()=>{
  const options={sceneSync:true,acceptControls:true,savedScope:'favorites',favorites:['sensor.room'],
    states:[{entity_id:'sensor.room',state:'24',attributes:{}}],
    sceneStates:Array.from({length:140},(_,i)=>({entity_id:'scene.preset_'+i,state:'unknown',attributes:{friendly_name:'场景 '+i}}))};
  const r=boot(1024,536,options);r.context.ESPHomeHA.refresh();for(let i=0;i<45;i++)r.tick();chooseSection(r,162);
  const snapshot=r.context.ESPHomeHA.snapshot();assert.equal(snapshot.scope,'favorites');
  assert.equal(snapshot.entities.length,1);assert.equal(snapshot.scenes.length,128);assert.equal(snapshot.sceneTotal,140);
});
test('HA scene/script execution targets one preset, coalesces repeat taps and acknowledges acceptance without on-state verification',()=>{
  for(const domain of ['scene','script']){
    const entity=domain+'.preset',options={sceneSync:true,nativeActions:true,acceptControls:true,services:sceneServices,
      sceneStates:[{entity_id:entity,state:domain==='scene'?'unknown':'off',attributes:{friendly_name:'一键联动'}}]};
    const r=boot(1024,536,options);r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();chooseSection(r,162);
    const button=sceneExecuteButton(r,entity);assert(button);options.holdRequests=true;button.callback();button.callback();
    assert.equal(r.controls.length,1);assert.equal(r.controls[0].entity,entity);assert.equal(r.controls[0].action,'turn_on');
    assert.deepEqual(r.controls[0].body,{entity_id:entity});assert.equal(r.context.ESPHomeHA.snapshot().pending.kind,'service');
    options.holdRequests=false;for(let i=0;i<45;i++)r.tick();const snapshot=r.context.ESPHomeHA.snapshot();
    assert.equal(r.controls.length,1);assert.equal(snapshot.phase,'ready');assert.equal(snapshot.controlQueue.length,0);
    assert(snapshot.activityLog.some(row=>row.name==='HA 已接受场景'));
    assert(!snapshot.activityLog.some(row=>row.name==='操作已确认'),'HTTP success does not claim all grouped devices reached their target');
    assert.equal(snapshot.scenes[0].state,domain==='scene'?'unknown':'off','a preset is never forced into an on state');
    assert(!snapshot.functionControls.some(spec=>spec.service==='turn_off'));
  }
});
test('scene sync failure retains stale rows, recovers and clears the independent catalog on a home change',()=>{
  const options={sceneSync:true,sceneStates:[{entity_id:'scene.old_home',state:'unknown',attributes:{friendly_name:'旧家庭'}}]};
  const r=boot(1024,536,options);r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();chooseSection(r,162);
  options.sceneStatus=500;r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
  let snapshot=r.context.ESPHomeHA.snapshot();assert(snapshot.connected);assert(snapshot.sceneSyncError);assert(snapshot.scenes[0].stale);
  options.sceneStatus=200;options.sceneStates=[];r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
  snapshot=r.context.ESPHomeHA.snapshot();assert.equal(snapshot.sceneTotal,0);assert.equal(snapshot.scenes.length,0);assert(!snapshot.sceneSyncError);
  r.remoteConfigure('http://other-home.local:8123');r.tick();snapshot=r.context.ESPHomeHA.snapshot();assert(!snapshot.sceneSyncReady);assert.equal(snapshot.scenes.length,0);
});
test('scene action is absent without HA control grants or the advertised turn_on service',()=>{
  for(const controlAllowed of [false,true]){
    const options={sceneSync:true,nativeActions:true,canControl:controlAllowed,services:controlAllowed?[]:sceneServices,
      sceneStates:[{entity_id:'scene.preset',state:'unknown',attributes:{}}]};
    const r=boot(1024,536,options);r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();chooseSection(r,162);
    assert(!sceneExecuteButton(r,'scene.preset'));assert.equal(r.controls.length,0);
  }
});

test('a failed scene submission is never replayed automatically',()=>{
  const options={sceneSync:true,nativeActions:true,acceptControls:true,controlStatus:500,services:sceneServices,
    sceneStates:[{entity_id:'scene.preset',state:'unknown',attributes:{friendly_name:'回家场景'}}]};
  const r=boot(1024,536,options);r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();chooseSection(r,162);
  sceneExecuteButton(r,'scene.preset').callback();for(let i=0;i<100;i++)r.tick();
  const snapshot=r.context.ESPHomeHA.snapshot();assert.equal(r.controls.length,1);assert.equal(snapshot.controlQueue.length,0);
  assert.equal(snapshot.nextRefresh,Infinity);assert(snapshot.activityLog.some(row=>row.name==='操作失败'));
});




function lampFixture(overrides={},serviceOptions={}){
  const state={entity_id:'light.bedside',state:'on',attributes:{friendly_name:'床头灯',brightness:200,
    supported_color_modes:['color_temp','rgb'],color_mode:'color_temp',rgb_color:[255,150,46],
    color_temp_kelvin:2700,min_color_temp_kelvin:2700,max_color_temp_kelvin:6500,
    supported_features:4,effect_list:['none','Day','Night','Relax'],effect:'Relax',...overrides}};
  const options={states:[state],nativeActions:true,acceptControls:true,...serviceOptions};
  const r=boot(1024,536,options);r.fixtureState=state;r.fixtureOptions=options;
  r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();deviceBody(r,state.entity_id).callback();for(let i=0;i<20;i++)r.tick();return r;
}
function lampClickLabel(r,text){
  const label=[...r.textValues].find(([id,t])=>!r.hidden.get(id)&&t===text);assert(label,text);
  const g=r.geometry.get(label[0]);lampClickAt(r,g.x+g.w/2,g.y+g.h/2);
}
function lampClickAt(r,x,y){
  const b=r.buttons.find(b=>{const g=r.geometry.get(b.id);return !r.hidden.get(b.id)&&x>=g.x&&x<=g.x+g.w&&y>=g.y&&y<=g.y+g.h;});assert(b,'native lamp hit target');b.callback();for(let i=0;i<10;i++)r.tick();
}

test('bedside controls fit the real 504px content height with auxiliary functions',()=>{
  const main={entity_id:'light.bedside',state:'on',attributes:{friendly_name:'床头灯',brightness:200,
    supported_color_modes:['color_temp','rgb'],color_mode:'rgb',rgb_color:[255,150,46],
    color_temp_kelvin:2700,min_color_temp_kelvin:2700,max_color_temp_kelvin:6500,
    supported_features:4,effect_list:['none','Day','Night','Relax'],effect:'Day'}};
  const aux={entity_id:'switch.ambient',state:'on',attributes:{friendly_name:'辅助功能'}};
  const r=boot(1024,504,{states:[main,aux],devices:[['bedside','床头灯','卧室',[main.entity_id,aux.entity_id]]],nativeActions:true,acceptControls:true});
  r.context.ESPHomeHA.refresh();for(let i=0;i<50;i++)r.tick();deviceBody(r,main.entity_id).callback();for(let i=0;i<30;i++)r.tick();
  const visibleButtons=()=>r.buttons.filter(b=>!r.hidden.get(b.id)).map(b=>({b,g:r.geometry.get(b.id)}));
  function assertNoOverlap(){
    const choices=visibleButtons().filter(({g})=>g.x>650&&g.y>150);
    for(let i=0;i<choices.length;i++)for(let j=i+1;j<choices.length;j++){
      const a=choices[i].g,b=choices[j].g;
      assert(!(a.x<b.x+b.w&&b.x<a.x+a.w&&a.y<b.y+b.h&&b.y<a.y+a.h),'bedside button overlap '+JSON.stringify([a,b]));
    }
    for(const {g} of choices)assert(g.y+g.h<=486,'button below detail panel');
  }
  assertNoOverlap();assert(r.context.ESPHomeHA.snapshot().colorWheel);
  const wheel=r.context.ESPHomeHA.snapshot().colorWheel;
  assert(wheel.y+wheel.h<420,'wheel leaves space for effects and auxiliary entry');
  assert([...r.textValues].some(([id,t])=>!r.hidden.get(id)&&t==='常用色温 · 颜色'));
  assert(![...r.textValues].some(([id,t])=>!r.hidden.get(id)&&t==='自选颜色'));
  lampClickLabel(r,'夜间');assert.equal(r.controls.at(-1).body.effect,'Night');
  lampClickLabel(r,'其他设备功能');assert(visibleButtons().length>0);assertNoOverlap();
});
test('lamp detail has real color presets, an effect selector, and bounded native controls',()=>{
  const r=lampFixture();const visible=()=>[...r.textValues].filter(([id])=>!r.hidden.get(id)).map(([,v])=>v);
  for(const t of ['亮度','色温','常用色温 · 颜色','颜色选择','灯光效果'])assert(visible().includes(t),t);
  const face=r.nativeWrites.filter(w=>w.method==='color'&&w.value===0x7facff).at(-1);assert(face);
  const g=r.geometry.get(face.id);lampClickAt(r,g.x+g.w/2,g.y+g.h/2);
  assert.deepEqual(r.controls[0].body,{entity_id:'light.bedside',rgb_color:[127,172,255]});assert.equal(r.controls.length,1);
  assert.equal(r.context.ESPHomeHA.snapshot().phase,'ready');assert.equal(r.buttons.length,64);
  lampClickLabel(r,'夜间');assert.equal(r.context.ESPHomeHA.snapshot().optionPicker,null);assert.deepEqual(r.controls[1].body,{entity_id:'light.bedside',effect:'Night'});
  for(const [id,g] of r.geometry)if(!r.hidden.get(id)){assert(g.x>=0&&g.y>=0&&g.x+g.w<=1024&&g.y+g.h<=536,'lamp bounds '+JSON.stringify(g));}
});
test('native color disc previews locally, cancels safely and applies only on release',()=>{
  const r=lampFixture(),wheel=r.context.ESPHomeHA.snapshot().colorWheel;assert(wheel);
  const x=wheel.x+wheel.w/2,y=wheel.y+2;
  r.touch('down',x/1024,y/536);for(let i=0;i<25;i++)r.touch('move',x/1024,y/536);
  assert.equal(r.controls.length,0);r.swipe('left');r.touch('cancel',x/1024,y/536);assert.equal(r.controls.length,0);
  r.touch('down',x/1024,y/536);r.touch('move',x/1024,y/536);r.touch('up',x/1024,y/536);
  assert.equal(r.controls.length,1);assert.equal(r.controls[0].body.rgb_color[0],255);
  assert(r.controls[0].body.rgb_color[1]<8&&r.controls[0].body.rgb_color[2]<8);
  assert.equal(r.context.ESPHomeHA.snapshot().phase,'ready');
  const before=r.controls.length;r.touch('down',(wheel.x-4)/1024,(wheel.y-4)/536);r.touch('up',(wheel.x+10)/1024,(wheel.y+10)/536);assert.equal(r.controls.length,before,'outside circle is not a color gesture');
});
test('color disc drag performs bounded native writes and never touches the HA transport before release',()=>{
  const r=lampFixture(),wheel=r.context.ESPHomeHA.snapshot().colorWheel,x=wheel.x+wheel.w/2,y=wheel.y+2;
  r.nativeWrites.length=0;r.touch('down',x/1024,y/536);
  for(let i=0;i<100;i++){r.touch('move',x/1024,y/536);r.tick();}
  assert.equal(r.controls.length,0);assert(r.nativeWrites.length<12,'only marker, preview chip and hex label change');
  r.context.ESPHomeHA.disconnect();r.touch('up',x/1024,y/536);assert.equal(r.controls.length,0,'stale color gesture cannot control disconnected home');
});
test('background refresh preserves a live color preview until cancellation restores the latest server color',()=>{
  const r=lampFixture(),wheel=r.context.ESPHomeHA.snapshot().colorWheel,x=wheel.x+wheel.w/2,y=wheel.y+2;
  const visibleHex=()=>[...r.textValues].find(([id,value])=>!r.hidden.get(id)&&/^#[0-9A-F]{6}$/.test(value))?.[1];
  r.touch('down',x/1024,y/536);const preview=visibleHex();assert(preview);
  const previewChip=r.nativeWrites.filter(write=>write.method==='color'&&write.value===parseInt(preview.slice(1),16)).at(-1);assert(previewChip);
  r.fixtureState.attributes.rgb_color=[0,0,255];r.fixtureState.attributes.color_mode='rgb';
  r.context.ESPHomeHA.refresh();for(let i=0;i<50;i++)r.tick();
  assert.equal(visibleHex(),preview,'background updates cannot pull the marker away from the finger');assert.equal(r.controls.length,0);
  r.touch('cancel',x/1024,y/536);for(let i=0;i<30;i++)r.tick();assert(r.nativeWrites.some(write=>write.method==='color'&&write.id===previewChip.id&&write.value===0x0000ff));assert.equal(r.controls.length,0);
});

test('a color gesture cannot survive settings navigation or loss of color capability',()=>{
  for(const mode of ['settings','capability']){
    const r=lampFixture(),wheel=r.context.ESPHomeHA.snapshot().colorWheel,x=wheel.x+wheel.w/2,y=wheel.y+2;
    r.touch('down',x/1024,y/536);
    if(mode==='settings')r.context.ESPHomeHA.settings();
    else{Object.assign(r.fixtureState.attributes,{supported_color_modes:['brightness'],rgb_color:null,color_mode:'brightness'});r.context.ESPHomeHA.refresh();}
    for(let i=0;i<50;i++)r.tick();r.touch('up',x/1024,y/536);assert.equal(r.controls.length,0,mode);
  }
});

test('lamp detail drag updates a fixed value and thumb locally then sends one release request',()=>{
  const r=lampFixture(),rail=r.context.ESPHomeHA.snapshot().parameterControls.find(row=>row.key==='brightness_pct');assert.equal(rail.h,52);
  const nx=(rail.x+rail.w*.5)/1024,ny=(rail.y+rail.h/2)/536;
  const warm=r.nativeWrites.find(w=>w.method==='color'&&w.value===0xffa64e);assert(warm,'orange fill matches reference');
  r.nativeWrites.length=0;r.touch('down',nx,ny);for(let i=0;i<24;i++)r.touch('move',nx,ny);
  assert.equal(r.controls.length,0);assert(r.nativeWrites.some(w=>w.method==='text'&&w.value==='50%'));
  assert(r.nativeWrites.length<25,'drag writes only changed fill, thumb and value');
  r.touch('up',nx,ny);assert.equal(r.controls.length,1);assert.equal(r.controls[0].body.brightness_pct,50);
});
test('white-only lamps expose actual kelvin presets while color/effect controls remain capability gated',()=>{
  const r=lampFixture({supported_color_modes:['color_temp'],rgb_color:null,supported_features:0,effect_list:[]});
  const visible=[...r.textValues].filter(([id])=>!r.hidden.get(id)).map(([,v])=>v);
  assert(visible.includes('常用色温'));assert.equal(r.context.ESPHomeHA.snapshot().colorWheel,null);assert(!visible.includes('灯光效果'));
  const face=r.nativeWrites.filter(w=>w.method==='color'&&w.value===0xfffdfa).at(-1);const g=r.geometry.get(face.id);lampClickAt(r,g.x+g.w/2,g.y+g.h/2);
  assert.deepEqual(r.controls[0].body,{entity_id:'light.bedside',color_temp_kelvin:6500});
});

function profileDetailFixture(id,overrides={},dark=false){
  const state=commonProfileSamples().find(row=>row.entity_id===id);assert(state,id);
  Object.assign(state.attributes,overrides);
  const r=boot(1024,536,{states:[state],services:commonServices(),nativeActions:true,acceptControls:true,dark});
  r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();deviceBody(r,id).callback();for(let i=0;i<25;i++)r.tick();return r;
}

test('device detail sliders follow approved preview geometry and repaint only their live value and handle',()=>{
  for(const dark of [false,true])for(const [entity,key] of [['fan.standard','percentage'],['cover.curtain','position'],['humidifier.room','humidity'],['media_player.speaker','volume_level']]){
    const r=profileDetailFixture(entity,{},dark),rail=r.context.ESPHomeHA.snapshot().parameterControls.find(row=>row.key===key);
    assert.equal(rail.h,52);assert.equal(rail.w,370);
    const x=(rail.x+rail.w*.5)/1024,y=(rail.y+rail.h/2)/536;
    r.nativeWrites.length=0;r.touch('down',x,y);
    for(let i=0;i<100;i++){r.touch('move',x,y);r.tick();}
    assert.equal(r.controls.length,0);assert(r.nativeWrites.length<15,entity+' repeated pointer writes '+r.nativeWrites.length);
    r.touch('up',x,y);assert.equal(r.controls.length,1);assert.equal(r.buttons.length,64);
    for(const [id,g] of r.geometry)if(!r.hidden.get(id))assert(g.x>=0&&g.y>=0&&g.x+g.w<=1024&&g.y+g.h<=536,entity+' bounds '+JSON.stringify(g));
  }
});

test('cover and speaker actions share a row and retain their exact HA services',()=>{
  for(const [entity,labels,actions] of [
    ['cover.curtain',['打开','停止','关闭'],['open_cover','stop_cover','close_cover']],
    ['media_player.speaker',['上一首','播放','暂停','下一首'],['media_previous_track','media_play','media_pause','media_next_track']]]){
    const r=profileDetailFixture(entity);
    const positions=labels.map(text=>{const row=[...r.textValues].find(([id,t])=>!r.hidden.get(id)&&t===text);assert(row,text);return r.geometry.get(row[0]);});
    assert(positions.every(g=>g.y+g.h/2===positions[0].y+positions[0].h/2),entity+' '+JSON.stringify(positions));
    labels.forEach(label=>lampClickLabel(r,label));
    assert.deepEqual(r.controls.map(row=>row.action),actions);
    assert(r.controls.every(row=>row.body.entity_id===entity));
  }
});

test('climate detail uses bounded steppers while mode buttons preserve raw HA values',()=>{
  const r=profileDetailFixture('climate.room');lampClickLabel(r,'+');
  assert.deepEqual(r.controls[0].body,{entity_id:'climate.room',temperature:24.5});
  lampClickLabel(r,'制热');assert.deepEqual(r.controls[1].body,{entity_id:'climate.room',hvac_mode:'heat'});
  const range=r.context.ESPHomeHA.snapshot().parameterControls.find(row=>row.key==='temperature');
  const x=(range.x+range.w/2)/1024,y=(range.y+range.h/2)/536;
  r.touch('down',x,y);r.touch('cancel',x,y);assert.equal(r.controls.length,2);
  lampClickLabel(r,'低');assert.deepEqual(r.controls[2].body,{entity_id:'climate.room',fan_mode:'low'});
});

test('secondary modes and actual readings appear without adding missing measurements',()=>{
  const r=profileDetailFixture('humidifier.room');
  const visible=()=>[...r.textValues].filter(([id])=>!r.hidden.get(id));
  const range=r.context.ESPHomeHA.snapshot().parameterControls.find(row=>row.key==='humidity');
  const mode=visible().find(([,t])=>t==='加湿模式'),reading=visible().find(([,t])=>t==='当前湿度');
  assert(r.geometry.get(mode[0]).x>range.x+range.w);assert(reading);
  assert(visible().some(([,t])=>t==='48'));assert(r.geometry.get(reading[0]).x<r.geometry.get(mode[0]).x);lampClickLabel(r,'睡眠');
  assert.deepEqual(r.controls[0].body,{entity_id:'humidifier.room',mode:'sleep'});
  const absent=profileDetailFixture('humidifier.room',{current_humidity:null});
  assert(![...absent.textValues].some(([id,t])=>!absent.hidden.get(id)&&t==='当前湿度'));
});

test('long profile mode lists remain paged and send the original option after switching pages',()=>{
  const modes=Array.from({length:9},(_,i)=>'mode-'+i),r=profileDetailFixture('fan.standard',{preset_modes:modes,preset_mode:modes[0]});
  lampClickLabel(r,'mode-0  >');clickIndicator(r,1,'optionPageIndicator');lampClickLabel(r,'mode-6');
  assert.deepEqual(r.controls[0].body,{entity_id:'fan.standard',preset_mode:'mode-6'});
  assert.equal(r.buttons.length,64);
});

test('purifier reads real MIoT PM2.5 and filter values, including zero',()=>{
  const r=profileDetailFixture('fan.purifier',{pm25:null,miot_properties:{'environment.pm2.5-density':0,'air-purifier.filter-life-level':83}});
  const visible=[...r.textValues].filter(([id])=>!r.hidden.get(id));
  const pm=visible.find(([,t])=>t==='PM2.5'),filter=visible.find(([,t])=>t==='滤芯余量');assert(pm);assert(filter);
  assert(visible.some(([,t])=>t==='0'));assert(visible.some(([,t])=>t==='ug/m3'));assert(visible.some(([,t])=>t==='83'));
  assert(r.geometry.get(pm[0]).x<r.geometry.get(filter[0]).x);assert.equal(r.controls.length,0);
});

test('switching device details reuses bounded widgets and hides every previous detail decoration',()=>{
  const ids=['light.bedside','fan.standard','climate.room','cover.curtain','humidifier.room','media_player.speaker'];
  const states=commonProfileSamples().filter(row=>ids.includes(row.entity_id));
  const r=boot(1024,536,{states,services:commonServices(),nativeActions:true});r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
  let allocated=0;
  for(let pass=0;pass<3;pass++){
    for(const entity of ids){
      deviceBody(r,entity).callback();for(let i=0;i<25;i++)r.tick();
      const back=r.buttons.find(b=>{const g=r.geometry.get(b.id);return !r.hidden.get(b.id)&&g.x===176&&g.w===32&&g.h===30;});assert(back);back.callback();for(let i=0;i<25;i++)r.tick();
      assert(![...r.textValues].some(([id,t])=>!r.hidden.get(id)&&['播放控制','窗帘控制','常用色温','目标湿度','目标温度'].includes(t)));
      assert.equal(r.buttons.length,64);
    }
    if(pass===0)allocated=r.geometry.size;else assert.equal(r.geometry.size,allocated,'no new native widgets after visiting each profile once');
  }
  assert.equal(r.controls.length,0);
});

test('firmware layout dimensions and field order come from the unchanged approved HTML',()=>{
  const html=fs.readFileSync(path.join(__dirname,'../../homeassistant/preview/unified-device-cards.html'),'utf8');
  const design=JSON.parse(source.match(/const detailDesign = (.*);/)[1]);
  const css=(selector,property)=>{const escaped=('#ha-unified59-devices '+selector).replace(/[.*+?^${}()|[\]\\]/g,'\\$&');const rule=html.match(new RegExp(escaped+'\\{([^}]+)\\}'))[1];return Number(rule.match(new RegExp('(?:^|;)'+property+':([\\d.]+)'))[1]);};
  assert.equal(design.primaryMax,css('.dc-primary .dc-control','max-width'));
  assert.equal(design.rangeHeight,css('.dc-range','height'));
  assert.equal(design.minHeight,css('.dc-device-card','min-height'));
  assert.equal(design.buttonHeight,css('.dc-buttons button','min-height'));
  assert.deepEqual(design.layouts.humidifier.left.map(f=>f[1]),['目标湿度','当前湿度']);
  assert.deepEqual(design.layouts.heater.left.map(f=>f[1]),['目标水温','当前水温']);
  assert.deepEqual(design.layouts.fan.right.map(f=>f[0]),['swing']);
});

test('boolean controls use the preview switch and cover position presets reuse the existing service',()=>{
  const fan=profileDetailFixture('fan.standard');lampClickLabel(fan,'摆风');
  assert.deepEqual(fan.controls[0].body,{entity_id:'fan.standard',oscillating:true});
  assert([...fan.geometry].some(([id,g])=>!fan.hidden.get(id)&&g.w===46&&g.h===28));
  assert(![...fan.textValues].some(([id,t])=>!fan.hidden.get(id)&&['开启','关闭'].includes(t)),'a boolean is not a pair of option buttons');
  const cover=profileDetailFixture('cover.curtain');
  for(const [caption,value] of [['全开',100],['半开',50],['全关',0]]){
    lampClickLabel(cover,caption);assert.deepEqual(cover.controls.at(-1).body,{entity_id:'cover.curtain',position:value});
  }
});

test('the preview slider uses one clipped gradient track and keeps its value right aligned at both endpoints',()=>{
  const r=profileDetailFixture('fan.standard'),rail=r.context.ESPHomeHA.snapshot().parameterControls.find(s=>s.key==='percentage');
  r.nativeWrites.length=0;const y=(rail.y+rail.h/2)/536;
  for(const p of [0,100]){const x=(rail.x+rail.w*p/100)/1024;r.touch('down',x,y);r.touch('move',x,y);r.touch('cancel',x,y);}
  assert(r.nativeWrites.some(w=>w.method==='style'&&w.style.gradientMainStop===0&&w.style.gradientEndStop===0));
  assert(r.nativeWrites.some(w=>w.method==='style'&&w.style.gradientMainStop===255&&w.style.gradientEndStop===255));
  assert([...r.styles.values()].some(style=>style.rightAlign===1));assert.equal(r.controls.length,0);
});

test('lamp effects switch directly with one tap and keep their exact case-sensitive HA values',()=>{
  const r=lampFixture();
  for(const [label,raw] of [['夜间','Night'],['日间','Day'],['放松','Relax'],['无','none']]){
    const before=r.controls.length;lampClickLabel(r,label);
    assert.equal(r.controls.length,before+1);assert.equal(r.context.ESPHomeHA.snapshot().optionPicker,null);
    assert.deepEqual(r.controls.at(-1).body,{entity_id:'light.bedside',effect:raw});
  }
  const labels=['无','日间','夜间','放松'].map(text=>[...r.textValues].find(([id,t])=>!r.hidden.get(id)&&t===text));
  assert(labels.every(Boolean));const positions=labels.map(([id])=>r.geometry.get(id));
  assert(positions.every(g=>Math.abs(g.y+g.h/2-positions[0].y-positions[0].h/2)<1));
  assert(positions.every(g=>g.y+g.h<=536));assert.equal(r.buttons.length,64);
});

test('rapid effect changes keep the most recent choice while the HA request is pending',()=>{
  const r=lampFixture(),transport=r.fixtureOptions;
  transport.holdRequests=true;lampClickLabel(r,'夜间');lampClickLabel(r,'日间');lampClickLabel(r,'放松');
  assert.equal(r.controls.length,1);assert.equal(r.context.ESPHomeHA.snapshot().controlQueue.length,1);
  assert.equal(r.context.ESPHomeHA.snapshot().optionPicker,null);
  transport.holdRequests=false;for(let i=0;i<80;i++)r.tick();
  assert.deepEqual(r.controls.map(row=>row.body.effect),['Night','Relax']);
  assert.equal(r.context.ESPHomeHA.snapshot().phase,'ready');
});

test('long effect lists still choose an exact option and reflow without leaking or overlapping buttons',()=>{
  const r=lampFixture(),state=r.fixtureState,before=r.geometry.size;
  state.attributes.effect_list=Array.from({length:9},(_,i)=>'effect-'+i);state.attributes.effect='effect-0';
  r.context.ESPHomeHA.refresh();for(let i=0;i<50;i++)r.tick();
  lampClickLabel(r,'灯光效果');assert(r.context.ESPHomeHA.snapshot().optionPicker);
  clickIndicator(r,1,'optionPageIndicator');lampClickLabel(r,'effect-6');
  assert.deepEqual(r.controls.at(-1).body,{entity_id:'light.bedside',effect:'effect-6'});
  state.attributes.effect_list=['none','Day','Night','Relax'];state.attributes.effect='Night';
  r.context.ESPHomeHA.refresh();for(let i=0;i<50;i++)r.tick();lampClickLabel(r,'日间');
  assert.deepEqual(r.controls.at(-1).body,{entity_id:'light.bedside',effect:'Day'});
  const wheel=r.context.ESPHomeHA.snapshot().colorWheel;
  assert([...r.geometry].some(([id,g])=>!r.hidden.get(id)&&g.x===wheel.x&&g.y===wheel.y&&g.w===Math.round(wheel.w)&&g.h===Math.round(wheel.h)));
  assert.equal(r.buttons.length,64);assert(r.geometry.size>before,'long-list picker is allocated once');
});


function dragLightPercent(r,entity,percent,release='up'){
  const snapshot=r.context.ESPHomeHA.snapshot();
  const rail=snapshot.parameterControls.find(row=>row.entity===entity&&row.key==='brightness_pct')||snapshot.brightnessControls.find(row=>row.entity===entity);
  assert(rail,entity+' has a usable brightness slider');
  if(rail.min!==undefined){assert.equal(rail.min,0);assert.equal(rail.max,100);}
  const x=(rail.x+rail.w*percent/100)/1024,y=(rail.y+rail.h/2)/536,before=r.controls.length;
  r.touch('down',x,y);for(let i=0;i<6;i++)r.touch('move',x,y);
  assert.equal(r.controls.length,before,'no commands during brightness preview');
  assert.equal(r.context.ESPHomeHA.snapshot().brightnessInput.percent,percent);
  r.touch(release,x,y);
}

test('all dimmable light profiles use exact 0/1/50/100 percent on small and detail cards with both service bridges',()=>{
  for(const nativeActions of [false,true])for(const detail of [false,true])for(const [type,modes] of [
    ['light',['brightness']],['light',['color_temp']],['light',['rgb']],['mirror',['color_temp']]]){
    const entity='light.range',state={entity_id:entity,state:'on',attributes:{friendly_name:'Range',brightness:200,
      device_type:type,supported_color_modes:modes,color_temp_kelvin:2700,min_color_temp_kelvin:2700,max_color_temp_kelvin:6500}};
    const options={states:[state],nativeActions,acceptControls:true};
    const r=boot(1024,536,options);r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
    if(detail){deviceBody(r,entity).callback();for(let i=0;i<20;i++)r.tick();}
    for(const percent of [0,1,50,100]){
      dragLightPercent(r,entity,percent);
      const command=r.controls.at(-1);assert(command,`${type} ${detail} ${percent}`);
      if(nativeActions)assert.deepEqual(command,{entity,action:percent?'turn_on':'turn_off',body:percent?{entity_id:entity,brightness_pct:percent}:{entity_id:entity}});
      else assert.deepEqual(command,{entity,on:percent>0,brightness:percent||-1});
      for(let i=0;i<40;i++)r.tick();
      assert.equal(r.context.ESPHomeHA.snapshot().phase,'ready');
      assert.equal(state.state,percent?'on':'off');
      assert.equal(r.controls.length,[0,1,50,100].indexOf(percent)+1,'one request per release');
    }
  }
});

test('off lamps show zero even when HA retains brightness or omits it and can be started at one percent',()=>{
  for(const brightness of [undefined,200])for(const detail of [false,true]){
    const entity='light.off',state={entity_id:entity,state:'off',attributes:{friendly_name:'Off',brightness,supported_color_modes:['brightness']}};
    const r=boot(1024,536,{states:[state],nativeActions:true,acceptControls:true});r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
    assert.equal(r.context.ESPHomeHA.snapshot().brightnessControls[0].value,0);
    if(detail){deviceBody(r,entity).callback();for(let i=0;i<20;i++)r.tick();assert([...r.textValues].some(([id,value])=>!r.hidden.get(id)&&value==='0%'));}
    dragLightPercent(r,entity,1);assert.equal(r.controls[0].body.brightness_pct,1);for(let i=0;i<40;i++)r.tick();
    assert.equal(state.state,'on');assert.equal(r.context.ESPHomeHA.snapshot().phase,'ready');
  }
});

test('zero and positive brightness intents coalesce together without replaying an obsolete power state',()=>{
  for(const last of [0,1,100]){
    const options={acceptControls:true,nativeActions:true},r=boot(1024,536,options);
    r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();options.holdRequests=true;
    dragLightPercent(r,'light.desk',50);
    for(const percent of [0,1,100,last])dragLightPercent(r,'light.spotlights',percent);
    assert.equal(r.controls.length,1);const queue=r.context.ESPHomeHA.snapshot().controlQueue;
    assert.equal(queue.length,1);assert.equal(queue[0].brightness,last);assert.equal(queue[0].service,last?'turn_on':'turn_off');
    options.holdRequests=false;for(let i=0;i<70;i++)r.tick();
    assert.equal(r.controls.length,2);assert.deepEqual(r.controls[1],{entity:'light.spotlights',action:last?'turn_on':'turn_off',body:last?{entity_id:'light.spotlights',brightness_pct:last}:{entity_id:'light.spotlights'}});
    assert.equal(r.context.ESPHomeHA.snapshot().phase,'ready');
  }
});

test('canceling a zero brightness preview does not switch off a lamp and raw HA brightness one remains positive',()=>{
  const r=lampFixture({brightness:1});assert.equal(r.context.ESPHomeHA.snapshot().entities[0].brightness,1);
  assert([...r.textValues].some(([id,value])=>!r.hidden.get(id)&&value==='1%'));
  dragLightPercent(r,'light.bedside',0,'cancel');for(let i=0;i<15;i++)r.tick();assert.equal(r.controls.length,0);assert.equal(r.fixtureState.state,'on');
  assert([...r.textValues].some(([id,value])=>!r.hidden.get(id)&&value==='1%'));
});

test('interactive preview shares zero-off and positive-on brightness semantics for every light alias',()=>{
  const html=fs.readFileSync(path.join(__dirname,'../../homeassistant/preview/unified-device-cards.html'),'utf8');
  const script=html.match(/<script>([\s\S]*?)<\/script>/);assert(script);
  const functions=['lightBrightness','val','store','snap','changeValue'].map(name=>{
    const line=script[1].split('\n').find(line=>line.trimStart().startsWith('function '+name+'('));assert(line,name);return line;
  }).join('\n');
  // Execute the exact preview handlers with a local binding spy; no DOM or HA transport is involved.
  const context=vm.createContext({assert});
  vm.runInContext(`
    const devices={bedside:{primary:[{id:'brightness'}]},mirror:{primary:[{id:'brightness'}]},fan:{primary:[{id:'speed'}]}};
    const state={values:{}},powerOn=id=>state.values[id]?.power??true;
    let savePending=false,saveTimer=0;const paints=[];
    const markChanged=()=>{},syncPower=()=>{},flushPaint=()=>{},refresh=(id,c)=>paints.push(val(id,c)),notice=()=>{},fmt=()=>'',queuePaint=()=>{};
    ${functions}
    for(const device of ['bedside','mirror']){
      const c={id:'brightness',value:79,min:0,max:100,step:1};
      assert.equal(snap(c,0),0);assert.equal(snap(c,1),1);assert.equal(snap(c,50),50);assert.equal(snap(c,100),100);
      changeValue(device,c,0);assert.equal(val(device,c),0);assert.equal(powerOn(device),false);
      store(device,{id:'power'},true);assert.equal(val(device,c),79);
      for(const value of [1,50,100]){changeValue(device,c,value);assert.equal(val(device,c),value);assert.equal(powerOn(device),true);}
      changeValue(device,c,0);store(device,{id:'power'},true);assert.equal(val(device,c),100);
    }
    changeValue('fan',{id:'speed',value:40},0);assert.equal(state.values.fan.speed,0);assert.equal(state.values.fan.power,undefined);
  `,context);
  assert(html.includes("R('brightness','亮度',79)"),'preview brightness starts at zero');
});


test('grouped auxiliary lighting uses the same range and controls only its own light entity',()=>{
  const main={entity_id:'light.main',state:'on',attributes:{friendly_name:'主灯',brightness:128,supported_color_modes:['brightness']}},
    aux={entity_id:'light.aux',state:'on',attributes:{friendly_name:'辅助灯',brightness:200,supported_color_modes:['brightness']}};
  const r=boot(1024,536,{states:[main,aux],devices:[['combo','组合灯','',[main.entity_id,aux.entity_id]]],nativeActions:true,acceptControls:true});
  r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();deviceBody(r,'light.main').callback();for(let i=0;i<20;i++)r.tick();
  for(const percent of [0,1,100]){dragLightPercent(r,'light.aux',percent);for(let i=0;i<40;i++)r.tick();}
  assert.deepEqual(r.controls.map(c=>[c.entity,c.action,c.body.brightness_pct]),[['light.aux','turn_off',undefined],['light.aux','turn_on',1],['light.aux','turn_on',100]]);
  assert.equal(main.attributes.brightness,128);assert.equal(main.state,'on');
});


test('zero percent has no accent-colored gradient endpoint and leaving zero restores the fill',()=>{
  for(const dark of [false,true])for(const detail of [false,true]){
    const r=boot(1024,536,{dark,nativeActions:true,acceptControls:true});r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
    if(detail){deviceBody(r,'light.desk').callback();for(let i=0;i<20;i++)r.tick();}
    const snapshot=r.context.ESPHomeHA.snapshot(),rail=snapshot.parameterControls.find(row=>row.entity==='light.desk'&&row.key==='brightness_pct')||snapshot.brightnessControls.find(row=>row.entity==='light.desk');assert(rail);
    const track=[...r.geometry].find(([id,g])=>!r.hidden.get(id)&&g.x===rail.x-15&&g.w===rail.w+30&&g.y===rail.y&&g.h===rail.h&&r.styles.get(id)?.gradientHorizontal===1);assert(track);
    const id=track[0],background=r.styles.get(id).gradientEnd;
    const color=()=>r.nativeWrites.findLast(w=>w.method==='color'&&w.id===id)?.value;
    const accent=color();assert.notEqual(accent,background);
    const y=(rail.y+rail.h/2)/536,x=p=>(rail.x+rail.w*p/100)/1024;
    r.touch('down',x(0),y);
    assert.equal(color(),background);assert.equal(r.styles.get(id).gradientEnd,background,'both stops use gray at zero');
    for(const percent of [1,50,100,0,1]){
      r.touch('move',x(percent),y);
      assert.equal(color(),percent?accent:background);
      assert.equal(r.styles.get(id).gradientEnd,percent===100?accent:background,'full brightness also has no gray endpoint');
    }
    assert.equal(r.controls.length,0);r.touch('up',x(0),y);for(let i=0;i<40;i++)r.tick();
    assert.equal(color(),background);assert.equal(r.styles.get(id).gradientEnd,background,'verified off render remains gray');
    dragLightPercent(r,'light.desk',50);for(let i=0;i<40;i++)r.tick();assert.equal(color(),accent);assert.equal(r.styles.get(id).gradientEnd,background);
  }
});


test('camera cards retain a readable preview placeholder above its background and align real reading baselines',()=>{
  for(const dark of [false,true])for(const type of ['camera','video-doorbell']){
    const entity='camera.entry',state={entity_id:entity,state:'idle',attributes:{friendly_name:'监控摄像机',device_type:type,device_id:'entry',miot_properties:{'battery.battery-level':86}}};
    const r=boot(1024,536,{states:[state],dark,frontOrder:true,measureMetrics:(text,size)=>({width:String(text).length*12,height:size===43?32:18,baseline:size===43?5:3})});
    r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
    const labels=text=>[...r.textValues].filter(([id,value])=>!r.hidden.get(id)&&value===text);
    assert(labels('暂无预览画面').length,'small camera preview text');
    assert(![...r.textValues].some(([id,value])=>!r.hidden.get(id)&&String(value).startsWith('设备状态 · 电量')),'no crowded status header');
    r.nativeWrites.length=0;deviceBody(r,entity).callback();for(let i=0;i<25;i++)r.tick();
    const notice=labels('暂无预览画面')[0];assert(notice);
    const bg=[...r.geometry].find(([id,g])=>!r.hidden.get(id)&&g.w===400&&g.h===225);assert(bg);
    const fronts=r.nativeWrites.filter(w=>w.method==='front').map(w=>w.id);
    assert(fronts.lastIndexOf(notice[0])>fronts.lastIndexOf(bg[0]),'notice paints above preview background');
    const battery=labels('86')[0],unit=labels('%')[0];assert(battery&&unit);
    const numberBox=r.geometry.get(battery[0]),unitBox=r.geometry.get(unit[0]);
    assert.equal(numberBox.y+32-5,unitBox.y+18-3,'battery and percent share their measured baseline');
    assert.equal(r.controls.length,0,'opening previews never controls a camera');
  }
});


test('HA software entities stay out of device boards while real sensors, helpers and grouped auxiliary controls remain accessible',()=>{
  const states=[
    {entity_id:'conversation.home_assistant',state:'unknown',attributes:{friendly_name:'Assistant'}},
    {entity_id:'zone.home',state:'0',attributes:{}},{entity_id:'person.me',state:'home',attributes:{}},
    {entity_id:'sun.sun',state:'above_horizon',attributes:{}},{entity_id:'todo.shopping',state:'0',attributes:{}},
    {entity_id:'tts.voice',state:'unknown',attributes:{}},{entity_id:'update.core',state:'off',attributes:{}},
    {entity_id:'sensor.backup_state',state:'idle',attributes:{device_class:'enum'}},
    {entity_id:'sensor.next_dawn',state:'2026-10-16T01:00:00Z',attributes:{device_class:'timestamp'}},
    {entity_id:'scene.evening',state:'unknown',attributes:{}},{entity_id:'script.sleep',state:'off',attributes:{}},
    {entity_id:'light.desk',state:'on',attributes:{brightness:128,supported_color_modes:['brightness']}},
    {entity_id:'update.desk_firmware',state:'on',attributes:{}},
    {entity_id:'sensor.room',state:'24',attributes:{device_class:'temperature',unit_of_measurement:'°C'}},
    {entity_id:'sensor.plant_seen',state:'2026-10-15T01:00:00Z',attributes:{device_class:'timestamp'}},
    {entity_id:'input_boolean.mode',state:'on',attributes:{}},
    {entity_id:'sensor.followed_time',state:'2026-10-15T01:00:00Z',attributes:{device_class:'timestamp'}}];
  const devices=[['tts','Voice','',['tts.voice'],'Google Translate TTS','Google'],
    ['backup','任意名字','',['sensor.backup_state'],'Home Assistant Backup','Home Assistant'],
    ['sun','任意名字','',['sensor.next_dawn']],[ 'lamp','台灯','',['light.desk','update.desk_firmware'],'yeelink.light.lamp28','Yeelink'],
    ['plant','植物检测仪','',['sensor.plant_seen'],'monitor','Acme']];
  const r=boot(1024,536,{states,devices,favorites:['sensor.followed_time']});r.context.ESPHomeHA.refresh();for(let i=0;i<45;i++)r.tick();
  const ids=r.context.ESPHomeHA.snapshot().deviceCards.map(card=>card.entity_id);
  assert.deepEqual([...ids].sort(),['input_boolean.mode','light.desk','sensor.followed_time','sensor.plant_seen','sensor.room']);
  assert.equal(r.context.ESPHomeHA.snapshot().entities.length,states.length,'filtering cards never discards entity data');
  const lamp=r.context.ESPHomeHA.snapshot().deviceCards.find(card=>card.entity_id==='light.desk');assert(lamp.members.includes('update.desk_firmware'));
  chooseSection(r,116);assert(r.context.ESPHomeHA.snapshot().layoutPages.flat().every(row=>ids.includes(row.entity_id)));
  chooseSection(r,162);assert.deepEqual([...r.context.ESPHomeHA.snapshot().layoutPages.flat().map(row=>row.entity_id)].sort(),['scene.evening','script.sleep']);
  assert.equal(r.controls.length,0);
});

function pinButton(r) {
  return r.buttons.find(b=>!r.hidden.get(b.id)&&r.geometry.get(b.id).x===864&&r.geometry.get(b.id).y===10&&r.geometry.get(b.id).w===144);
}
const desktopEndpoint='http://192.168.1.20:8123';
function desktopFixture(extra={}) {
  return {states:[['a','on'],['b','off'],['c','unavailable']].map(([id,state])=>({entity_id:'light.'+id,state,attributes:{friendly_name:'设备 '+id,supported_color_modes:['brightness']}})),
    devices:[['dev_a','设备 A','',['light.a']],['dev_b','设备 B','',['light.b']],['dev_c','设备 C','',['light.c']]],...extra};
}
test('desktop uses manual device pins before usage, deduplicates physical devices and bounds shortcuts to two',()=>{
  const r=boot(1024,536,desktopFixture({savedPins:JSON.stringify({server:desktopEndpoint,pins:['device.dev_b']}),
    savedUsage:JSON.stringify({server:desktopEndpoint,counts:{'device.dev_a':20,'device.dev_c':10}})}));
  r.context.ESPHomeHA.refresh();for(let i=0;i<45;i++)r.tick();
  assert.deepEqual(r.desktopSnapshots.at(-1).items.map(x=>[x.entity,x.pinned]),[['light.b',true],['light.a',false]]);
  const n=r.desktopSnapshots.length;for(let i=0;i<30;i++)r.tick();
  assert.equal(r.desktopSnapshots.length,n,'unchanged renders do not cross the native bridge');
  assert.equal(r.controls.length,0,'desktop publishing is read-only');
});
test('pin and unpin persist physical identities without changing the approved card geometry',()=>{
  const r=boot(1024,536,desktopFixture());r.context.ESPHomeHA.refresh();for(let i=0;i<45;i++)r.tick();
  r.context.ESPHomeHA.openDevice(desktopEndpoint,'light.b');for(let i=0;i<40;i++)r.tick();
  assert(pinButton(r));pinButton(r).callback();for(let i=0;i<30;i++)r.tick();
  assert.deepEqual(JSON.parse(r.saves.get('eh_desktop_pins')),{server:desktopEndpoint,pins:['device.dev_b']});
  assert.equal(r.desktopSnapshots.at(-1).items[0].entity,'light.b');assert.equal(r.desktopSnapshots.at(-1).items[0].pinned,true);
  const savedPins=r.saves.get('eh_desktop_pins');
  const again=boot(1024,536,desktopFixture({savedPins}));again.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)again.tick();
  assert.equal(again.desktopSnapshots.at(-1).items[0].entity,'light.b');assert.equal(again.desktopSnapshots.at(-1).items[0].pinned,true);
  pinButton(r).callback();for(let i=0;i<30;i++)r.tick();
  assert.deepEqual(JSON.parse(r.saves.get('eh_desktop_pins')).pins,[]);assert.equal(r.controls.length,0);
});
test('a third pin preserves both existing pins and unavailable devices remain identifiable',()=>{
  const r=boot(1024,536,desktopFixture({savedPins:JSON.stringify({server:desktopEndpoint,pins:['device.dev_a','device.dev_b']})}));
  r.context.ESPHomeHA.refresh();for(let i=0;i<45;i++)r.tick();
  r.context.ESPHomeHA.openDevice(desktopEndpoint,'light.c');for(let i=0;i<40;i++)r.tick();pinButton(r).callback();for(let i=0;i<25;i++)r.tick();
  assert.deepEqual(Array.from(r.context.ESPHomeHA.snapshot().desktopPins),['device.dev_a','device.dev_b']);
  assert.equal(r.context.ESPHomeHA.snapshot().message,'桌面已固定两台设备，请先取消一个固定');
  assert.equal(r.controls.length,0);
});
test('desktop deep links defer until sync, open the physical detail and reject stale endpoint navigation',()=>{
  const r=boot(1024,536,desktopFixture());
  assert.equal(r.context.ESPHomeHA.openDevice(desktopEndpoint,'light.b'),true);
  for(let i=0;i<55;i++)r.tick();
  assert.equal(r.context.ESPHomeHA.snapshot().view,'detail');assert.equal(r.context.ESPHomeHA.snapshot().selectedId,'light.b');
  assert.equal(r.context.ESPHomeHA.openDevice('http://192.168.1.21:8123','light.a'),false);
  assert.equal(r.context.ESPHomeHA.openDevice(desktopEndpoint,'invalid'),false);
  assert.equal(r.context.ESPHomeHA.snapshot().selectedId,'light.b');assert.equal(r.controls.length,0);
});
test('pins and usage from another HA endpoint or removed device cannot create ghost desktop cards',()=>{
  const r=boot(1024,536,desktopFixture({savedPins:JSON.stringify({server:'http://192.168.1.21:8123',pins:['device.dev_c']}),
    savedUsage:JSON.stringify({server:'http://192.168.1.21:8123',counts:{'device.dev_c':50}})}));
  r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();assert.equal(r.desktopSnapshots.at(-1).items[0].entity,'light.a');
  assert.equal(r.desktopSnapshots.at(-1).items.some(x=>x.pinned),false);
  r.context.ESPHomeHA.openDevice(desktopEndpoint,'light.removed');for(let i=0;i<20;i++)r.tick();
  assert.equal(r.context.ESPHomeHA.snapshot().message,'该设备已移除或未在当前同步范围内');assert.equal(r.controls.length,0);
});

test('confirmed removal releases a manual desktop pin without pruning during a partial sync',()=>{
  const savedPins=JSON.stringify({server:desktopEndpoint,pins:['device.removed','device.dev_b']});
  const r=boot(1024,536,desktopFixture({savedPins}));r.context.ESPHomeHA.refresh();for(let i=0;i<45;i++)r.tick();
  assert.deepEqual(Array.from(r.context.ESPHomeHA.snapshot().desktopPins),['device.dev_b']);
  assert.deepEqual(JSON.parse(r.saves.get('eh_desktop_pins')).pins,['device.dev_b']);
  const partial=boot(1024,536,desktopFixture({savedPins,savedScope:'favorites',favorites:['light.b']}));
  partial.context.ESPHomeHA.refresh();for(let i=0;i<45;i++)partial.tick();
  assert.deepEqual(Array.from(partial.context.ESPHomeHA.snapshot().desktopPins),['device.removed','device.dev_b']);
});

test('desktop state labels are bounded independently of unusually long HA sensor state strings',()=>{
  const r=boot(1024,536,{states:[{entity_id:'sensor.custom',state:'设备'.repeat(64),attributes:{friendly_name:'名称'.repeat(64)}}]});
  r.context.ESPHomeHA.refresh();for(let i=0;i<45;i++)r.tick();const row=r.desktopSnapshots.at(-1).items[0];
  assert(Buffer.byteLength(row.name)<128);assert(Buffer.byteLength(row.state)<80);assert.equal(r.controls.length,0);
});

test('desktop navigation accepts the exact endpoint already approved by the native HA service',()=>{
  const endpoint='http://203.0.113.20:8123',r=boot(1024,536,desktopFixture({url:endpoint}));
  assert.equal(r.context.ESPHomeHA.openDevice(endpoint,'light.b'),true);for(let i=0;i<55;i++)r.tick();
  assert.equal(r.context.ESPHomeHA.snapshot().selectedId,'light.b');assert.equal(r.context.ESPHomeHA.snapshot().detailOpen,true);
  assert.equal(r.context.ESPHomeHA.openDevice(endpoint+'/', 'light.a'),false);assert.equal(r.controls.length,0);
});


test('opening and returning from a card never adds an upward presentation or sends commands',()=>{
  const r=boot(1024,600),motions=[];
  r.context.ui.present=(ids,offset)=>{assert(ids.length>0&&ids.length<=128);ids.forEach(id=>assert(r.geometry.has(id)));motions.push(offset);};
  r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();motions.length=0;
  deviceBody(r,'light.desk').callback();for(let i=0;i<40;i++)r.tick();
  assert.equal(motions.length,0);assert.equal(r.controls.length,0);
  const back=r.buttons.find(b=>{const g=r.geometry.get(b.id);return !r.hidden.get(b.id)&&g.x>=164&&g.x<200&&g.y<45;});assert(back);back.callback();
  for(let i=0;i<40;i++)r.tick();assert.equal(motions.length,0);assert.equal(r.controls.length,0);
});
test('lamp motion smooths presets but keeps both wheel and slider drags direct',()=>{
  const r=lampFixture(),wheel=r.context.ESPHomeHA.snapshot().colorWheel;
  const preset=r.nativeWrites.filter(w=>w.method==='color'&&w.value===0x7facff).at(-1);assert(preset);
  const g=r.geometry.get(preset.id);r.nativeWrites.length=0;lampClickAt(r,g.x+g.w/2,g.y+g.h/2);
  assert(r.nativeWrites.some(w=>w.method==='position'&&w.smooth),'preset marker interpolates');
  r.nativeWrites.length=0;r.touch('down',(wheel.x+wheel.w/2)/1024,(wheel.y+2)/536);
  assert(r.nativeWrites.some(w=>w.method==='position'&&!w.smooth),'wheel remains attached to finger');
  r.touch('cancel',0,0);for(let i=0;i<20;i++)r.tick();
  const ranges=[];r.context.ui.range=(id,stop,accent,background,animate)=>ranges.push({id,stop,animate});
  const rail=r.context.ESPHomeHA.snapshot().parameterControls.find(row=>row.key==='brightness_pct');assert(rail);
  r.touch('down',(rail.x+rail.w*.2)/1024,(rail.y+rail.h/2)/536);
  assert(ranges.some(row=>!row.animate));r.touch('cancel',0,0);
  const commands=r.controls.length;r.context.ESPHomeHA.disconnect();for(let i=0;i<20;i++)r.tick();assert.equal(r.controls.length,commands);
});


test('sidebar cursor follows a drag, snaps once on release and rejects stale, canceled or outside releases',()=>{
  const r=boot(1024,600,{waterfall:true});r.context.ESPHomeHA.refresh();for(let i=0;i<50;i++)r.tick();
  const original=r.buttons.find(b=>{const g=r.geometry.get(b.id);return !r.hidden.get(b.id)&&g.x===12&&g.y===104;});assert(original);
  const writes=r.nativeWrites.length;
  r.touch('down',80/1024,126/600);r.touch('move',80/1024,230/600);
  let state=r.context.ESPHomeHA.snapshot();assert.equal(state.nav,'overview');assert.equal(state.sidebarCursor.y,208);assert(state.sidebarCursor.dragging);
  assert.equal(r.nativeWrites.slice(writes).filter(w=>w.method==='position'&&w.smooth).length,0);
  r.touch('up',80/1024,230/600);for(let i=0;i<40;i++)r.tick();
  state=r.context.ESPHomeHA.snapshot();assert.equal(state.nav,'scenes');assert.equal(state.sidebarCursor.y,208);assert(!state.sidebarCursor.dragging);
  original.callback();for(let i=0;i<10;i++)r.tick();assert.equal(r.context.ESPHomeHA.snapshot().nav,'scenes','native original click is suppressed');
  r.touch('down',80/1024,230/600);r.touch('move',80/1024,280/600);r.touch('cancel',0,0);for(let i=0;i<20;i++)r.tick();
  assert.equal(r.context.ESPHomeHA.snapshot().nav,'scenes');assert.equal(r.context.ESPHomeHA.snapshot().sidebarCursor.y,208);
  r.touch('down',80/1024,230/600);r.touch('move',80/1024,126/600);r.touch('up',250/1024,126/600);for(let i=0;i<20;i++)r.tick();assert.equal(r.context.ESPHomeHA.snapshot().nav,'scenes');
  r.touch('down',80/1024,230/600);r.touch('move',80/1024,126/600);r.context.ESPHomeHA.settings();r.touch('up',80/1024,126/600);for(let i=0;i<20;i++)r.tick();assert(r.context.ESPHomeHA.snapshot().settingsOpen);
  assert.equal(r.controls.length,0);
});

test('sidebar geometry matches native navigation across supported display sizes',()=>{
  const header=fs.readFileSync(path.join(__dirname,'../overlay/apps/system/desktop/glass_sidebar.h'),'utf8');
  const token=name=>Number(header.match(new RegExp('#define '+name+' (\\d+)'))[1]);
  assert.equal(token('GLASS_SIDEBAR_WIDTH'),164);
  for(const [width,height] of [[640,400],[800,450],[1024,504],[1024,600]]){
    const r=boot(width,height);r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
    const rows=r.buttons.filter(b=>!r.hidden.get(b.id)).map(b=>r.geometry.get(b.id)).filter(g=>g.x===12&&g.w===140&&g.h===44);
    assert.equal(rows.length,4);
    for(let i=0;i<rows.length;i++){
      assert.equal(rows[i].x,token('GLASS_SIDEBAR_PAD'));
      assert.equal(rows[i].w,token('GLASS_SIDEBAR_ITEM_WIDTH'));
      assert.equal(rows[i].h,token('GLASS_SIDEBAR_ITEM_HEIGHT'));
      assert(rows[i].x+rows[i].w<164&&rows[i].y+rows[i].h<height-36);
      if(i)assert.equal(rows[i].y-rows[i-1].y,44+token('GLASS_SIDEBAR_GAP'));
    }
  }
});

test('sidebar uses shared dimensions and updates its retained selection without floating',()=>{
  const r=boot(1024,600);r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
  const face=[...r.geometry].find(([id,g])=>!r.hidden.get(id)&&g.x===12&&g.w===140&&g.h===44&&!r.buttons.some(b=>b.id===id));assert(face);
  const clickRow=y=>{const b=r.buttons.find(b=>{const g=r.geometry.get(b.id);return !r.hidden.get(b.id)&&g.x===12&&g.y===104+(y-70)/46*52&&g.w===140;});assert(b);b.callback();for(let i=0;i<40;i++)r.tick();};
  r.nativeWrites.length=0;clickRow(208);
  assert(r.nativeWrites.some(w=>w.method==='position'&&w.id===face[0]&&w.y===260&&!w.smooth));
  clickRow(70);assert.equal(r.geometry.get(face[0]).y,104);assert.equal(r.controls.length,0);
});
test('HA pagination changes pages without floating the content',()=>{
  const states=Array.from({length:18},(_,i)=>({entity_id:'light.motion_'+i,state:'on',attributes:{friendly_name:'灯 '+i,brightness:120,supported_color_modes:['brightness']}}));
  const r=boot(1024,600,{states}),motions=[];r.context.ui.present=(ids,offset,horizontal)=>motions.push({ids,offset,horizontal});
  r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();motions.length=0;
  clickIndicator(r,1);for(let i=0;i<30;i++)r.tick();
  assert.equal(motions.length,0);
  clickIndicator(r,0);for(let i=0;i<30;i++)r.tick();
  assert.equal(motions.length,0);
  for(const m of motions.filter(m=>m.horizontal))for(const id of m.ids)assert(r.geometry.get(id).y<546,'footer remains still');
  assert.equal(r.controls.length,0);
});
test('mode faces fade after a choice and purifier meters interpolate real values with an empty zero endpoint',()=>{
  const lamp=lampFixture();lamp.nativeWrites.length=0;lampClickLabel(lamp,'夜间');
  assert(lamp.nativeWrites.some(w=>w.method==='color'&&w.smooth),'selected mode face fades');assert.equal(lamp.controls.length,1);
  const state=commonProfileSamples().find(row=>row.entity_id==='fan.purifier');
  state.attributes.miot_properties={'air-purifier.filter-life-level':83};
  const r=boot(1024,536,{states:[state],services:commonServices()});r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
  deviceBody(r,state.entity_id).callback();for(let i=0;i<30;i++)r.tick();
  const initial=[...r.geometry].find(([id,g])=>!r.hidden.get(id)&&g.h===8&&[...r.geometry].some(([other,box])=>other!==id&&!r.hidden.get(other)&&box.h===8&&box.x===g.x&&box.y===g.y&&box.w>g.w));assert(initial);
  const id=initial[0],width=initial[1].w;r.nativeWrites.length=0;
  state.attributes.miot_properties['air-purifier.filter-life-level']=40;r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
  assert(r.nativeWrites.some(w=>w.method==='size'&&w.id===id&&w.smooth&&w.w<width));
  state.attributes.miot_properties['air-purifier.filter-life-level']=0;r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
  assert.equal(r.hidden.get(id),true);assert(r.geometry.get(id).w>=1);assert.equal(r.controls.length,0);
});


function activePagerPill(r,kind='pageIndicator'){
  const state=r.context.ESPHomeHA.snapshot(),indicator=state[kind],current=kind==='optionPageIndicator'?state.optionPicker.page:kind==='detailPageIndicator'?indicator.current:state.roomPage;
  const active=indicator.slots.find(slot=>slot.page===current);assert(active);
  const found=[...r.geometry].find(([id,g])=>!r.hidden.get(id)&&g.w===20&&g.h===6&&g.x===active.x+2&&g.y===active.y+17);assert(found);return found[0];
}
test('Mijia reuses one constant-width sliding pill through left, middle and right pages in both themes',()=>{
  for(const dark of [false,true]){
    const states=Array.from({length:18},(_,i)=>({entity_id:'light.slide_'+i,state:'on',attributes:{brightness:120,supported_color_modes:['brightness']}}));
    const r=boot(1024,600,{states,dark});r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
    const id=activePagerPill(r),allocated=r.geometry.size;r.nativeWrites.length=0;
    for(const page of [1,2,1,0]){clickIndicator(r,page);assert.equal(activePagerPill(r),id);}
    assert(r.nativeWrites.filter(w=>w.method==='position'&&w.id===id&&w.smooth).length>=4);
    assert(!r.nativeWrites.some(w=>w.method==='size'&&w.id===id),'pill never shrinks or stretches');
    assert.equal(r.geometry.size,allocated);assert.equal(r.controls.length,0);
  }
});
test('option picker also preserves its sliding pill across repeated page renders without requesting device actions',()=>{
  const modes=Array.from({length:9},(_,i)=>'mode-'+i),r=profileDetailFixture('fan.standard',{preset_modes:modes,preset_mode:modes[0]});
  lampClickLabel(r,'mode-0  >');const id=activePagerPill(r,'optionPageIndicator');r.nativeWrites.length=0;
  for(const page of [1,2,1,0]){clickIndicator(r,page,'optionPageIndicator');assert.equal(activePagerPill(r,'optionPageIndicator'),id);}
  assert(r.nativeWrites.filter(w=>w.method==='position'&&w.id===id&&w.smooth).length>=4);assert.equal(r.controls.length,0);
});


function discoveryHeader(r,width,offset){
  const button=r.buttons.find(button=>{const box=r.geometry.get(button.id);return !r.hidden.get(button.id)&&box.x===width-offset&&box.y===10&&box.h===44;});assert(button);button.callback();for(let i=0;i<40;i++)r.tick();
}
function clickNativeCaption(r,caption){const button=r.buttons.find(button=>!r.hidden.get(button.id)&&r.textValues.get(button.id)===caption);assert(button,caption);button.callback();for(let i=0;i<50;i++)r.tick();}
test('device discovery combines room, member search and favorites without device requests',()=>{
  const states=Array.from({length:12},(_,i)=>({entity_id:'switch.find_'+i,state:'on',attributes:{friendly_name:'插座 '+i}}));
  const devices=states.map((state,i)=>['device_'+i,'插座 '+i,i%2?'卧室':'客厅',[state.entity_id]]);
  const r=boot(1024,600,{states,devices,waterfall:true,favorites:['switch.find_2']});r.context.ESPHomeHA.refresh();for(let i=0;i<90;i++)r.tick();
  discoveryHeader(r,1024,136);assert(r.context.ESPHomeHA.snapshot().discovery.rooms.includes('客厅'));clickNativeCaption(r,'客厅');
  assert.equal(r.context.ESPHomeHA.snapshot().discovery.count,6);
  discoveryHeader(r,1024,180);assert.equal(r.prompts.length,1);r.prompts[0].callback('FIND_2');for(let i=0;i<70;i++)r.tick();assert.equal(r.context.ESPHomeHA.snapshot().discovery.count,1);
  discoveryHeader(r,1024,136);clickNativeCaption(r,'仅看关注设备');assert(r.context.ESPHomeHA.snapshot().discovery.favoritesOnly);assert.equal(r.context.ESPHomeHA.snapshot().discovery.count,1);
  discoveryHeader(r,1024,136);clickNativeCaption(r,'清除筛选');assert.equal(r.context.ESPHomeHA.snapshot().discovery.count,12);assert.equal(r.controls.length,0);
  discoveryHeader(r,1024,180);const old=r.prompts.at(-1);r.remoteConfigure('http://192.168.1.21:8123');for(let i=0;i<80;i++)r.tick();old.callback('old-home');assert.equal(r.context.ESPHomeHA.snapshot().search,'');
});

test('room picker stays bounded and exclusive at minimum resolution and does not create devices per room',()=>{
  const states=Array.from({length:36},(_,i)=>({entity_id:'switch.room_'+i,state:'on',attributes:{friendly_name:'设备 '+i}}));
  const devices=states.map((state,i)=>['roomdev_'+i,state.attributes.friendly_name,'房间 '+i,[state.entity_id]]);
  const r=boot(640,400,{states,devices,waterfall:true});r.context.ESPHomeHA.refresh();for(let i=0;i<90;i++)r.tick();
  discoveryHeader(r,640,136);const count=r.geometry.size;assert(r.context.ESPHomeHA.snapshot().discovery.open);
  for(let i=0;i<10;i++)clickNativeCaption(r,'下一页');
  for(const b of r.buttons)if(!r.hidden.get(b.id)){const g=r.geometry.get(b.id);assert(g.x>=164&&g.x+g.w<=640&&g.y>=64&&g.y+g.h<=400);assert(g.h>=40);}
  assert.equal(r.geometry.size,count);clickNativeCaption(r,'返回');assert(!r.context.ESPHomeHA.snapshot().discovery.open);assert.equal(r.controls.length,0);
});

test('filter panel completely hides and locks the underlying waterfall and shrinks to its content',()=>{
  const r=boot(1024,600,{waterfall:true});r.context.ESPHomeHA.refresh();for(let i=0;i<90;i++)r.tick();
  const view=r.context.ESPHomeHA.snapshot().waterfall.viewport;r.context.ui.scrollTo(view,100,false);for(let i=0;i<40;i++)r.tick();
  discoveryHeader(r,1024,136);assert(r.hidden.get(view),'all card labels and the native scrollbar must be hidden with their parent');assert(r.viewports.get(view).locked);
  const clear=r.buttons.find(button=>!r.hidden.get(button.id)&&r.textValues.get(button.id)==='清除筛选');assert(clear);
  assert(r.geometry.get(clear.id).y<330,'a single all-rooms option must not reserve a tall empty panel');
  assert([...r.textValues].some(([id,text])=>!r.hidden.get(id)&&text==='家庭中枢尚未分配房间'));
  clickNativeCaption(r,'返回');assert(!r.hidden.get(view));assert(!r.viewports.get(view).locked);assert.equal(r.context.ESPHomeHA.snapshot().waterfall.scrollY,100);
  assert.equal(r.controls.length,0);
});

test('empty and filtered states offer a recovery action and the header has separate 44px touch targets',()=>{
  for(const [width,height] of [[640,400],[1024,600]]){
    const r=boot(width,height,{waterfall:true,states:[]});r.context.ESPHomeHA.refresh();for(let i=0;i<80;i++)r.tick();
    assert([...r.textValues].some(([id,value])=>!r.hidden.get(id)&&value==='暂无设备'));
    for(const offset of [180,136,92,48])assert(r.buttons.some(button=>{const g=r.geometry.get(button.id);return !r.hidden.get(button.id)&&g.x===width-offset&&g.w===44&&g.h===44;}));
    discoveryHeader(r,width,180);r.prompts.at(-1).callback('missing');for(let i=0;i<60;i++)r.tick();assert([...r.textValues].some(([id,value])=>!r.hidden.get(id)&&value==='没有匹配的设备'));
    clickNativeCaption(r,'清除筛选');assert.equal(r.context.ESPHomeHA.snapshot().search,'');r.context.ESPHomeHA.disconnect();for(let i=0;i<60;i++)r.tick();clickNativeCaption(r,'连接设置');assert(r.context.ESPHomeHA.snapshot().settingsOpen);
  }
});

test('device detail provides a local follow shortcut used by discovery',()=>{
  const r=boot(1024,600,{waterfall:true});r.context.ESPHomeHA.refresh();for(let i=0;i<80;i++)r.tick();deviceBody(r,'light.desk').callback();for(let i=0;i<60;i++)r.tick();clickNativeCaption(r,'关注设备');assert(r.context.ESPHomeHA.snapshot().favorites.includes('light.desk'));clickNativeCaption(r,'取消关注');assert(!r.context.ESPHomeHA.snapshot().favorites.includes('light.desk'));assert.equal(r.controls.length,0);
});

test('profile detail keeps primary controls while extra member fields expand and collapse locally',()=>{
  const primary={entity_id:'fan.more',state:'on',attributes:{friendly_name:'风扇',percentage:40,supported_features:49}};
  const states=[primary,...Array.from({length:9},(_,i)=>({entity_id:'sensor.more_'+i,state:String(i+20),attributes:{friendly_name:'附加读数 '+i}}))];
  const devices=[['more_device','风扇','客厅',states.map(state=>state.entity_id)]];
  const r=boot(1024,536,{states,devices,waterfall:true,services:commonServices(),nativeActions:true});r.context.ESPHomeHA.refresh();for(let i=0;i<90;i++)r.tick();
  deviceBody(r,'fan.more').callback();for(let i=0;i<60;i++)r.tick();const before=r.context.ESPHomeHA.snapshot().detailPageIndicator.total;
  clickNativeCaption(r,'更多参数');assert(r.context.ESPHomeHA.snapshot().detailExpanded);assert(r.context.ESPHomeHA.snapshot().detailPageIndicator.total>before);
  clickNativeCaption(r,'收起参数');assert(!r.context.ESPHomeHA.snapshot().detailExpanded);assert.equal(r.context.ESPHomeHA.snapshot().detailPageIndicator.total,before);assert.equal(r.controls.length,0);
});

test('following a grouped device removes an existing secondary-entity follow rather than adding a conflicting primary follow',()=>{
  const states=[{entity_id:'switch.group_main',state:'on',attributes:{friendly_name:'插座'}},{entity_id:'sensor.group_watt',state:'25',attributes:{friendly_name:'功率',unit_of_measurement:'W'}}];
  const r=boot(1024,600,{states,devices:[['groupfollow','插座','客厅',states.map(row=>row.entity_id)]],waterfall:true,favorites:['sensor.group_watt']});
  r.context.ESPHomeHA.refresh();for(let i=0;i<80;i++)r.tick();deviceBody(r,'switch.group_main').callback();for(let i=0;i<50;i++)r.tick();clickNativeCaption(r,'取消关注');assert.equal(r.context.ESPHomeHA.snapshot().favorites.length,0);assert.equal(r.controls.length,0);
});

test('native-capable Mi boards use one continuous shortest-column waterfall with a bounded card pool',()=>{
  for(const [width,height] of [[1024,600],[640,400],[800,480]]){
    const states=Array.from({length:512},(_,i)=>i%3===0?{entity_id:'light.fall_'+i,state:'on',attributes:{friendly_name:'台灯 '+i,brightness:128,supported_color_modes:['brightness']}}:i%3===1?{entity_id:'switch.fall_'+i,state:'off',attributes:{friendly_name:'插座 '+i}}:{entity_id:'sensor.fall_'+i,state:'23',attributes:{friendly_name:'温度 '+i,device_class:'temperature'}});
    const r=boot(width,height,{states,waterfall:true});r.context.ESPHomeHA.refresh();for(let i=0;i<90;i++)r.tick();
    let snap=r.context.ESPHomeHA.snapshot(),flow=snap.waterfall;assert(flow);assert.equal(snap.layoutPages.length,1);assert.equal(snap.layoutPages[0].length,512);assert(!snap.pageIndicator.visible);
    assert.equal(flow.columns,width===1024?3:2);assert.equal(flow.poolSlots,12);assert.equal(r.buttons.length,64);
    assert(new Set(snap.layoutPages[0].map(row=>row.h)).size>1);assert(flow.contentHeight>height);
    const bottoms=Array(flow.columns).fill(76),xs=[...new Set(snap.layoutPages[0].map(row=>row.x))].sort((a,b)=>a-b);
    for(const row of snap.layoutPages[0]){const col=xs.indexOf(row.x);assert.equal(row.y,Math.min(...bottoms));assert(row.x>=164&&row.x+row.w<=width-14);bottoms[col]=row.y+row.h+14;}
    const view=flow.viewport,max=r.context.ui.scrollInfo(view).max;
    for(let j=0;j<12;j++){r.context.ui.scrollTo(view,max*j/11,false);for(let i=0;i<35;i++)r.tick();}
    snap=r.context.ESPHomeHA.snapshot();assert.equal(snap.waterfall.scrollY,max);assert(snap.waterfall.visibleCards.length<=12);assert(r.geometry.size<500);
    const allocated=r.geometry.size;for(let j=0;j<50;j++){r.context.ui.scrollTo(view,max,false);for(let i=0;i<3;i++)r.tick();}assert.equal(r.geometry.size,allocated);
    assert.equal(r.controls.length,0);
    states.splice(1);r.context.ESPHomeHA.refresh();for(let i=0;i<80;i++)r.tick();assert.equal(r.context.ESPHomeHA.snapshot().waterfall.scrollY,0);assert.equal(r.context.ESPHomeHA.snapshot().layoutPages[0].length,1);
  }
});
test('native scrolling within the retained window avoids JS repaints and prioritizes newly visible cards',()=>{
  const states=Array.from({length:120},(_,i)=>({entity_id:'light.window_'+i,state:'on',attributes:{friendly_name:'灯 '+i,brightness:128,supported_color_modes:['brightness']}}));
  const r=boot(1024,600,{waterfall:true,states,nowMs:()=>10000});r.context.ESPHomeHA.refresh();for(let i=0;i<90;i++)r.tick();
  const view=r.context.ESPHomeHA.snapshot().waterfall.viewport,allocated=r.geometry.size;
  r.nativeWrites.length=0;
  for(let y=1;y<=8;y++){r.context.ui.scrollTo(view,y,false);for(let i=0;i<6;i++)r.tick();}
  assert.equal(r.context.ESPHomeHA.snapshot().waterfall.scrollY,8);assert.equal(r.nativeWrites.length,0,'native clipping moves existing cards without repainting the page');
  r.context.ui.scrollTo(view,3000,false);r.tick();
  const flow=r.context.ESPHomeHA.snapshot().waterfall;
  assert(flow.visibleCards.some(row=>row.y+row.h>64&&row.y<64+flow.h),'new visible cards are painted in the first batch');
  for(let i=0;i<50;i++)r.tick();assert(r.geometry.size<500);assert.equal(r.controls.length,0);
  const warm=r.geometry.size;r.context.ui.scrollTo(view,0,false);for(let i=0;i<40;i++)r.tick();r.context.ui.scrollTo(view,3000,false);for(let i=0;i<40;i++)r.tick();assert.equal(r.geometry.size,warm);
});

test('a held waterfall button cannot control a different entity after card recycling',()=>{
  const states=Array.from({length:60},(_,i)=>({entity_id:'light.press_'+i,state:'on',attributes:{friendly_name:'灯 '+i,brightness:128,supported_color_modes:['brightness']}}));
  const r=boot(1024,600,{waterfall:true,states,nativeActions:true,acceptControls:true});r.context.ESPHomeHA.refresh();for(let i=0;i<90;i++)r.tick();
  const before=r.context.ESPHomeHA.snapshot().waterfall,row=before.visibleCards.find(row=>row.y>=64),x=row.x+row.w-36,y=row.y+38;
  const buttonAt=()=>r.buttons.find(button=>{const g=r.geometry.get(button.id);return !r.hidden.get(button.id)&&g.w===48&&g.h===48&&x>=g.x&&x<=g.x+g.w&&y>=g.y&&y<=g.y+g.h;});
  r.touch('down',x/1024,y/600);states.splice(0,3);r.context.ESPHomeHA.refresh();for(let i=0;i<90;i++)r.tick();r.touch('up',x/1024,y/600);
  const recycled=buttonAt();assert(recycled);recycled.callback();assert.equal(r.controls.length,0,'refresh during a press cannot retarget the command');
  r.touch('down',x/1024,y/600);r.touch('up',x/1024,y/600);buttonAt().callback();assert.equal(r.controls.length,1,'a fresh deliberate press works');
});

test('waterfall input teardown immediately unlocks scrolling and rejects late releases',()=>{
  for(const mode of ['disconnect','settings','settings-button','dispose','invalid-pointer','failed','removed','capability']){
    const states=Array.from({length:24},(_,i)=>({entity_id:'light.life_'+i,state:'on',attributes:{friendly_name:'灯 '+i,brightness:128,supported_color_modes:['brightness']}}));
    const options={waterfall:true,states,nativeActions:true,acceptControls:true};
    const r=boot(1024,600,options);r.context.ESPHomeHA.refresh();for(let i=0;i<90;i++)r.tick();
    const snap=r.context.ESPHomeHA.snapshot(),view=snap.waterfall.viewport,rail=snap.brightnessControls[0];
    const x=(rail.x+rail.w*.3)/1024,y=(rail.y+rail.h/2)/600;
    r.touch('down',x,y);r.touch('move',x+30/1024,y);assert(r.viewports.get(view).locked,mode);
    if(mode==='disconnect')r.context.ESPHomeHA.disconnect();
    else if(mode==='settings')r.context.ESPHomeHA.settings();
    else if(mode==='settings-button'){
      const button=r.buttons.find(button=>{const box=r.geometry.get(button.id);return !r.hidden.get(button.id)&&box.x===1024-92&&box.y===10;});assert(button);button.callback();
    }
    else if(mode==='dispose')r.context.ESPHomeHA.dispose();
    else if(mode==='invalid-pointer')r.touch('move',NaN,y);
    else{
      if(mode==='failed')options.stateStatus=503;
      if(mode==='removed')states.length=0;
      if(mode==='capability'){states[0].attributes.supported_color_modes=['onoff'];delete states[0].attributes.brightness;}
      r.context.ESPHomeHA.refresh();for(let i=0;i<60;i++)r.tick();
    }
    assert(!r.viewports.get(view).locked,mode+' releases native scroll ownership without waiting for up');
    assert.equal(r.context.ESPHomeHA.snapshot().brightnessInput,null,mode);
    r.touch('up',x+30/1024,y);assert.equal(r.controls.length,0,mode+' cannot submit its late release');
  }
});

test('weak-network brightness repeats coalesce while reversal preserves the newest target',()=>{
  let now=10000;
  const options={waterfall:true,nativeActions:true,acceptControls:true,nowMs:()=>now};
  const r=boot(1024,600,options);r.context.ESPHomeHA.refresh();for(let i=0;i<90;i++)r.tick();
  const rail=r.context.ESPHomeHA.snapshot().brightnessControls[0];assert(rail);
  const drag=p=>{const x=(rail.x+rail.w*p/100)/1024,y=(rail.y+rail.h/2)/600;r.touch('down',x,y);r.touch('up',x,y);for(let i=0;i<20;i++)r.tick();};
  options.holdRequests=true;drag(60);assert.equal(r.controls.length,1);
  for(let i=0;i<20;i++)drag(60);
  assert.equal(r.context.ESPHomeHA.snapshot().controlQueue.length,0,'identical in-flight targets do not queue');
  drag(30);drag(60);drag(60);assert.equal(r.context.ESPHomeHA.snapshot().controlQueue.length,1);assert.equal(r.context.ESPHomeHA.snapshot().controlQueue[0].brightness,60,'returning to an earlier target must replace the newer unsent reversal');
  now+=4100;for(let i=0;i<20;i++)r.tick();
  assert(r.context.ESPHomeHA.snapshot().controlProgress.some(row=>row.entity===rail.entity&&row.slow&&!row.sent));
  assert([...r.textValues].some(([id,text])=>!r.hidden.get(id)&&text==='等待前序操作'));
  options.holdRequests=false;for(let i=0;i<90;i++)r.tick();assert.equal(r.controls.length,2);assert.equal(r.context.ESPHomeHA.snapshot().controlQueue.length,0);
});

test('weak-network same mode selection is deduplicated until completion then may be selected again',()=>{
  const r=lampFixture();const transport=r.fixtureOptions;
  transport.holdRequests=true;lampClickLabel(r,'夜间');for(let i=0;i<12;i++)lampClickLabel(r,'夜间');
  assert.equal(r.controls.length,1);assert.equal(r.context.ESPHomeHA.snapshot().controlQueue.length,0);
  lampClickLabel(r,'日间');lampClickLabel(r,'夜间');assert.equal(r.context.ESPHomeHA.snapshot().controlQueue.length,1);
  transport.holdRequests=false;for(let i=0;i<80;i++)r.tick();assert.equal(r.controls.length,2);
  lampClickLabel(r,'夜间');for(let i=0;i<40;i++)r.tick();assert.equal(r.controls.length,3,'dedupe does not suppress a later deliberate request');
});

test('slow admitted control reports waiting for the device without retrying or claiming success',()=>{
  let now=10000;const options={waterfall:true,nativeActions:true,acceptControls:true,nowMs:()=>now};
  const r=boot(1024,600,options);r.context.ESPHomeHA.refresh();for(let i=0;i<90;i++)r.tick();
  const rail=r.context.ESPHomeHA.snapshot().brightnessControls[0],x=(rail.x+rail.w*.55)/1024,y=(rail.y+rail.h/2)/600;
  options.holdRequests=true;r.touch('down',x,y);r.touch('up',x,y);for(let i=0;i<20;i++)r.tick();now+=4100;
  for(let i=0;i<40;i++)r.tick();assert.equal(r.controls.length,1);assert.equal(r.context.ESPHomeHA.snapshot().controlFeedback.length,0);
  assert([...r.textValues].some(([id,text])=>!r.hidden.get(id)&&text==='等待设备回报'));
  const writes=r.nativeWrites.length;for(let i=0;i<30;i++)r.tick();assert.equal(r.nativeWrites.length,writes,'waiting does not create a looping paint effect');
  options.holdRequests=false;for(let i=0;i<90;i++)r.tick();assert(r.context.ESPHomeHA.snapshot().controlFeedback.some(row=>row.kind==='success'));assert.equal(r.controls.length,1);
});

test('waterfall refresh anchors the visible device when earlier cards disappear',()=>{
  const states=Array.from({length:60},(_,i)=>({entity_id:'light.anchor_'+i,state:'on',attributes:{friendly_name:'灯 '+i,brightness:128,supported_color_modes:['brightness']}}));
  const r=boot(1024,600,{states,waterfall:true});r.context.ESPHomeHA.refresh();for(let i=0;i<90;i++)r.tick();
  const view=r.context.ESPHomeHA.snapshot().waterfall.viewport;r.context.ui.scrollTo(view,650,false);for(let i=0;i<40;i++)r.tick();
  const before=r.context.ESPHomeHA.snapshot().waterfall.visibleCards.filter(row=>row.y+row.h>64).sort((a,b)=>a.y-b.y||a.x-b.x)[0];assert(before);
  states.splice(0,3);r.context.ESPHomeHA.refresh();for(let i=0;i<90;i++)r.tick();
  const after=r.context.ESPHomeHA.snapshot().waterfall.visibleCards.find(row=>row.entity_id===before.entity_id);assert(after);
  assert.equal(after.y,before.y,'the visible device stays at the same screen height');assert(r.context.ESPHomeHA.snapshot().waterfall.scrollY<650);
  const scroll=r.context.ESPHomeHA.snapshot().waterfall.scrollY;r.context.ESPHomeHA.refresh();for(let i=0;i<60;i++)r.tick();assert.equal(r.context.ESPHomeHA.snapshot().waterfall.scrollY,scroll);
  assert.equal(r.controls.length,0);
});

test('card readings reduce type size before truncating a long numeric value',()=>{
  const value='1234567890123';
  const r=boot(1024,600,{waterfall:true,states:[{entity_id:'sensor.long_reading',state:value,attributes:{friendly_name:'用电累计读数'}}]});
  r.context.ESPHomeHA.refresh();for(let i=0;i<90;i++)r.tick();
  const reading=[...r.textValues].find(([id,text])=>!r.hidden.get(id)&&text===value);assert(reading,'retain the full number when it fits');
  assert(r.styles.get(reading[0]).fontSize<=28);assert.equal(r.controls.length,0);
});

test('card control feedback separates pending, confirmed and unconfirmed results and expires',()=>{
  for(const controlStatus of [200,500]){
    let now=10000;
    const r=boot(1024,600,{waterfall:true,nativeActions:true,acceptControls:true,controlStatus,nowMs:()=>now});
    r.context.ESPHomeHA.refresh();for(let i=0;i<90;i++)r.tick();
    const rail=r.context.ESPHomeHA.snapshot().brightnessControls[0];assert(rail);
    const x=(rail.x+rail.w*.6)/1024,y=(rail.y+rail.h/2)/600;r.touch('down',x,y);r.touch('up',x,y);
    assert(r.context.ESPHomeHA.snapshot().controlFeedback.every(row=>row.kind!=='success'),'admission is not confirmation');
    for(let i=0;i<60;i++)r.tick();
    const feedback=r.context.ESPHomeHA.snapshot().controlFeedback.find(row=>row.entity===rail.entity);assert(feedback);
    assert.equal(feedback.kind,controlStatus===200?'success':'error');assert.equal(r.controls.length,1);
    now+=6000;for(let i=0;i<15;i++)r.tick();assert.equal(r.context.ESPHomeHA.snapshot().controlFeedback.length,0);assert.equal(r.controls.length,1);
  }
});

test('waterfall scroll does not issue commands and a scrolled slider owns only its release gesture',()=>{
  const states=Array.from({length:60},(_,i)=>({entity_id:'light.scroll_'+i,state:'on',attributes:{friendly_name:'灯 '+i,brightness:128,supported_color_modes:['brightness']}}));
  const r=boot(1024,600,{states,waterfall:true,nativeActions:true,acceptControls:true});r.context.ESPHomeHA.refresh();for(let i=0;i<90;i++)r.tick();
  const view=r.context.ESPHomeHA.snapshot().waterfall.viewport;r.context.ui.scrollTo(view,430,false);for(let i=0;i<60;i++)r.tick();assert.equal(r.controls.length,0);
  const scroll=r.context.ESPHomeHA.snapshot().waterfall.scrollY;
  const rail=r.context.ESPHomeHA.snapshot().brightnessControls.find(rect=>rect.y-scroll>=64&&rect.y-scroll+rect.h<546);assert(rail);
  const x=(rail.x+rail.w*.6)/1024,y=(rail.y-scroll+rail.h/2)/600;
  r.touch('down',x,y);assert(r.viewports.get(view).locked);r.touch('move',x,y);assert.equal(r.controls.length,0);
  r.touch('up',x,y);assert(!r.viewports.get(view).locked);assert.equal(r.controls.length,1);
  const count=r.controls.length;r.touch('down',x,.98);r.touch('up',x,.98);assert.equal(r.controls.length,count,'clipped rows cannot receive footer gestures');
});


test('waterfall slider distinguishes vertical scrolling, horizontal adjustment and a deliberate tap',()=>{
  for(const dark of [false,true]){
    const states=Array.from({length:30},(_,i)=>({entity_id:'light.axis_'+i,state:'on',attributes:{friendly_name:'台灯 '+i,brightness:128,supported_color_modes:['brightness']}}));
    const r=boot(1024,600,{states,waterfall:true,nativeActions:true,acceptControls:true,dark});
    r.context.ESPHomeHA.refresh();for(let i=0;i<90;i++)r.tick();
    const snapshot=r.context.ESPHomeHA.snapshot(),view=snapshot.waterfall.viewport;
    const rail=snapshot.brightnessControls.find(row=>row.y>=64&&row.y+row.h<546);assert(rail);
    const x=(rail.x+rail.w*.3)/1024,y=(rail.y+rail.h/2)/600;
    r.nativeWrites.length=0;r.touch('down',x,y);r.touch('move',x+2/1024,y-3/600);
    assert(!r.nativeWrites.some(w=>w.method==='text'&&String(w.value).includes('%')),'touch slop does not alter displayed brightness');
    r.touch('move',x+3/1024,y-18/600);assert(!r.viewports.get(view).locked);
    r.touch('up',x+3/1024,y-18/600);assert.equal(r.controls.length,0,'vertical scroll never submits a brightness value');
    r.touch('down',x,y);r.touch('up',x,y-24/600);assert.equal(r.controls.length,0,'a fast vertical release also cancels adjustment');
    r.touch('down',x,y);r.touch('move',x+30/1024,y+2/600);
    assert(r.viewports.get(view).locked);assert.equal(r.controls.length,0);
    assert(r.nativeWrites.some(w=>w.method==='text'&&/\d+% · 已开启/.test(w.value)),'drag retains power status');
    r.touch('cancel',x+30/1024,y);assert(!r.viewports.get(view).locked);assert.equal(r.controls.length,0);
    r.touch('down',x,y);r.touch('up',x,y);assert.equal(r.controls.length,1,'tap deliberately selects brightness');
    assert.equal(r.controls[0].body.brightness_pct,30);
  }
});

test('waterfall preserves the scrolled reading position across device details and scene controls stay explicit',()=>{
  const states=Array.from({length:30},(_,i)=>({entity_id:'light.return_'+i,state:'on',attributes:{friendly_name:'台灯 '+i,brightness:128,supported_color_modes:['brightness']}}));
  const r=boot(1024,600,{states,waterfall:true});r.context.ESPHomeHA.refresh();for(let i=0;i<90;i++)r.tick();
  const view=r.context.ESPHomeHA.snapshot().waterfall.viewport;r.context.ui.scrollTo(view,430,false);for(let i=0;i<60;i++)r.tick();
  const row=r.context.ESPHomeHA.snapshot().waterfall.visibleCards.find(row=>row.y>=64&&row.y+row.h<546);assert(row);
  assert(r.context.ESPHomeHA.openDevice('http://192.168.1.20:8123',row.entity_id));for(let i=0;i<60;i++)r.tick();assert(r.hidden.get(view));
  const back=r.buttons.find(b=>{const g=r.geometry.get(b.id);return !r.hidden.get(b.id)&&g.x===176&&g.y===16&&g.w===32;});assert(back);back.callback();for(let i=0;i<60;i++)r.tick();
  assert.equal(r.context.ESPHomeHA.snapshot().waterfall.scrollY,430);assert(!r.hidden.get(view));assert.equal(r.controls.length,0);
  const sceneStates=Array.from({length:30},(_,i)=>({entity_id:'scene.fall_'+i,state:'unknown',attributes:{friendly_name:'场景 '+i}}));
  const scenes=boot(1024,600,{waterfall:true,sceneSync:true,sceneStates,nativeActions:true,acceptControls:true,services:sceneServices});scenes.context.ESPHomeHA.refresh();for(let i=0;i<80;i++)scenes.tick();chooseSection(scenes,162);
  assert.equal(scenes.context.ESPHomeHA.snapshot().layoutPages.length,1);assert.equal(scenes.context.ESPHomeHA.snapshot().layoutPages[0].length,30);assert.equal(scenes.controls.length,0);
  const button=sceneExecuteButton(scenes,'scene.fall_0');assert(button);button.callback();assert.equal(scenes.controls.length,1);
});

test('bounded text metrics cache avoids repeated native measurements without changing camera metric layout',()=>{
  const uncached=source.replace(/function measureTextCached\(text,font\) \{[\s\S]*?\n  \}/,
    'function measureTextCached(text,font) { return ui.measureText(text,font); }');
  assert.notEqual(uncached,source);
  const run=script=>{
    let calls=0;
    const states=[{entity_id:'camera.entry',state:'idle',attributes:{friendly_name:'监控摄像机',device_type:'camera',device_id:'entry',miot_properties:{'battery.battery-level':86}}}];
    const r=boot(1024,536,{source:script,states,measureMetrics:(text,font)=>{calls++;return {width:String(text).length*12,height:font===43?32:18,baseline:font===43?5:3};}});
    r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
    deviceBody(r,'camera.entry').callback();for(let i=0;i<25;i++)r.tick();
    const initial=calls;
    for(let n=0;n<15;n++){states[0].attributes.miot_properties['battery.battery-level']=n%2?86:87;r.context.ESPHomeHA.refresh();for(let i=0;i<30;i++)r.tick();}
    const metrics=[...r.textValues].filter(([id,text])=>!r.hidden.get(id)&&['86','%'].includes(text)).map(([id,text])=>({text,...r.geometry.get(id)}));
    return {calls,initial,metrics};
  };
  const before=run(uncached),after=run(source);
  assert(before.calls>before.initial,'fixture exercises repeated native measurements');
  assert(after.calls<before.calls*.2,'unchanged readings should reuse measured metrics');
  assert.deepEqual(after.metrics,before.metrics,'pixel geometry and baseline stay unchanged');
  console.log('TEXT_METRICS_CALLS',JSON.stringify({before:before.calls,after:after.calls}));
});

test('large device function ordering computes descriptors once per member and preserves permissions and order',()=>{
  const begin=source.indexOf('  function deviceFunctions(){'),end=source.indexOf('  function functionControls(item){',begin);
  assert(begin>=0&&end>begin);
  const oldBody=`  function deviceFunctions(){
    const card=currentDeviceCard(),ids=card&&card.device_entities||[selectedId];
    if(nav==='scenes'&&current())return [current()];
    return ids.map(id=>indexedEntity(id)).filter(Boolean).sort((a,b)=>
      Number(functionControls(b).length>0)-Number(functionControls(a).length>0));
  }
`;
  const oldSource=source.slice(0,begin)+oldBody+source.slice(end);
  const run=(script,canControl)=>{
    script=script.replace('function functionControls(item){','function functionControls(item){globalThis.__specCalls=(globalThis.__specCalls||0)+1;');
    const states=Array.from({length:100},(_,i)=>i%3===0?
      {entity_id:'switch.perf_'+i,state:'on',attributes:{friendly_name:'插座 '+i}}:
      {entity_id:'sensor.perf_'+i,state:String(i),attributes:{friendly_name:'温度 '+i,device_class:'temperature',unit_of_measurement:'C'}});
    const r=boot(1024,600,{source:script,states,devices:[['perf','组合设备','客厅',states.map(s=>s.entity_id)]],waterfall:true,canControl});
    r.context.ESPHomeHA.refresh();for(let i=0;i<90;i++)r.tick();
    deviceBody(r,'switch.perf_0').callback();for(let i=0;i<60;i++)r.tick();
    r.context.__specCalls=0;
    const snapshot=r.context.ESPHomeHA.snapshot();
    return {calls:r.context.__specCalls,functions:JSON.stringify(snapshot.functionControls),members:JSON.stringify(snapshot.deviceCards.map(c=>c.members)),controls:r.controls.length};
  };
  for(const canControl of [true,false]){
    const before=run(oldSource,canControl),after=run(source,canControl);
    assert.equal(after.functions,before.functions);assert.equal(after.members,before.members);
    assert.equal(after.controls,0);assert(after.calls<before.calls);
    console.log('DEVICE_DESCRIPTOR_CALLS',JSON.stringify({canControl,before:before.calls,after:after.calls}));
  }
});

test('waterfall indexed window matches full-scan selection at every scroll depth and after layout replacement',()=>{
  const begin=source.indexOf('    // A layout\'s rows are immutable'),end=source.indexOf('    candidates.sort((a,b)=>{',begin);
  assert(begin>=0&&end>begin);
  const oldSource=source.slice(0,begin)+'    const candidates=rows.filter(row=>row.y+row.h>top-110&&row.y<bottom+110);\n'+source.slice(end);
  const run=script=>{
    script=script.replace('  function snapshot() {',`  function snapshot() {
      globalThis.__windowRows=()=>dashboardPages()[0].items;
      globalThis.__windowSample=y=>{boardScroll=y;return boardWindow(dashboardPages()[0].items).map(row=>row?row.item.entity_id:null);};`);
    const states=Array.from({length:512},(_,i)=>i%3===0?
      {entity_id:'light.index_'+i,state:'on',attributes:{friendly_name:'灯 '+i,brightness:128,supported_color_modes:['brightness']}}:
      {entity_id:'sensor.index_'+i,state:String(i),attributes:{friendly_name:'温度 '+i,device_class:'temperature'}});
    const r=boot(1024,600,{source:script,states,waterfall:true});
    r.context.ESPHomeHA.refresh();for(let i=0;i<90;i++)r.tick();
    const snapshots=[];let yReads=0;
    for(let pass=0;pass<2;pass++){
      const snap=r.context.ESPHomeHA.snapshot(),rows=r.context.__windowRows();
      for(const row of rows){const y=row.y;Object.defineProperty(row,'y',{get(){yReads++;return y;}});}
      for(let i=0;i<=100;i++)snapshots.push(JSON.stringify(r.context.__windowSample((snap.waterfall.contentHeight-500)*i/100)));
      if(!pass){states.splice(0,47);r.context.ESPHomeHA.refresh();for(let i=0;i<90;i++)r.tick();}
    }
    return {snapshots,yReads,controls:r.controls.length};
  };
  const before=run(oldSource),after=run(source);
  assert.deepEqual(after.snapshots,before.snapshots,'same pooled slots for all 202 scroll positions');
  assert.equal(after.controls,0);assert(after.yReads<before.yReads*.4);
  console.log('WATERFALL_Y_READS',JSON.stringify({before:before.yReads,after:after.yReads}));
});

function internalFunctionsFixture(){
  const states=[
    {entity_id:'fan.internal',state:'on',attributes:{friendly_name:'客厅风扇',supported_features:63,direction:'forward',percentage:40,preset_modes:['normal','sleep']}},
    {entity_id:'cover.internal',state:'open',attributes:{friendly_name:'百叶窗',supported_features:255,current_position:50,current_tilt_position:10}},
    {entity_id:'climate.internal',state:'cool',attributes:{friendly_name:'空调',supported_features:2|4|128|256|512,target_temp_low:22,target_temp_high:27,min_temp:16,max_temp:30,target_temp_step:.5,humidity:50,min_humidity:40,max_humidity:60,swing_horizontal_modes:['off','on'],swing_horizontal_mode:'off'}},
    {entity_id:'water_heater.internal',state:'electric',attributes:{friendly_name:'热水器',supported_features:15,temperature:50,min_temp:35,max_temp:75,away_mode:false,operation_mode:'electric',operation_list:['electric','eco']}},
    {entity_id:'button.internal',state:'unknown',attributes:{friendly_name:'执行按钮'}},
    {entity_id:'sensor.internal',state:'25',attributes:{friendly_name:'温度',unit_of_measurement:'°C'}}
  ];
  const catalog={fan:['turn_on','turn_off','set_direction','set_percentage','set_preset_mode'],cover:['open_cover','close_cover','stop_cover','open_cover_tilt','close_cover_tilt','stop_cover_tilt','set_cover_position','set_cover_tilt_position'],climate:['turn_on','turn_off','set_temperature','set_humidity','set_swing_horizontal_mode'],water_heater:['turn_on','turn_off','set_temperature','set_operation_mode','set_away_mode'],button:['press']};
  const services=Object.entries(catalog).map(([domain,actions])=>({domain,services:Object.fromEntries(actions.map(a=>[a,{}]))}));
  const instrumented=source.replace('  globalThis.ESPHomeHA = {','  globalThis.__internal={functionControls,requestFunction,indexedEntity,supports,serviceReached};\n  globalThis.ESPHomeHA = {');
  const r=boot(1024,536,{nativeActions:true,acceptControls:true,states,services,source:instrumented});
  r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
  r.internal=r.context.__internal;r.states=states;
  r.specs=id=>r.internal.functionControls(r.internal.indexedEntity(id));
  r.run=(id,key,value)=>{const item=r.internal.indexedEntity(id),spec=r.specs(id).find(s=>s.key===key);assert(spec,id+' '+key);r.internal.requestFunction(item,spec,value);for(let i=0;i<25;i++)r.tick();};
  return r;
}
test('device internal functions preserve actual capability flags and expose missing controls',()=>{
  const r=internalFunctionsFixture();
  assert(r.specs('fan.internal').some(s=>s.service==='set_direction'));
  assert(r.specs('cover.internal').some(s=>s.key==='tilt_position'));
  for(const key of ['swing_horizontal_mode','target_temp_low','target_temp_high','humidity'])assert(r.specs('climate.internal').some(s=>s.key===key),key);
  assert(r.specs('water_heater.internal').some(s=>s.key==='away_mode'));
  assert.equal(r.specs('sensor.internal').length,0);
  const fan=r.internal.indexedEntity('fan.internal');fan.supported_features=32;fan.state='off';
  assert.equal(r.internal.supports(fan,'turn_on'),true);assert.equal(r.internal.supports(fan,'turn_off'),false);
  assert.equal(r.specs('fan.internal').some(s=>s.key==='direction'),false);
  const climate=r.internal.indexedEntity('climate.internal');climate.supported_features=128;
  assert.equal(r.internal.supports(climate,'turn_on'),false);assert.equal(r.internal.supports(climate,'turn_off'),true);
});
test('device internal function operations send exact keys and verify corresponding readback',()=>{
  const r=internalFunctionsFixture();
  for(const [id,key,value] of [['fan.internal','direction','reverse'],['cover.internal','tilt_position',60],['climate.internal','swing_horizontal_mode','on'],['climate.internal','humidity',55],['water_heater.internal','away_mode',true]]){
    const count=r.controls.length;r.run(id,key,value);assert.equal(r.controls.length,count+1);
    assert.equal(r.controls.at(-1).entity,id);assert.equal(r.controls.at(-1).body[key],value);
    assert.equal(r.context.ESPHomeHA.snapshot().phase,'ready',id+' readback');
  }
  r.run('climate.internal','target_temp_low',23);
  assert.deepEqual(r.controls.at(-1).body,{entity_id:'climate.internal',target_temp_low:23,target_temp_high:27});
  const count=r.controls.length;r.run('climate.internal','target_temp_high',20);assert.equal(r.controls.length,count,'invalid pair never posted');
  const climate=r.internal.indexedEntity('climate.internal');climate.state='cool';
  assert.equal(r.internal.serviceReached(climate,'turn_on'),true,'climate power does not require state=on');
  const cover=r.internal.indexedEntity('cover.internal');cover.current_tilt_position=20;
  assert.equal(r.internal.serviceReached(cover,'open_cover_tilt'),false,'POST alone cannot verify tilt open');
});
test('device internal stateless buttons report acceptance rather than confirmed completion',()=>{
  const r=internalFunctionsFixture();const item=r.internal.indexedEntity('button.internal'),spec=r.specs(item.entity_id).find(s=>s.service==='press');assert(spec);
  r.internal.requestFunction(item,spec);for(let i=0;i<25;i++)r.tick();
  assert.equal(r.controls.at(-1).action,'press');assert.equal(r.context.ESPHomeHA.snapshot().phase,'ready');
  assert.equal(r.context.ESPHomeHA.snapshot().controlFeedback.find(f=>f.entity==='button.internal').kind,'accepted');
});

test('English UI preserves Chinese device names and exact service option values',()=>{
  const states=[{entity_id:'light.locale',state:'on',attributes:{friendly_name:'设置',
    supported_features:4,supported_color_modes:['brightness'],brightness:128,effect_list:['自动','睡眠'],effect:'睡眠'}}];
  const services=[{domain:'light',services:{turn_on:{},turn_off:{}}}];
  const instrumented=source.replace('  globalThis.ESPHomeHA = {',
    '  globalThis.__locale={functionControls,requestFunction,indexedEntity};\n  globalThis.ESPHomeHA = {');
  const r=boot(1024,536,{language:'en',nativeActions:true,acceptControls:true,states,services,source:instrumented});
  r.context.ESPHomeHA.refresh();for(let i=0;i<40;i++)r.tick();
  assert([...r.textValues.values()].some(t=>t==='Overview'));
  const api=r.context.__locale,item=api.indexedEntity('light.locale');
  assert.equal(item.name,'设置');
  const spec=api.functionControls(item).find(s=>s.key==='effect');assert(spec);
  api.requestFunction(item,spec,'自动');for(let i=0;i<25;i++)r.tick();
  assert.equal(r.controls.at(-1).entity,'light.locale');
  assert.equal(r.controls.at(-1).body.effect,'自动');
});
