/* SPDX-License-Identifier: Apache-2.0 */
const vm = require('node:vm'), fs = require('node:fs'), assert = require('node:assert/strict');
const source = fs.readFileSync(process.argv[2], 'utf8');
function launch() {
  const widgets = [], jobs = new Map(), submitted = [], stopped = [], cancelled = [];
  let timer, back, sequence = 0;
  function widget(label, x, y, width, height, callback) {
    widgets.push({label, x, y, width, height, callback, style: {}}); return widgets.length - 1;
  }
  const ui = {getSize: () => ({width: 1022, height: 536}), card: 0xf5f6fd, surface: 0xe9eafa,
    primary: 0x252d46, secondary: 0x616981, accent: 0x5860bf, background() {},
    panel: (x,y,w,h) => widget('',x,y,w,h), rect: (x,y,w,h) => widget('',x,y,w,h),
    text: (label,x,y) => widget(label,x,y), number: widget, button: widget,
    setText: (id,label) => widgets[id].label = label,
    setPos: (id,x,y) => Object.assign(widgets[id], {x,y}),
    setSize: (id,width,height) => Object.assign(widgets[id], {width,height}),
    setStyle: (id,style) => Object.assign(widgets[id].style, style),
    setHidden: (id,hidden) => widgets[id].hidden = hidden, setColor() {},onBack:fn=>back=fn};
  const audio = {};
  for (const op of ['list','record','play','remove']) audio[op] = args => {
    const id = ++sequence; jobs.set(id, {done:false, state:'running'});
    submitted.push({id,op,args}); return id;
  };
  audio.stop = id => { assert(jobs.has(id)); stopped.push(id); };
  const context = {ui, system: {audio, hardware: {
    poll(id) { assert(jobs.has(id), 'completed results must be consumed once'); const s = jobs.get(id); if (s.done) jobs.delete(id); return s; },
    cancel(id) { assert(jobs.has(id)); cancelled.push(id); }
  }}, setInterval: callback => { timer = callback; return 1; }};
  vm.createContext(context); vm.runInContext(source, context);
  const app = context.RecorderApp;
  const last = () => submitted.at(-1);
  const done = (ok, result = {}, error = 0) => {
    jobs.set(app.state().task, {done: true, ok, result, error, elapsedMs: 200}); timer();
  };
  return {app, widgets, submitted, stopped, cancelled, jobs, timer: () => timer(), last, done,back:()=>back()};
}
const a = launch();
assert.equal(a.last().op, 'list'); assert.equal(a.submitted.length, 1, 'opening must never record');
a.app.record(); assert.equal(a.submitted.length, 1);
a.done(true, {clips: []});
a.app.record(); const first = a.last(); assert.equal(first.op, 'record'); assert.equal(first.args.durationMs, 0);
a.app.record(); a.app.play(); assert.equal(a.submitted.length, 2, 'double taps cannot start more work');
a.jobs.set(first.id, {done:false, elapsedMs:200, bytes:6400, peak:16384}); a.timer();
assert.equal(a.app.state().elapsed, 200);
assert(a.widgets.some(w => w.y > 240 && w.y < 280 && w.height > 4));
a.app.stop(); assert.equal(a.stopped[0], first.id); assert(a.app.state().stopping);
a.app.record(); assert.equal(a.last().id, first.id, 'wait for actual stop completion');
a.done(true, {bytes:6400}); assert.equal(a.last().op, 'list');
const clip = {clip:first.args.clip, valid:true, fileBytes:6444, durationMs:200};
a.done(true, {clips:[clip]}); assert.equal(a.app.state().selected, clip.clip);
a.app.play(); assert.equal(a.last().args.clip, clip.clip); a.app.stop(); a.done(true);
a.app.record(); assert.notEqual(a.last().args.clip, clip.clip);
a.app.cancel(); assert.equal(a.cancelled.at(-1), a.last().id);
a.done(false, {}, -125); a.done(true, {clips:[clip]});
assert.equal(a.app.state().count, 1); assert(a.widgets.some(w => w.label === '已放弃本次录音'));
const beforeDelete = a.submitted.length;
a.app.remove(); assert.equal(a.submitted.length, beforeDelete, 'delete requires an explicit second press');
assert.equal(a.back(),true);assert.equal(a.submitted.length,beforeDelete,'back cancels confirmation without deleting');
assert(a.widgets.some(w=>w.label==='删除'));assert.equal(a.back(),false);
a.app.remove();assert.equal(a.submitted.length,beforeDelete,'deletion needs fresh confirmation after back');
a.app.remove(); assert.equal(a.last().op, 'remove'); a.done(true); a.done(true, {clips:[]});
assert.equal(a.app.state().count, 0);

const b = launch();
b.done(false, {}, -5); b.app.record(); assert.equal(b.last().op, 'list', 'failed list cannot overwrite an unknown existing clip');
const existing = Array.from({length:9}, (_,i) => ({clip:'rec_' + String(i + 91).padStart(6,'0'), valid:true, fileBytes:32044, durationMs:1000}));
b.done(true, {clips:existing}); assert.equal(b.app.state().count, 9);
const target=b.app.state().pager.find(s=>s.page===1);assert(target);b.widgets[target.button].callback();assert.equal(b.app.state().page,1);
assert(b.widgets.some(w=>!w.hidden&&w.width===20&&w.height===6));
b.app.select(4); b.app.record(); assert.equal(b.last().args.clip, 'rec_000100');
b.done(false, {}, -28); b.done(true, {clips:existing});
assert.equal(b.app.state().count, 9); assert(b.widgets.some(w => w.label.includes('存储空间不足')));
for (const w of b.widgets) if (!w.hidden && Number.isFinite(w.width) && Number.isFinite(w.height)) {
  assert(w.x >= 0 && w.y >= 0 && w.x + w.width <= 1022 && w.y + w.height <= 536, 'widget outside content area');
}
const legacy=launch();
legacy.done(true,{clips:[{clip:'flash:rec_000001',readOnly:true,valid:true,fileBytes:6444,durationMs:200}]});
let legacyCalls=legacy.submitted.length;
legacy.app.remove();assert.equal(legacy.submitted.length,legacyCalls,'legacy Flash recordings remain read-only');
legacy.app.play();assert.equal(legacy.last().args.clip,'flash:rec_000001');legacy.done(true);
legacy.app.record();legacy.done(false,{},-19);legacy.done(true,{clips:[]});
assert(legacy.widgets.some(w=>w.label.includes('SD 卡')),'missing SD explains why recording cannot start');
const result = {result: 'PASS: explicit recording, real progress, stop/cancel completion, playback, confirmed deletion, durable paging, list-failure recovery, full-storage errors, read-only legacy clips and missing SD',
  source_sha256: require('node:crypto').createHash('sha256').update(source).digest('hex'), physical_io:'mocked'};
console.log(result.result);
fs.writeFileSync(require('node:path').join(__dirname, '../evidence/recorder-js-validation.json'), JSON.stringify(result,null,2) + '\n');
