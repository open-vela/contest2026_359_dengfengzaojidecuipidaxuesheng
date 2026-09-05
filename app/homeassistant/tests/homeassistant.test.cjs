const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const appRoot = path.join(__dirname, '..');
const qa = path.join(__dirname, '..', '..', '..', 'quickapp', 'homeassistant');

test('home assistant uses the lovelace palette from ref-db', () => {
  const src = fs.readFileSync(path.join(qa, 'app.js'), 'utf8');
  const db = JSON.parse(fs.readFileSync(path.join(qa, 'ref-db.json'), 'utf8'));
  assert.equal(db.compose.shell, 'lovelace');
  assert.equal(db.compose.tiles, 'hui-tile-card');
  assert.match(src, /HUB_BG = 0xfafafa/);
  assert.match(src, /HUB_ACCENT = 0x009ac7/);
  assert.match(src, /开启/);
  assert.match(src, /paintClock/);
  assert.match(src, /PAGE_SIZE = 4/);
  assert.match(src, /COLS = 2/);
  assert.doesNotMatch(src, /PAPER_BG/);
});

test('preview stays a quiet lovelace home', () => {
  const html = fs.readFileSync(path.join(qa, 'preview.html'), 'utf8');
  assert.match(html, /开启/);
  assert.match(html, /#009ac7/);
  assert.match(html, /#fafafa/);
  assert.match(html, /class="badge"/);
  assert.match(html, /className = 'tile/);
  assert.match(html, /ha_slots/);
  assert.match(html, /注册 ID/);
  assert.match(html, /entity_id/);
  assert.doesNotMatch(html, /data-tab=/);
  assert.doesNotMatch(html, /id="fab"/);
});

test('device app registers entity ids into slots', () => {
  const src = fs.readFileSync(path.join(qa, 'app.js'), 'utf8');
  assert.match(src, /ha_slots/);
  assert.match(src, /function registerId/);
  assert.match(src, /function applySlots/);
  assert.match(src, /function validEntityId/);
  assert.match(src, /function getOrCreate/);
  assert.match(src, /unique_id/);
  assert.match(src, /writeUnavailable/);
  assert.match(src, /ha_deleted/);
  assert.match(src, /function pushDeleted/);
  assert.match(src, /pending = 'stream'/);
  assert.match(src, /isEnergyItem/);
  assert.doesNotMatch(src, /dev\./);
});

test('preview registry follows home assistant core entity registry', () => {
  const html = fs.readFileSync(path.join(qa, 'preview.html'), 'utf8');
  const db = JSON.parse(fs.readFileSync(path.join(qa, 'ref-db.json'), 'utf8'));
  assert.equal(db.homeassistant.core.identity[0], 'domain');
  assert.match(html, /function validEntityId/);
  assert.match(html, /function getOrCreate/);
  assert.match(html, /unique_id/);
  assert.match(html, /write_unavailable_state|unavailable/);
  assert.match(html, /ha_deleted/);
  assert.match(html, /function pushDeleted/);
  assert.match(html, /sensor.electric_meter_power/);
  assert.match(html, /能源/);
  assert.doesNotMatch(html, /dev\./);
});

test('c client stays in the contest app tree', () => {
  const src = fs.readFileSync(path.join(appRoot, 'qpk_homeassistant.c'), 'utf8');
  assert.match(src, /qpk_ha_get/);
  assert.match(src, /\/api\/states/);
});
