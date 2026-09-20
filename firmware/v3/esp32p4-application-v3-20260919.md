# v3 应用实板验证记录

这是 2026-09-19 在 ESP32-P4 v3.2 实板上烧录并验证的本地应用版本记录。它与同目录的 2026-08-27 `esp32p4-desktop-v3.2-candidate/nuttx.bin` 构建候选不同；源码和应用资源以仓库当前 `app/espdl-quickapp/` 为准。

## 镜像标识

| 项目 | 值 |
| --- | --- |
| 芯片 | ESP32-P4 revision v3.2 |
| 镜像大小 | `7096868` bytes |
| SHA-256 | `25f42781e8cd0dfe3a814f0a7b9e83b7b86910887f090860655e310275fdd6fe` |
| 烧录范围 | 仅程序分区；模型分区和 `/data` 保留 |
| 快应用 | 米家 HA `com.openvela.homeassistant 0.7.0` |

## 已验证项目

- 烧录后冷启动日志识别 `chip revision: v3.2`，系统进入 NuttX 和桌面。
- `desktop ui ha` 成功启动 Home Assistant 快应用。
- `hass status` 显示已配置，UI 状态和布局捕获已归档。
- Home Assistant 前端使用已验证的原生连接服务路径。
- BLE HID/触摸板状态记录为 ready、enabled、connected、encrypted、notify。

原始记录见 [`logs/2026-09-19/`](../../logs/2026-09-19/)、[`docs/WORKLOG_2026-09-19.md`](../../docs/WORKLOG_2026-09-19.md) 和 [`app/espdl-quickapp/evidence/`](../../app/espdl-quickapp/evidence/)。

该记录证明本地 v3 应用版本已完成实板验证；它不把 2026-08-27 的旧桌面候选二进制重新标记为同一版本。
