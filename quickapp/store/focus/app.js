/* SPDX-License-Identifier: Apache-2.0 */
ui.sidebar([]);
const size = ui.getSize(), left = Math.round((size.width - 600) / 2);
ui.text('留一段时间给自己', left, 40, 600, 28, ui.primary);
ui.text('专注计时 · 退出应用后停止', left, 86, 600, 20, ui.secondary);
let remaining = 25 * 60 * 1000, deadline = 0, running = false, timer = null, painted = '';
const clock = ui.text('', left, 166, 600, 48, ui.primary);
const state = ui.text('准备好就开始', left, 252, 600, 20, ui.secondary);
let toggle;
function render() {
  if (running) remaining = Math.max(0, deadline - Date.now());
  if (running && remaining === 0) {
    running = false; clearInterval(timer); timer = null;
    ui.setText(toggle, '开始'); ui.setText(state, '本次专注已完成');
    prompt.showToast('专注时间到，休息一下吧');
  }
  const seconds = Math.ceil(remaining / 1000), value = String(Math.floor(seconds / 60)).padStart(2, '0') + ':' + String(seconds % 60).padStart(2, '0');
  if (value !== painted && (!painted || ui.isVisible())) { ui.setText(clock, value); painted = value; }
}
function reset(minutes) {
  if (timer !== null) clearInterval(timer);
  timer = null; running = false; remaining = minutes * 60 * 1000;
  ui.setText(toggle, '开始'); ui.setText(state, '准备好就开始'); render();
}
toggle = ui.button('开始', left, 330, 286, 52, () => {
  if (running) { remaining = Math.max(0, deadline - Date.now()); running = false; clearInterval(timer); timer = null; }
  else { if (!remaining) remaining = 25 * 60 * 1000; deadline = Date.now() + remaining; running = true; timer = setInterval(render, 250); }
  ui.setText(toggle, running ? '暂停' : '继续'); ui.setText(state, running ? '专注进行中' : '已暂停'); render();
}, ui.accent);
ui.button('重置 25 分钟', left + 310, 330, 290, 52, () => reset(25), ui.surface);
ui.button('休息 5 分钟', left, 402, 600, 48, () => reset(5), ui.surface);
render();
