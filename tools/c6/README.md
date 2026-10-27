# C6 可选无线集成源码片段

本目录保存 C6 扫描、连接状态、RPC、桌面后端/worker 和 BLE 等适配片段。
它不是独立可烧录的 C6 固件，也不会因目录存在就自动被所有配置编译。
返回[工具索引](../README.md)。

2026-10-27 新增独立 C6 八连接构建入口 `../build_c6_ble_eight.py`，配置为
`sdkconfig.ble-eight.defaults`；固定 Hosted 2.0.13／IDF 5.5，保留诊断及 SDIO 修复。
它只准备、编译和验证独立源码副本，不执行烧录。制品及验证范围见
[C6 八连接报告](../../docs/C6_BLE_EIGHT_2026-10-27.md)。

后续构建还会应用 `../prepare_c6_hci_frames.py`，核对 Command／ACL 长度；
双向压力测试及最新未烧录制品见 [HCI 后续报告](../../docs/C6_HCI_EIGHT_2026-10-27.md)。

集成脚本、依赖和验证边界以 [README.c6.md](../README.c6.md) 为准。该文档记录的离线
构建候选不能直接代表 Wi-Fi 关联、DHCP、BLE 或实际射频功能已经通过验收；不同硅 revision
的限制必须分别核对。

宿主用例位于 [tools/tests](../tests/README.md)。改动协议、消息大小、并发所有权或重试逻辑时，
需同时运行对应测试，再安排与版本匹配的实板验证。
