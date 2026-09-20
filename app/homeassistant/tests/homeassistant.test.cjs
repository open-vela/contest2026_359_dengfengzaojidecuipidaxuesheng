const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const appRoot = path.join(__dirname, '..');
const publishRoot = path.join(appRoot, '..', '..');
const system = path.join(publishRoot, 'app', 'espdl-quickapp', 'overlay',
  'apps', 'system');
const read = (...parts) => fs.readFileSync(path.join(system, ...parts), 'utf8');

test('native service accepts HTTP hosts and replaces active configuration', () => {
  const service = read('hass', 'hass_service.c');
  const transport = read('hass', 'hass_transport.c');

  assert.match(service, /static bool http_url/);
  assert.doesNotMatch(service, /static bool local_url|inet_pton|\.local/);
  assert.match(service, /HASS_CACHE_TTL_MS 10000/);
  assert.match(service, /"light".*"switch".*"input_boolean".*"fan"/s);
  assert.match(service, /wire\.status == 401 \|\| wire\.status == 403/);
  assert.match(service, /if \(owner\) hass_transport_stop\(\)/);
  assert.match(service, /-ECANCELED/);
  assert.match(transport, /HA_RESPONSE_MAX 65536/);
  assert.match(transport, /PTHREAD_CREATE_DETACHED/);
});

test('portal keeps an omitted token and applies Home Assistant saves', () => {
  const portal = read('desktop', 'portal', 'app.js');
  const http = read('desktop', 'glass_portal_http.c');

  assert.match(portal, /if\(data\[key\]===''\)delete data\[key\]/);
  assert.match(portal, /智能家居配置已保存并应用/);
  const save = http.indexOf('portal_config_save(arg,o)');
  const reload = http.indexOf('hass_portal_bootstrap()', save);
  assert.ok(save >= 0 && reload > save);
});

test('native page paginates, confirms controls, and has no legacy advanced UI', () => {
  const nativeUi = read('desktop', 'glass_hass_ui.inc');

  assert.match(nativeUi, /HASS_UI_VISIBLE 6/);
  assert.match(nativeUi, /HASS_UI_MAX_DEVICES 128/);
  assert.match(nativeUi, /HASS_UI_REQUEST_VERIFY/);
  assert.match(nativeUi, /HASS_UI_VERIFY_ATTEMPTS 4/);
  assert.match(nativeUi, /HASS_UI_VERIFY_WINDOW_MS 8000/);
  assert.match(nativeUi, /HASS_UI_REFRESH_INTERVAL_MS 10000/);
  assert.match(nativeUi, /hass_get_state\(g_hass_ui\.client,/);
  assert.match(nativeUi, /abs\(level - g_hass_ui\.pending_brightness\) <= 1/);
  assert.match(nativeUi, /g_hass_ui\.page \* HASS_UI_VISIBLE/);
  assert.match(nativeUi, /"alarm_control_panel"/);
  assert.match(nativeUi, /"weather"/);
  assert.match(nativeUi, /状态超过 64 KiB，请在 HA 中减少公开实体/);
  assert.doesNotMatch(nativeUi, /"高级"/);
  assert.ok((nativeUi.match(/device->brightness >= 0/g) || []).length >= 2);
  assert.doesNotMatch(nativeUi, /device->on = g_hass_ui\.pending_on/);
});

test('desktop opens the reviewed app.js UI on the shared native service', () => {
  const makefile = read('desktop', 'Makefile');
  const desktop = read('desktop', 'desktop_main.c');
  const glass = read('desktop', 'glass_ui.inc');
  const runtime = read('desktop', 'qpk_runtime.c');
  const ui = fs.readFileSync(path.join(publishRoot, 'quickapp',
    'homeassistant', 'app.js'), 'utf8');
  const resource = fs.readFileSync(path.join(system, 'desktop',
    'homeassistant_resource.c'));
  const generated = fs.readFileSync(path.join(appRoot,
    'homeassistant_resource.c'));

  assert.match(makefile, /camera_resource\.c/);
  assert.match(makefile, /recorder_resource\.c/);
  assert.match(makefile, /homeassistant_resource\.c/);
  assert.deepEqual(resource, generated);
  assert.match(desktop, /static const struct builtin_qpk_s g_builtin_homeassistant_qpk/);
  assert.match(desktop, /qpk_runtime_launch\(card, manifest->name, manifest->package,/);
  assert.match(glass, /static void glass_homeassistant[\s\S]*launch_builtin_homeassistant_qapp\(e\)/);
  assert.match(glass, /#include "glass_hass_ui\.inc"/);

  assert.match(runtime, /hass_qjs_install\(context, system, g_qpk\.hass_grants\)/);
  assert.match(ui, /const nativeService = system\.homeAssistantService/);
  assert.match(ui, /function sharedAdapter\(\)/);
  assert.match(ui, /VERIFY_ATTEMPTS = 4/);
  assert.match(ui, /VERIFY_WINDOW = 8000/);
  assert.match(ui, /nativeService\.getState\(entity\)/);
  assert.match(ui, /控制已受理，正在核对状态/);
});
