# 停止测试后的失败证据

CURRENT_STATE.json 是停止时状态。Flash 校验写回候选 19；最后复位后的运行状态未确认。

- business-c20-01.json 保持失败；原始 2h0 日志有 reason 520，2h1 是自动重连后的 epoch 15。
- channel-c20-01.json 是串口标签丢字的检查失败；后一次 channel-c20-02 有 8 轮完整收发和中立报告结果，不覆盖前次失败。
- pure-c20-01、input-c20-before、native-pair-20 是候选 20 的局部成功，不能扩大为整个版本通过。
- C21-FROZEN-HOST／UI／ELF 是冻结候选 21 的历史构建、宿主、链接证据，不属于停止后修改的当前源码。
- C21-FIRST-BOOT-ERROR、C19-ROLLBACK-BOOT-ERROR、Flash verify、双核锁等待和最后 JTAG reset 分别记录，不合并成“恢复正常”。
- UNVERIFIED-SOURCE.json 记录停止后的被动观察源码哈希；没有执行它的测试或构建。

MANIFEST.json 索引所复制原记录的 SHA256。私有 Flash／数据备份、密钥和完整用户配置不入仓。后续自动测试与烧录已停止。
