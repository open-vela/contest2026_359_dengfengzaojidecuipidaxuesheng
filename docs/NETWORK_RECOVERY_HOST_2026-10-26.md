# 网络自救服务宿主验收

执行 `tools/test_network_recovery.py --output <host-output>`。测试直接编译实际 `glass_network_recovery.c` 与 `desktop_worker.c`，只替换平台 I/O、时钟和远端设备；不重写服务状态机。服务与 worker 的源码/头文件在执行前冻结到输出的 `source/`，`RESULT.json` 记录对应 SHA256。

GCC ASan/UBSan 已通过。宿主线程为 sanitizer 使用 64 KiB 栈，不改变目标固件 8 KiB 配置。

| 验收范围 | 宿主证据 |
|---|---|
| 常驻所有者 | AP、DHCP、portal、STA 试连和维护 I/O 均断言来自同一个真实 worker pthread |
| UI 与邮箱 | AP 后端被阻塞时，UI 仍能读取状态；并发请求被有界邮箱拒绝，无第二个硬件工作线程 |
| AP 就绪 | DHCP start 成功只进入 STARTING；绑定状态确认后才 READY；超时与 READY 后故障均清理并报告 |
| 清理 | IP/掩码/网关逐项故障；DHCP start 失败但已留 PID；DHCP stop 失败不停止 AP；AP stop/status 失败保留错误、可能活动状态与本地密码 |
| 错误保留 | AP 启用原始错误与清理错误分别报告；试连 begin 失败且回退失败保留两项错误 |
| 密码隐私 | 每次热点启动产生新密码；只有本地 credential API 返回；HTTP status/config 源码没有该 accessor，状态字段不含密码 |
| RAM 试连 | 旧 generation、错误 DHCP lease 均不能提交；当前代次与 DHCP 同时有效才提交 |
| 不重发 | commit 错误保持 pending，维护不自动重试；明确停止才回退；超时回退一次 |
| 状态刷新 | 提交/回退后重新读取真实 link，不沿用提交前的 IPv4 快照 |
| 持续轮询 | joining 时反复接纳 STATUS 仍能提交；AP STARTING 时共享邮箱持续 STATUS 仍能按维护周期更新 READY |

审核过程中发现并修正的生产缺陷：AP status 失败曾被当作已停、DHCP start 负回值可能遗漏子任务清理、试连清理错误被忽略，以及不断 STATUS 请求可能重置维护截止时间。最终宿主回归覆盖这些触发条件。

## 栈与堆核对

当前目标配置是 FLAT、独立 kernel heap 和 PSRAM user heap。实际源码链路：`pthread_create` 继承父 group，`up_create_stack` 使用 `group_malloc`；仅 `GROUP_FLAG_PRIVILEGED` 的组分配到 kmm，普通 task 组使用 kumm。`group_create` 仅对 kernel thread 类型设置该特权标志。普通桌面任务创建的 worker 栈与 `task_create` 创建的 DHCP 栈因此走用户堆，当前 P4 主用户堆位于 PSRAM。

内核堆仍需承担 TCB、group、文件表及 socket 等开销。不能直接把 8 KiB worker 栈加 DHCP 栈从先前约 9960 B 内核空闲量中扣除，也没有证据支持缩小工作线程栈。保留 8 KiB；实际候选仍需核对运行时栈地址、余量与内核堆。

这些结果属于宿主与目标源码审计，尚不能替代最终固件构建、真实热点/DHCP/浏览器配网、失败凭据回退或显示/Flash 实体验收。
