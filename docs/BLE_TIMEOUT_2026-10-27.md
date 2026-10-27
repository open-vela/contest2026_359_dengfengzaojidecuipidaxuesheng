# 瞬时断连：NuttX NimBLE 定时器交付修复

最新补充：候选 20 仍复现过监督超时，候选 21 的启动检查也失败。用户已停止测试。
本文保留候选 19 的历史结果；当前回退后的启动状态与新源码不继承这些结论。
详见 [失败经验与收敛方案](BLE_FAILURE_LESSONS_2026-10-27.md)。

本轮定位候选 18 曾出现的 `disconnect reason=520`。固定 SDK 中它表示 HCI connection
supervision timeout；此前 `encryption status=7` 为 `BLE_HS_ENOTCONN`，是连接清理路径发出的通知。
不能将这个状态单独解释为密钥损坏，也不能将后续自动重连当成根因已解决。

当前已经使用标准 BLE 的 GAP、SMP、GATT 和 HID over GATT；自定义的是业务服务 UUID／帧格式，
不是新的空口或配对协议。ESP-Hosted 封装与 NuttX 事件调度属于板级内部适配，
不能用“协议是标准的”代替其实现正确性。ESP32-C6 不支持经典蓝牙，SPP 串口、A2DP 等经典蓝牙 profile
不能通过 P4 固件补出，见 [乐鑫 BLE 协议栈说明](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-guides/ble/overview.html)。

## 确认的缺陷与修改

原 NuttX NPL 在 SIGEV_THREAD 到期回调与常驻分派线程之间只保存一个 `pending_callout` 指针。
分派线程尚未处理时，另一个定时器到期会覆盖它。固定 SDK 的 HCI socket 接收通过 10 ms callout
继续轮询；其到期交付丢失会阻断后续收包。HID watchdog、host heartbeat 也共用这条交付路径。
参考 [Apache NimBLE NuttX 原实现](https://github.com/apache/mynewt-nimble/blob/master/porting/npl/nuttx/src/os_callout.c)。

回归编译的是完整生产文件：强制 32 个独立定时器在分派前到期，候选 18 的旧实现只交付 **1/32**，
HCI 轮询代表事件为零次，程序返回 42；负控制明确失败。修正后 **100 批 × 32 个独立定时器**全部交付。
测试使用受控 POSIX 通知和事件队列，不是射频丢包试验。

修复为每个 callout 保留一个独立待处理节点，使用互斥保护的链表交给原常驻 NPL 线程。
不同定时器不再互相覆盖；同一定时器的重复待处理到期可合并，沿用 NPL 事件的合并语义。
到期时不分配内存、不改变 HCI 协议、不增加蓝牙栈，跨任务组的 SIGEV_THREAD 回调仍不直接投递 NPL mq。
原有定时器停止、事件已入队后的生命周期语义未扩大；本配置的相关 callout 对象为常驻对象。

另按固定 SDK 的 `ble_sm_connection_broken()` 调用顺序修正 HID 加密回调：
当前句柄的 ENOTCONN 只撤销加密就绪和待保存标记，等待正式 DISCONNECT 统一清理，
不再伪报鉴权失败或向已经被控制器移除的句柄补发 terminate。16 字节密钥、实际加密及句柄归属检查保留。
仍使用官方 GAP/GATT 和安全管理 API，参考 [NimBLE GAP](https://mynewt.apache.org/latest/network/ble_hs/ble_gap.html)。

## 诊断与验证范围

增加只读命令 `c6ble timers`：

```text
NPL_CALLOUT expirations=... dispatched=... coalesced=... pending=... highwater=...
```

计数由同一互斥保护，满足 `expirations = dispatched + coalesced + pending`。
dispatched 表示交给事件队列或直接回调，不等同于应用回调执行完毕；计数按 uint32_t 回绕。
它不读取密钥或业务内容，不会启动蓝牙或触发设备动作。

候选 18 实机对照：额外 6 轮纯 HID 关闭／重开，以及 4 轮业务收发／客户端退出／HID 重连均未再次出现额外代次变化。
因此，本轮确认并修复的是可重复的 NPL 丢事件缺陷；**没有还原历史那一次 520 的完整空口因果链**。
当前回归结果不得写成所有射频、电脑驱动或控制器超时原因均已排除。

候选 19 完整构建及 15 组宿主回归通过；原生 LVGL 双主题／60 次复开的 103 个实际输入逐项不变，合计 16 组检查。
新增回归保留真实错误／弱密钥拒绝，并检查 ENOTCONN 不补发 terminate。

镜像：`b6b17eed6481edfa4ba977a2c19a5930452081281b0ae585f44a039c8ca39b0f`。
ELF：`574c8fad502bbb1c7bf31aefb08b8887bb90e18d00a5f100a91723d577ac428c`。
新身份：`F9:71:66:11:70:FC / OpenVela Mouse-661170FC`。
BSS `0x4ffad2b0`，低于 `0x4ffaefc0`；连接容量仍为 3。
预期 XIP text/rodata FNV 为 `d536b33c / ccf853f1`。
源码变化限制为 HID 回调、NPL 源码／结构及诊断头、CLI；SDK 配置与 XIP 来源保持不变。
C6 固件、分区、eFuse、安全设置及已拒绝的 IRQ／自动挂起／DSI reload 试验状态不变。

第 19 版已应用区烧录并独立回读，整个 4 MiB 数据区在启动前核对一致。
首次部署的启动诊断写串口超时，已自动校验回退候选 18；旧辅助脚本未实时保存早期启动输出，
不能据此判定是固件还是电脑串口原因。同一镜像与身份的第二次部署采用连续启动日志，启动及后续暖重启均通过；
首次失败和回退记录独立保留，不覆盖或改写为成功。

新身份已自动完成 Windows 首配，板端确认加密／HID 订阅；暖重启后原配对恢复，无重新配对。
Windows 配对 API 的 protection-level-used 报告 `NONE`，实际链路加密由板端检查，未宣称 MITM。
实机完成 **12 轮纯 HID＋8 轮业务客户端退出后的重连**，逐轮和轮间代次核对均只有预期变化，未观察到额外断连。
实板 NPL 待处理队列峰值达到 **2**，计数守恒且持续交付。

重启前／后两组完整检查各 8 轮，加业务循环的 16 轮，共 **32 轮主要收发校验**通过；
每轮严格比较电脑通知与读取回执的全部字节，同时核对板端接收校验和鼠标加密／订阅。
244 字节 RX、四帧 FIFO、满队列 ATT `0x11` 和取消业务通知保留 HID 通过。
真实 TX 最大检查 20 字节，244 字节 TX 仍只有宿主验证。

两次辅助脚本失败保留：纯 HID 首批 6 轮后，串口标签 `expirations` 少一个字母；
按固定字段顺序逐项复核原始 12 个完整数值及守恒关系，另存 REVIEW，原 runner 的失败状态不改。
暖重启后的第一次完整数据尝试已完成 8 轮对端字节比较，但第四帧 FIFO 的串口前缀丢了 `B`，
其 8 轮不计入上述主要计数。修正辅助脚本为发送命令期间持续读取串口、保留 CR 后立即到达的回执，
并保留全部校验值严格比较后，重新跑完整协议检查通过。辅助脚本调整不改变烧录镜像。

最终 `connected=1 encrypted=1 notify=1 epoch=1 store_error=0`，业务通知已取消、队列为空；
一条 HID／GATT 共用 ACL、普通连接零。暖重启后最终 NPL `5664/5664` 次到期／交付、待处理零；
UI 软件帧数从 4917 增至 4942，不作为实体显示验收。
实机结果见 [RUNTIME.json](evidence/ble-timeout-20261027/RUNTIME.json) 和
[证据索引](evidence/ble-timeout-20261027/README.md)。
冷断电及实体显示验收独立保留状态；完整 Flash 备份和用户数据不入仓。
