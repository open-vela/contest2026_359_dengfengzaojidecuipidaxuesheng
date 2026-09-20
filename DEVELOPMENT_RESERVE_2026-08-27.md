# ESP32-P4 OpenVela 本地开发余量交付清单

整理日期：2026-08-27；对应正式分支：`codex/esp32p4-lcd-bringup`。

## 定位

本清单保存不改变当前 release 判定、但能支持后续排障和验收的本地开发资产。正式 v1.0 发布固件归档在 `firmware/历史测试固件/esp32p4-desktop-v1.0-release`；当前浏览入口是 `firmware/v3`。2026-08-27 的桌面候选仍按其原始边界记录，2026-09-19 的实际 v3 应用实板验证另见 `firmware/v3/esp32p4-application-v3-20260919.md`。

## 本次纳入

| 文件 | 用途 | 状态 |
| --- | --- | --- |
| `tools/fb_dump.py` | 经 UART/NSH 从 `/dev/fb0` 采集 1024×600 RGB565 帧 | 诊断工具；增加独立 capture 文件、显式 desktop PID 和二进制边界保护 |
| `tools/fb_render_map.py` | 将 RGB565 原始帧渲染为低分辨率字符色彩图 | 诊断工具；已修正 RGB 调色板并增加参数校验 |
| `logs/v1-coldboot-20260824-0056.log` | 早期已跑通基线的断电启动、DSI、fb0、GT911、PSRAM 证据 | 历史硬件能力证据，不替代当前干净 release 的复验 |
| `logs/v1-powercycle-20260824.log` | 早期基线的 POWERON、持续帧输出和触摸 down/up 事件 | 历史显示/触摸证据，不计入当前 release 的 5 次冷启动 |
| `logs/v1-lvgl-touch-monitor.log` | 早期基线的大量触摸坐标与复位后重新初始化记录 | 历史触摸链路证据，尚未证明当前 release 四角映射 |
| `logs/v1-known-good-restored-20260827.log` | 回刷已知可启动桌面后确认 NSH、desktop_main、fb0/input0 | 故障恢复证据 |
| `logs/flash555588/2026-08-24/*.jsonl` | 日志收集器生成的 AI Coding 会话增量 | 大赛日志；保留生成器原始记录，不手工改写 |
| `logs/flash555588/manifest.json` | AI Coding 会话索引 | 由收集器更新；与新增 JSONL 一同提交 |

## 明确不纳入

当前未提交的 board/chip overlay 源码没有进入本次交付。其中 `esp32p4_bringup.c` 会把已验证的触摸初始化顺序和总线退回旧路径，`esp32p4_desktop.c` 会删除触摸测试页，`esp32p4_display.c` 包含未经 v3.2 实板验证的时序，`esp_usbserial.c` 是成员 NuttX 中已有更完整实现的旧实验版本。这些文件继续保留在本机工作树，不进入大赛基线。

旧 NSH 二进制、失败的 I2C/DSI 试验日志、重复 reset 日志、`fb_dump1.raw*` 大文件以及尚未解决坐标边界语义的 GT9xx patch 同样不提交。它们不能提高当前可复现性，反而可能造成评委误用或发布状态混淆。

## 使用方法

先在 NSH 执行 `ps`，确认需要停止的桌面 PID，再采集 framebuffer：

```powershell
py tools\fb_dump.py --port COM7 --no-reset --desktop-pid <PID> `
  --out fb-current.rgb565
py tools\fb_render_map.py fb-current.rgb565 --w 1024 --h 600 --step 16
```

`--desktop-pid` 不是固定值，必须以当次 `ps` 输出为准。完整 UART capture 默认保存为 `<out>.capture`，RGB565 payload 保存为 `--out` 指定文件；两者都属于本地诊断输出，不应直接提交。

## 后续进入正式基线的门槛

诊断工具需要在当前干净 v1.0 release 上完成一次真实帧采集。GT9xx 坐标映射只有在四角原始坐标、旋转方向和 inclusive/exclusive 边界全部确认后才能形成正式驱动补丁。USB console 的后续改进只在成员 NuttX 固定提交上继续，避免大赛仓 overlay 与真实构建源码分叉。
