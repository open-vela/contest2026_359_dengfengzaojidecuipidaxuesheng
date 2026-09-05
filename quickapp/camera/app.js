'use strict';

const view = ui.getSize();
const previewX = Math.max(0, Math.floor((view.width - 512) / 2));
const previewY = 8;

ui.background(0x0b0f10);
const status = ui.text('正在启动摄像头…', previewX, 316, 18, 0xa9bad1);
let running = false;
let mode = 'stopped';

function updateStatus() {
  const frames = system.camera.frames();
  ui.setText(status, running ? '实时取景 · 已显示 ' + frames + ' 帧' : '预览已停止');
}

function startCamera() {
  if (running) return;
  if (running) system.camera.stop();
  running = system.camera.start(previewX, previewY);
  mode = running ? 'camera' : 'stopped';
  updateStatus();
  if (!running) ui.setText(status, '摄像头启动失败，请重试');
}

function stopCamera() {
  if (!running) return;
  system.camera.stop();
  running = false;
  mode = 'stopped';
  ui.setText(status, '预览已停止');
}

startCamera();

/* Create controls after the native canvas so they stay above the preview. */
ui.button('取景 / 重试', previewX, 350, 140, 44,
          startCamera, 0x168a72);
ui.button('拍照', previewX + 152, 350, 100, 44,
          () => prompt.showToast('拍照接口尚未接入'), 0x4f6fa8);
ui.button('退出', previewX + 260, 350, 100, 44,
          stopCamera, 0x965f45);

setInterval(updateStatus, 500);

globalThis.CameraPreview = {
  start: startCamera,
  stop: stopCamera,
  frames: () => system.camera.frames()
};
