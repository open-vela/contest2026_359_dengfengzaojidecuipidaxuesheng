# C6 HCI 八连接后续证据

对应 [HCI 后续报告](../../C6_HCI_EIGHT_2026-10-27.md)。所有记录均为离线验证，不表示设备已经更新。

- `HCI-FRAMES.json`：真实 Hosted／SDK 函数、8,000＋8,000 合成帧、边界及旧路径负对照。
- `HCI-CANDIDATE.log`、`HCI-OLD-OVERFLOW.log`：候选正常帧压力结果与旧路径 ASan 预期失败。
- `INITIALIZER.json`：本轮实际源码的初始化回归。
- `TARGET-ELF.json`：本轮实际 SDK ABI、容量、HCI 拒收路径、镜像及分区验证。
- `BUILD-RESULT.json`：独立 C6 完整编译退出码。
- `PAIR.json`：当前 C6 与原 P4 八槽候选的摘要及未烧录状态。
- `MANIFEST.json`：文件摘要。

完整源码副本、BIN／ELF／map 和日志保留在工作区 `output/c6-hci-eight-20261027/`。
