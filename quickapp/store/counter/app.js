/* SPDX-License-Identifier: Apache-2.0 */
ui.sidebar([]);
const size = ui.getSize(), left = Math.round((size.width - 600) / 2);
ui.text('随手计数', left, 40, 600, 28, ui.primary);
let count = 0;
try { const v = Number(system.storage.get('count')); if (Number.isSafeInteger(v) && Math.abs(v) <= 999999) count = v; } catch (_) {}
const output = ui.text(String(count), left, 132, 600, 48, ui.primary);
const hint = ui.text('点击保存后，退出重开仍保留', left, 220, 600, 20, ui.secondary);
function change(delta) {
  count = Math.max(-999999, Math.min(999999, count + delta));
  ui.setText(output, String(count)); ui.setText(hint, '尚未保存');
}
ui.button('－', left, 294, 286, 64, () => change(-1), ui.surface);
ui.button('＋', left + 310, 294, 290, 64, () => change(1), ui.accent);
ui.button('归零', left, 386, 286, 52, () => { count = 0; change(0); }, ui.surface);
ui.button('保存计数', left + 310, 386, 290, 52, () => {
  try { system.storage.set('count', String(count)); ui.setText(hint, '已保存'); }
  catch (_) { ui.setText(hint, '保存失败，当前计数仍保留在本次会话'); }
}, ui.accent);
