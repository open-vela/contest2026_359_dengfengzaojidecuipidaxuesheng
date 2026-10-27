# 八设备自动化证据

对应 [测试报告](../../BLE_EIGHT_TEST_2026-10-27.md)。这些是宿主和离线构建证据，不是无线实机验收。

- `HOST-EIGHT.json/log`：生产 central 的八会话、并行 GATT、十轮八设备恢复、取消未确认隔离及 reset；ASan/UBSan。
- `HOST-DEFAULT.json`：默认两会话兼容回归。
- `CAPACITY-MAPPING.json`：真实 Makefile 和固定 SDK 的 1／2／3／8 配置映射。
- `ELF-CAPACITY.json`：目标 ABI 探针和最终 ELF 八槽／896 字节核对，应用容量八。
- `ELF-AUDIT.json`：最终镜像边界及静态 XIP 检查，物理验收仍 pending。
- `UI-EIGHT.json`：原生 LVGL 深浅色、八会话选择和暂停状态的宿主截图摘要。
- `FINAL.json`：自动化结果、制品摘要、实际构建源码对应关系与实机边界。

CLI、QPK 所有权和 HID 邮箱回归的执行结果记录于本聊天与 `FINAL.json`；原始可执行夹具、编译目录、截图、完整构建日志及冻结源码保留在工作区 `output/ble-eight-20261027/`。

初次冻结包缺少 ESP-TTS 锁文件的失败日志保留为 `build-first-error.log`；补齐输入后构建成功。现有 C6 配套配置为三连接，本轮未改动或刷写设备。没有八台外设，所以没有八设备无线实测。
