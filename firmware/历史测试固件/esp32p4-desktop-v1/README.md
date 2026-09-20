# ESP32-P4 v1.0 桌面历史交付

本目录保存 `nuttx.bin`、[SHA256SUMS](SHA256SUMS) 和
[TEST_REPORT.md](TEST_REPORT.md)。测试报告对应 2026-08-30 的 v1.0 镜像。
返回[固件索引](../README.md)。

它记录了对应版本的烧录校验、冷启动、PSRAM 与桌面/QuickJS 状态，不是后续 v3 应用
开发快照的整机镜像。请先阅读报告中的板卡和芯片 revision，不能烧到不匹配的硬件。

在本目录下可用 `sha256sum -c SHA256SUMS` 核对文件；仓库根目录也可运行
`python3 tools/check_package.py`。校验成功只能证明文件与清单匹配，不等于设备已启动。
烧录接口、偏移与分区必须按该版本确认，不在本说明中提供自动刷写操作。
