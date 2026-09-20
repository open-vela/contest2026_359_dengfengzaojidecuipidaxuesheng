# ESP32-P4 v3.2 OpenVela 桌面构建候选

本目录是 `esp32p4-function-ev-board:desktop` 的 v3.2 编译候选。2026-08-27 在完成 v1.0 构建后，构建脚本以 `configure.sh -E -S` 清理并切换配置，解析结果为 `CONFIG_ESP32P4_REV_MIN_301=y`、`CONFIG_ESPRESSIF_CPU_FREQ_400=y`，全量构建返回 0，LVGL 版本警告为零。

```text
nuttx.bin  2954848 bytes  886A9FB5793F6EE17285C8BBE373FEFAE5450DD5BD95FF19A4E2A634B07FE80E
```

当前本机只有 revision v1.0 板卡，本候选没有 v3.2 实机启动证据，不能标记为正式发布，也不能烧到 v1.0 板。取得真正 v3.2 板卡后，必须完成芯片 ID 核验、烧录校验、5 次冷启动，以及 LCD、GT911、PSRAM、桌面和控制台验收。

ELF 和 HEX 位于本地构建输出，不纳入 Git 候选包。完整来源固定点见 `BUILD-METADATA.txt`，构建日志见 `logs/build-repro-v3.2-20260827.log`。
