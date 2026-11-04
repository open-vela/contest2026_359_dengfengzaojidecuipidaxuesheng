/* SPDX-License-Identifier: Apache-2.0 */
ui.sidebar([]);
const size = ui.getSize(), left = Math.round((size.width - 560) / 2);
ui.text('轻量计算器', left, 24, 560, 28, ui.primary);
const display = ui.text('0', left, 84, 560, 40, ui.primary);
ui.setSize(display, 560, 64);
let input = '0', saved = null, op = null, fresh = true;
function show() { ui.setText(display, input); }
function calculate() {
  if (saved === null || !op) return;
  const n = Number(input);
  let value = op === '+' ? saved + n : op === '-' ? saved - n : op === '×' ? saved * n : saved / n;
  input = Number.isFinite(value) ? String(Number(value.toPrecision(12))) : '无法计算';
  saved = null; op = null; fresh = true;
}
function press(key) {
  if (key === 'C') { input = '0'; saved = op = null; fresh = true; }
  else if (key === '±') { if (Number.isFinite(Number(input))) input = String(-Number(input)); }
  else if (key === '退格') { input = input.length > 1 ? input.slice(0, -1) : '0'; }
  else if (['+', '-', '×', '÷'].includes(key)) {
    if (op && !fresh) calculate();
    if (Number.isFinite(Number(input))) { saved = Number(input); op = key; fresh = true; }
  } else if (key === '=') calculate();
  else {
    if (fresh || !Number.isFinite(Number(input))) { input = key === '.' ? '0.' : key; fresh = false; }
    else if (input.length < 14 && (key !== '.' || !input.includes('.'))) input = input === '0' && key !== '.' ? key : input + key;
  }
  show();
}
const keys = ['C', '±', '退格', '÷', '7', '8', '9', '×', '4', '5', '6', '-', '1', '2', '3', '+', '0', '.', '=', '='];
keys.forEach((key, i) => {
  if (i === 19) return;
  ui.button(key, left + (i % 4) * 144, 174 + Math.floor(i / 4) * 58, i === 18 ? 272 : 128, 48, () => press(key), i === 18 ? ui.accent : ui.surface);
});
