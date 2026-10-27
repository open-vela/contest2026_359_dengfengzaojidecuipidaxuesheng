# C6 八连接离线证据

对应 [构建与自动化报告](../../C6_BLE_EIGHT_2026-10-27.md)。本目录不包含设备备份、用户配置或密钥。

- `BUILD-RESULT.json`：完整 C6 构建退出码。
- `C6-TARGET-ELF.json`：配置唯一差异、目标 SDK 探针、链接容量、分区与保留源码摘要。
- `C6-INITIALIZER.json`：实际初始化函数的宿主正例与编译失败负对照。
- `SDIO-PRESERVED.json`、`WIFI-DIAGNOSTIC.json`：实际候选保留修复的回归。
- `P4-EIGHT.json/log`：配套 P4 八设备宿主模拟回归。
- `PAIR.json`：P4／C6 的八容量构建匹配、应用／ELF 摘要以及未上板状态。
- `MANIFEST.json`：本目录记录的 SHA256。

完整源码副本、实际 sdkconfig、ELF／BIN／map、esptool 镜像检查、尺寸报告、编译命令与日志在工作区 `output/c6-ble-eight-20261027/`。所有验证均为离线检查；未运行任何烧录或复位步骤。
