# Firmware Index

本目录优先展示当前 v3 交付线。历史 v1 和早期实验镜像统一放在
[`历史测试固件/`](历史测试固件/)，不会作为当前版本推荐或自动烧录目标。

## 当前优先版本：v3

进入 [`v3/README.md`](v3/README.md) 查看当前 v3.2 构建候选、NSH 控制台镜像、USB Serial/JTAG 变体、芯片 revision 要求、校验和及烧录注意事项。

| 版本 | 用途 | 状态 |
| --- | --- | --- |
| [`v3/esp32p4-desktop-v3.2-candidate/`](v3/esp32p4-desktop-v3.2-candidate/) | ESP32-P4 v3.x 桌面构建候选 | 已构建；没有 v3.2 实板验收，不标记为正式 release |
| [`v3/esp32p4-nsh-v3.2/`](v3/esp32p4-nsh-v3.2/) | v3.2 UART NSH | 仅匹配 v3.x revision |
| [`v3/esp32p4-nsh-v3.2-usb/`](v3/esp32p4-nsh-v3.2-usb/) | v3.2 USB Serial/JTAG NSH | 仅匹配 v3.x revision |

当前已经在本地 v3 实板验证的应用固件和 UI 证据位于
[`app/espdl-quickapp/`](../app/espdl-quickapp/README.md) 与
[`logs/2026-09-19/`](../logs/2026-09-19/)。该实板镜像大小为 `7096868` bytes，SHA-256 为
`25f42781e8cd0dfe3a814f0a7b9e83b7b86910887f090860655e310275fdd6fe`；提交仓库保留源码和证据，未把本地整机生成物冒充为可复用通用镜像。

## 历史测试固件

[`历史测试固件/README.md`](历史测试固件/README.md) 说明 v1.0/ECO2 桌面、SC2336、相机预览和早期 NSH 镜像。它们只用于复盘和对比，不能替代 v3，也不能烧录到不匹配的芯片 revision。

## 校验与烧录规则

每个可烧录目录的 `SHA256SUMS` 只对同目录文件生效，不能跨目录套用摘要。仓库根目录执行
`python3 tools/check_package.py` 可检查固件清单、哈希和仓库卫生。

烧录前必须确认芯片 revision、下载接口、分区布局和镜像的 README。历史 v1 镜像不能直接用于 v3.x，v3.x 镜像也不能写入 v1.0/ECO2 板。不要把模型、整机 Flash 转储或含私有配置的文件当作固件提交。
