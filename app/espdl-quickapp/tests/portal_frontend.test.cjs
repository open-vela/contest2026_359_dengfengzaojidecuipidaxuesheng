'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname,
  '../overlay/apps/system/desktop/portal/app.js'), 'utf8');
const bearer = 'a'.repeat(32);

function boot() {
  const nodes = new Map(), intervals = [], calls = [], storage = [];
  const documentEvents = {}, windowEvents = {};
  const node = id => {
    if (!nodes.has(id)) nodes.set(id, {
      value: '', hidden: false, textContent: '', disabled: false,
      classList: { toggle() {}, add() {} }, addEventListener() {},
      elements: { namedItem: key => node(id + ':' + key) },
      querySelector: selector => node(id + ':' + selector),
      querySelectorAll: () => []
    });
    return nodes.get(id);
  };
  let handler = () => new Response('{}');
  const context = vm.createContext({
    document: { hidden: false, getElementById: node, querySelectorAll: () => [],
      addEventListener: (name, callback) => { documentEvents[name] = callback; } },
    window: { addEventListener: (name, callback) => { windowEvents[name] = callback; } },
    location: { hash: '', pathname: '/', search: '' },
    history: { replaceState() {} },
    sessionStorage: { getItem: () => '', setItem(key,value) { storage.push({key,value}); }, removeItem(key) { storage.push({key}); } },
    Blob, Uint8Array, AbortController, Response, TypeError, TextEncoder,
    FormData: class {
      constructor(form) { if(form.testData)return new Map(Object.entries(form.testData));return new Map([['url', form.elements.namedItem('url').value],
        ['token', form.elements.namedItem('token').value]]); }
    },
    fetch: async (url, options) => { calls.push({ url, options }); return handler(url, options); },
    setTimeout: (callback, ms) => {
      if (ms < 1000) queueMicrotask(callback);
      return 1;
    },
    clearTimeout() {},
    setInterval: (callback, ms) => { intervals.push({ callback, ms }); return intervals.length; },
    clearInterval() {}
  });
  vm.runInContext(source, context);
  return { context, nodes, node, calls, intervals, storage, documentEvents, windowEvents,
    handle: callback => { handler = callback; },
    evaluate: code => vm.runInContext(code, context),
    connected: async () => {
      vm.runInContext(`token='${bearer}';`, context);
      await vm.runInContext('connect()', context);
      assert.equal(node('workspace').hidden, false);
      calls.length = 0;
    }
  };
}

test('visible configuration form stays paired through more than 15 minutes of editing', async () => {
  const r = boot();
  await r.connected();
  let now = 0, lastAuth = 0;
  r.node('homeassistant:ha_url').value = 'http://draft.local:8123';
  r.handle((url, options) => {
    assert.equal(url, '/api/session');
    assert.equal(options.headers.Authorization, 'Bearer ' + bearer);
    if (now - lastAuth >= 900000) return new Response('{}', { status: 401 });
    lastAuth = now;
    return new Response('{"ok":true}');
  });
  const heartbeat = r.intervals.find(entry => entry.ms === 60000);
  assert.ok(heartbeat);
  for (let i = 0; i < 20; i++) { now += 60000; await heartbeat.callback(); }
  assert.equal(r.evaluate('token'), bearer);
  assert.equal(r.node('homeassistant:ha_url').value, 'http://draft.local:8123');
  assert.equal(r.calls.length, 20);
});

test('transient read failures recover with bounded retries and keep drafts', async () => {
  const r = boot(); await r.connected();
  r.node('homeassistant:ha_url').value = 'http://draft.local:8123';
  let failures = 2;
  r.handle(() => { if (failures-- > 0) throw new TypeError('network'); return new Response('{}'); });
  await r.evaluate("api('session')");
  assert.equal(r.calls.length, 3);
  assert.equal(r.node('workspace').hidden, false);
  assert.equal(r.node('homeassistant:ha_url').value, 'http://draft.local:8123');
});

test('reconnect verifies the existing session without overwriting edited forms', async () => {
  const r = boot(); await r.connected();
  r.node('homeassistant:ha_url').value = 'http://draft.local:8123';
  r.handle(() => { throw new TypeError('offline'); });
  await assert.rejects(r.evaluate("api('session')"), /已保留填写内容/);
  assert.equal(r.calls.length, 3);
  assert.equal(r.evaluate('token'), bearer);
  r.calls.length = 0;
  r.handle(() => new Response('{}'));
  await r.evaluate('connect()');
  assert.deepEqual(r.calls.map(call => call.url), ['/api/session']);
  assert.equal(r.node('workspace').hidden, false);
  assert.equal(r.node('homeassistant:ha_url').value, 'http://draft.local:8123');
});

test('a lost save reply is reported as uncertain and the POST is sent only once', async () => {
  const r = boot(); await r.connected();
  r.node('homeassistant:ha_token').value = 'draft-secret';
  r.handle(() => { throw new TypeError('reply lost'); });
  await assert.rejects(r.evaluate("api('config?section=homeassistant','POST',{ha_url:'http://new.local:8123'})"), /配置可能已保存/);
  assert.equal(r.calls.length, 1);
  assert.equal(r.calls[0].options.method, 'POST');
  assert.equal(r.node('homeassistant:ha_token').value, 'draft-secret');
  r.handle(() => new Response('{}'));
  await r.evaluate('keepSession()');
  assert.equal(r.node('connection-badge').textContent, '已连接设备');
});

test('expired or revoked authorization is not retried and clears the local session', async () => {
  const r = boot(); await r.connected();
  r.handle(() => new Response('{"error":"连接已失效"}', { status: 401 }));
  await r.evaluate('keepSession()');
  assert.equal(r.calls.length, 1);
  assert.equal(r.evaluate('token'), '');
  assert.equal(r.node('workspace').hidden, true);
});

test('heartbeat skips hidden pages and active requests, then resumes without reloading forms', async () => {
  const r = boot(); await r.connected();
  r.context.document.hidden = true;
  await r.evaluate('keepSession()');
  assert.equal(r.calls.length, 0);
  r.context.document.hidden = false;
  let release;
  r.handle(() => new Promise(resolve => { release = resolve; }));
  const read = r.evaluate("api('config?section=homeassistant')");
  await r.evaluate('keepSession()');
  assert.equal(r.calls.length, 1);
  release(new Response('{}')); await read;
  r.handle(() => new Response('{}'));
  await r.documentEvents.visibilitychange();
  // Event callback schedules an async check; wait for it to finish.
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(r.calls.at(-1).url, '/api/session');
});

test('late responses cannot reconnect a page after an explicit disconnect', async () => {
  const r = boot(); await r.connected();
  r.evaluate('connectionInterrupted=true');
  let release;
  r.handle(() => new Promise(resolve => { release = resolve; }));
  const read = r.evaluate("api('session')");
  r.node('disconnect').onclick();
  release(new Response('{}')); await read;
  assert.equal(r.evaluate('token'), '');
  assert.equal(r.node('workspace').hidden, true);
});

test('a lost code-exchange reply is retried using the same pairing code', async () => {
  const r = boot();
  let first = true;
  r.handle(url => {
    if (url === '/api/pair') {
      if (first) { first = false; throw new TypeError('reply lost'); }
      return new Response(JSON.stringify({ token: bearer }));
    }
    return new Response('{}');
  });
  await r.evaluate("connect('123456')");
  assert.equal(r.node('workspace').hidden, false);
  const exchanges = r.calls.filter(call => call.url === '/api/pair');
  assert.equal(exchanges.length, 2);
  assert.equal(exchanges[0].options.body, exchanges[1].options.body);
});

test('a saved configuration remains reported as saved if its follow-up read fails', async () => {
  const r = boot(); await r.connected();
  r.node('homeassistant:url').value = 'http://new.local:8123';
  r.handle((url, options) => {
    if(options.method === 'POST') return new Response('{"ok":true}');
    throw new TypeError('read failed');
  });
  r.node('homeassistant').onsubmit({ preventDefault() {}, target: r.node('homeassistant') });
  await new Promise(resolve => setImmediate(resolve));
  assert.match(r.node('status').textContent, /^配置已保存/);
  assert.equal(r.calls.filter(call => call.options.method === 'POST').length, 1);
  assert.equal(r.node('homeassistant:button').disabled, false);
});

test('a failed save keeps password drafts and re-enables the save button', async () => {
  const r = boot(); await r.connected();
  r.node('homeassistant:url').value = 'http://new.local:8123';
  r.node('homeassistant:token').value = 'draft-secret';
  r.handle(() => { throw new TypeError('save reply lost'); });
  r.node('homeassistant').onsubmit({ preventDefault() {}, target: r.node('homeassistant') });
  await new Promise(resolve => setImmediate(resolve));
  assert.match(r.node('status').textContent, /配置可能已保存/);
  assert.equal(r.calls.length, 1);
  assert.equal(r.node('homeassistant:token').value, 'draft-secret');
  assert.equal(r.node('homeassistant:button').disabled, false);
});

test('HA slash-port input is rejected before POST and preserves the token draft', async () => {
  const r=boot();await r.connected();
  r.node('homeassistant:url').value='http://124.223.30.246/8123';
  r.node('homeassistant:token').value='draft-secret';
  r.node('homeassistant').onsubmit({preventDefault(){},target:r.node('homeassistant')});
  await new Promise(resolve=>setImmediate(resolve));
  assert.equal(r.calls.length,0);
  assert.match(r.node('status').textContent,/端口前用冒号/);
  assert.equal(r.node('homeassistant:token').value,'draft-secret');
  assert.equal(r.node('homeassistant:button').disabled,false);
});

test('HA colon-port save trims copied credentials and a blank token preserves the stored one', async () => {
  const r=boot();await r.connected();r.node('homeassistant:url').value=' http://124.223.30.246:8123 ';
  r.node('homeassistant:token').value='  copied-token\n';
  r.handle(()=>new Response('{}'));
  r.node('homeassistant').onsubmit({preventDefault(){},target:r.node('homeassistant')});
  await new Promise(resolve=>setImmediate(resolve));
  let post=r.calls.find(c=>c.options.method==='POST');
  assert.deepEqual(JSON.parse(post.options.body),{url:'http://124.223.30.246:8123',token:'copied-token'});
  r.calls.length=0;r.node('homeassistant:token').value='';
  r.node('homeassistant').onsubmit({preventDefault(){},target:r.node('homeassistant')});
  await new Promise(resolve=>setImmediate(resolve));
  post=r.calls.find(c=>c.options.method==='POST');assert.equal('token' in JSON.parse(post.options.body),false);
});

for (const storage of ['internal','sdcard']) test('installer submits selected '+storage+' volume independently of package metadata',async()=>{
  const r=boot();await r.connected();r.node('install-storage').value=storage;
  r.evaluate("pack={manifest:{name:'Test',package:'test.app',storage:'untrusted'},files:{'manifest.json':new Uint8Array([1]),'app.js':new Uint8Array([2,3])}};");
  r.handle(()=>new Response('{}'));await r.node('install-button').onclick();
  const begin=r.calls.find(c=>c.url==='/api/install/begin');assert.ok(begin);
  assert.equal(JSON.parse(begin.options.body).storage,storage);
  assert.ok(r.calls.some(c=>c.url==='/api/install/commit'));
  assert.match(r.node('install-status').textContent,storage==='sdcard'?/已安装到SD 卡/:/已安装到内置存储/);
  assert.equal(r.evaluate('busy'),false);
});
test('failed SD installation aborts staging and never retries on internal storage',async()=>{
  const r=boot();await r.connected();r.node('install-storage').value='sdcard';
  r.evaluate("pack={manifest:{name:'Test',package:'test.app'},files:{'app.js':new Uint8Array([2,3])}};");
  r.handle(url=>url.startsWith('/api/install/file')?new Response('{"error":"card removed"}',{status:400}):new Response('{}'));
  await r.node('install-button').onclick();
  assert.equal(r.calls.filter(c=>c.url==='/api/install/begin').length,1);
  assert.equal(JSON.parse(r.calls.find(c=>c.url==='/api/install/begin').options.body).storage,'sdcard');
  assert.ok(r.calls.some(c=>c.url==='/api/install/abort'));
  assert.ok(!r.calls.some(c=>c.url==='/api/install/commit'));
  assert.equal(r.node('install-button').disabled,false);assert.equal(r.evaluate('busy'),false);
});
test('compiled portal assets match the current installer files byte for byte',()=>{
  const root=path.join(__dirname,'../overlay/apps/system/desktop');const c=fs.readFileSync(path.join(root,'glass_portal_resource.c'),'utf8');
  ['index.html','style.css','app.js'].forEach((name,i)=>{
    const array=c.match(new RegExp('asset'+i+'\\[\\]\\=\\{([^}]*)\\}'));
    assert.ok(array);assert.deepEqual(Buffer.from(array[1].split(',').map(Number)),fs.readFileSync(path.join(root,'portal',name)));
  });
});

test('Agent save keeps blank MCP secrets, converts switches and sends explicit token clear', async () => {
  const r=boot();await r.connected();
  r.node('agent').testData={skills_enabled:'1',home_control_enabled:'1',mcp1_enabled:'1',mcp1_url:'https://mcp.example/mcp',mcp1_token:'',mcp2_token:'',mcp3_token:'',mcp2_clear_token:'on'};
  let posted;
  r.handle((url,options)=>{if(options.method==='POST'){assert.equal(url,'/api/config?section=agent');posted=JSON.parse(options.body);}return new Response('{}');});
  await r.node('agent').onsubmit({preventDefault(){},target:r.node('agent')});
  await new Promise(resolve=>setImmediate(resolve));
  assert.equal(posted.skills_enabled,1);assert.equal(posted.mcp1_enabled,1);
  assert.equal(posted.home_control_enabled,1);
  assert.equal('mcp1_token' in posted,false);assert.equal(posted.mcp2_token,'');assert.equal('mcp3_token' in posted,false);
  assert.equal('mcp2_clear_token' in posted,false);
});

test('Agent reports saved MCP tokens accurately when later slots are empty', async () => {
  const r=boot();await r.connected();r.handle(()=>new Response(JSON.stringify({mcp1_token_set:true,mcp2_token_set:false,mcp3_token_set:false})));
  await r.evaluate("loadConfig('agent')");assert.match(r.node('agent:.saved').textContent,/1.*MCP/);
});

const networkState = patch => ({ok:true,busy:false,operation:0,error:0,
  associated:false,ipv4_ready:false,ipv4:'',ap_running:true,ap_state:2,
  ap_error:0,ap_ssid:'ESPClaw-Test',ap_ip:'192.168.240.1',...patch});
const routerDraft = (r,password='test-password') => {
  r.node('network-ssid').value='  路由器  ';
  r.node('network-password').value=password;
};

test('network status is independent and an unavailable API does not block configuration loading',async()=>{
  const r=boot();
  r.handle(url=>{if(url==='/api/network')throw new TypeError('offline');return new Response('{}');});
  await r.connected();
  r.node('ai:model').value='unsaved-model';
  await r.evaluate('refreshNetwork(true)');
  assert.equal(r.calls.length,3);
  assert.ok(r.calls.every(call=>call.url==='/api/network'));
  assert.equal(r.node('workspace').hidden,false);
  assert.equal(r.node('ai:model').value,'unsaved-model');
  assert.match(r.node('network-status').textContent,/无法读取网络状态/);
  assert.equal(r.evaluate('loadedToken'),bearer);
});

test('network acceptance waits for actual association, IP and persistence before reporting success',async()=>{
  const r=boot();await r.connected();routerDraft(r);
  r.handle(()=>new Response('{"ok":true}'));
  await r.evaluate('submitNetwork()');
  assert.equal(r.calls.length,1);
  assert.equal(r.calls[0].url,'/api/network/connect');
  assert.equal(r.calls[0].options.headers.Authorization,'Bearer '+bearer);
  assert.deepEqual(JSON.parse(r.calls[0].options.body),{ssid:'  路由器  ',password:'test-password'});
  assert.equal(r.node('network-password').value,'');
  assert.match(r.node('network-status').textContent,/已受理/);
  assert.doesNotMatch(r.node('network-status').textContent,/连接成功/);
  assert.equal(r.node('network-connect').disabled,true);
  const poll=r.intervals.find(entry=>entry.ms===2000);assert.ok(poll);
  let state=networkState({busy:true});
  r.handle((url,options)=>{assert.equal(url,'/api/network');assert.equal(options.method,'GET');return new Response(JSON.stringify(state));});
  await poll.callback();assert.match(r.node('network-status').textContent,/尝试连接/);
  state=networkState({busy:true,associated:true});
  await poll.callback();assert.match(r.node('network-status').textContent,/获取 IP 并确认保存/);
  assert.doesNotMatch(r.node('network-status').textContent,/连接成功/);
  state=networkState({associated:true,ipv4_ready:true,ipv4:'192.168.8.22'});
  await poll.callback();assert.match(r.node('network-status').textContent,/连接成功.*192\.168\.8\.22/);
  assert.match(r.node('network-status').textContent,/保持开启/);
  assert.match(r.node('network-ap').textContent,/ESPClaw-Test.*192\.168\.240\.1/);
  assert.equal(r.node('network-connect').disabled,false);
  const count=r.calls.length;await poll.callback();assert.equal(r.calls.length,count);
});

test('the hotspot address and malformed station addresses cannot pass network acceptance',async()=>{
  const r=boot();await r.connected();
  for(const state of [networkState(),networkState({associated:true,ipv4_ready:false}),
      networkState({associated:true,ipv4_ready:true,ipv4:'999.1.2.3'}),
      networkState({associated:true,ipv4_ready:true,ipv4:'0.0.0.0'})]){
    r.handle(()=>new Response(JSON.stringify(state)));await r.evaluate('refreshNetwork(true)');
    assert.doesNotMatch(r.node('network-status').textContent,/连接成功/);
    assert.equal(r.evaluate('networkWatching'),true);
  }
});

test('router credentials use UTF-8 SSID byte limits and printable ASCII WPA key validation',async()=>{
  const r=boot();await r.connected();
  const validate=(ssid,password)=>r.evaluate('validateNetwork('+JSON.stringify(ssid)+','+JSON.stringify(password)+')');
  for(const password of ['', 'a'.repeat(8),' '.repeat(63),'Ab09'.repeat(16)])
    assert.doesNotThrow(()=>validate('网'.repeat(10)+'ab',password));
  for(const ssid of ['', '网'.repeat(11),'a'.repeat(33),'bad\nname','bad\0name','bad\x7fname'])
    assert.throws(()=>validate(ssid,''),/网络名称/);
  for(const password of ['1234567','g'.repeat(64),'a'.repeat(65),'含中文密码123','abc\ndefgh'])
    assert.throws(()=>validate('router',password),/密码须为/);
  routerDraft(r,'short');await r.evaluate('submitNetwork()');
  assert.equal(r.calls.length,0);assert.equal(r.node('network-password').value,'');
  routerDraft(r,'');r.handle(()=>new Response('{"ok":true}'));await r.evaluate('submitNetwork()');
  assert.equal(JSON.parse(r.calls[0].options.body).password,'');
});

test('association and IP-or-save failures are visible and clear the password',async()=>{
  const r=boot();await r.connected();
  for(const associated of [false,true]){
    routerDraft(r);
    r.handle(()=>new Response(JSON.stringify(networkState({error:-5,associated}))));
    await r.evaluate('refreshNetwork(true)');
    assert.equal(r.node('network-password').value,'');
    assert.equal(r.node('network-connect').disabled,false);
    assert.match(r.node('network-status').textContent,associated?/获取 IP 或保存新配置失败/:/连接路由器失败/);
    assert.doesNotMatch(r.node('network-status').textContent,/路由器连接成功/);
  }
});

test('a lost network POST reply is never replayed and an old ready link stays explicitly uncertain',async()=>{
  const r=boot();await r.connected();routerDraft(r);
  r.handle(()=>{throw new TypeError('reply lost');});await r.evaluate('submitNetwork()');
  assert.equal(r.calls.length,1);assert.equal(r.calls[0].options.method,'POST');
  assert.equal(r.node('network-password').value,'');
  assert.match(r.node('network-status').textContent,/回执未收到/);
  assert.equal(r.node('network-connect').disabled,true);
  r.handle(()=>new Response(JSON.stringify(networkState({associated:true,ipv4_ready:true,ipv4:'192.168.8.22'}))));
  await r.evaluate('refreshNetwork(true)');
  assert.match(r.node('network-status').textContent,/当前路由器已连接.*本次请求回执未收到/);
  assert.doesNotMatch(r.node('network-status').textContent,/路由器连接成功/);
  assert.equal(r.calls.filter(call=>call.options.method==='POST').length,1);
});

test('duplicate network actions are blocked while submitting and awaiting the first status',async()=>{
  const r=boot();await r.connected();routerDraft(r);
  let release;r.handle(()=>new Promise(resolve=>{release=resolve;}));
  const pending=r.evaluate('submitNetwork()');
  await r.evaluate('submitNetwork()');assert.equal(r.calls.length,1);
  release(new Response('{"ok":true}'));await pending;
  await r.evaluate('submitNetwork()');assert.equal(r.calls.length,1);
  assert.equal(r.node('network-connect').disabled,true);
});

test('a stale network read cannot overwrite a newer connection request',async()=>{
  const r=boot();await r.connected();routerDraft(r);
  let release;r.handle((url,options)=>options.method==='GET'?
    new Promise(resolve=>{release=resolve;}):new Response('{"ok":true}'));
  const read=r.evaluate('refreshNetwork(true)');
  await r.evaluate('submitNetwork()');
  release(new Response(JSON.stringify(networkState({associated:true,ipv4_ready:true,ipv4:'192.168.8.22'}))));
  await read;
  assert.match(r.node('network-status').textContent,/已受理/);
  assert.doesNotMatch(r.node('network-status').textContent,/连接成功/);
  assert.equal(r.evaluate('networkWatching'),true);
  assert.equal(r.node('network-connect').disabled,true);
});

test('network credentials are never stored and are cleared on disconnect and page exit',async()=>{
  const r=boot();await r.connected();routerDraft(r);
  r.windowEvents.pagehide();assert.equal(r.node('network-password').value,'');
  routerDraft(r);let release;r.handle(()=>new Promise(resolve=>{release=resolve;}));
  const pending=r.evaluate('submitNetwork()');r.node('disconnect').onclick();
  assert.equal(r.node('network-password').value,'');
  release(new Response('{"ok":true}'));await pending;
  assert.equal(r.evaluate('token'),'');
  assert.equal(r.node('workspace').hidden,true);
  assert.doesNotMatch(r.node('network-status').textContent,/已受理/);
  assert.ok(r.storage.every(entry=>entry.key==='device-space-session-v1'));
  assert.ok(r.storage.every(entry=>!JSON.stringify(entry).includes('test-password')));
});

test('expired network authorization clears pairing and prevents further polling',async()=>{
  const r=boot();await r.connected();routerDraft(r);
  r.handle(()=>new Response('{"error":"连接已失效"}',{status:401}));
  await r.evaluate('refreshNetwork(true)');
  assert.equal(r.calls.length,1);assert.equal(r.evaluate('token'),'');
  assert.equal(r.node('network-password').value,'');
  assert.equal(r.node('workspace').hidden,true);
  await r.intervals.find(entry=>entry.ms===2000).callback();assert.equal(r.calls.length,1);
});
