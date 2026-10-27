# P4／C6 八连接候选烧录与启动检查

2026-10-27，用户授权烧录测试并完成 C6 IO9 接地／断开操作。两颗芯片的应用均已部署，设备保持运行，串口已释放。本轮没有重新编译。

## 制品与数据

| 项目 | 本轮实际结果 |
|---|---|
| P4 | COM4，ESP32-P4 rev3.2，MAC E8:F6:0A:E7:6F:F1；应用地址 0x2000 |
| P4 BIN SHA256 | `4c7c78a0ec810eb4bfb32c45e3c29c198a03b9c618052c0d85d812d4af07ffaa` |
| C6 | COM5，ESP32-C6FH4 rev0.2，BASE MAC 98:88:E0:72:58:90；实际 OTA 选择 slot0，地址 0x10000 |
| C6 BIN SHA256 | `be0df846a3d283c75b766bcd90953ab40dd3c48edb2846502a047f005fe5a534` |
| 写入验证 | 两者均通过工具校验及完整独立读回 SHA256 比对 |
| P4 数据 | 烧录前备份完整 16 MiB Flash；0xc00000..0x1000000 的 4 MiB 数据区烧录前后 SHA256 一致 |
| C6 数据 | 已核对实际分区与 OTA selector，备份完整 1.5 MiB 活跃应用槽；仅写入应用，不写 bootloader／分区／OTA selector／NVS／PHY |
| 新鼠标身份 | `E9:16:AF:2C:B6:31`，static-random；`OpenVela Mouse-AF2CB631`。本候选首次部署，未使用旧身份配对结果冒充新身份验收 |

回退备份保存在工作区 `output/c6-hci-eight-20261027/board-test/private-rollback/`，可能包含用户配置，仅在本机保留，不放入报告或提交。

## 实机验证

- P4 NSH 正常，运行构建时间 Oct 27 2026 14:43:19；实读 `configured_connections=8 effective_connections=8`。
- PSRAM XIP `ready=1 protected_cpus=3`，text hash `b35ecab2`、rodata hash `0d2abf97` 与候选段计算值一致；`cache_policy=on spi1_mutex=1`。
- C6 启动 ELF 摘要 `a94207697...` 与候选匹配；启动明确报告 `BLE controller capacity=8`，HCI Over SDIO 初始化。
- 实际 BLE 扫描返回成功，发现 31 条广播记录；应用会话容量为 8，停止扫描返回成功；最终 `host_ready=1`、连接数 0、配对存储错误 0。
- 鼠标广播启动返回成功，观察到 `advertising=1 connected=0`；停止返回成功，最终鼠标未连接，普通会话准入未处于鼠标暂停状态。
- 配套采集窗口未观察到 panic、断言或 load/store access fault。C6 实际 Wi-Fi 关联成功；本轮不宣称网络压力验收。

## 过程异常与限制

自动 C6 下载握手失败后，按该板已验证接线方式由用户接地 IO9。首次 P4 USB 复位启动报告 `mismatch chip ID, expected 18, found 32772` 并停止；读回 P4 Flash 头的 chip ID 实为 18。改用看门狗复位后恢复启动并使 C6 进入下载模式。本轮没有定位这次启动差异的根因，不能将其记为已经修复的代码问题。

C6 首次独立读回在约 25% 时遇到 Windows 串口 `GetOverlappedResult PermissionError`，完整只读重试成功；没有重复写入。P4 看门狗复位使 USB 串口短暂消失，前两次启动采集脚本未处理该情况；后续等待重新枚举后完成配套日志采集，原失败日志保留。

鼠标启动／停止检查期间另捕获 ES8311 `I2S transfer failed: -12` 两次及 `wake_release ... result=-5`，其后出现唤醒 worker 重启日志。BLE 命令与最终状态正常，但这些音频错误尚未分析，不能声称整机所有功能通过。

没有八台可控制实体外设，本轮未进行八连接并发、GATT 业务、鼠标首次配对／加密／订阅、跨重启配对保持、吞吐或长期稳定性验收。未进行断电冷启动或实体屏幕观察；温启动与基本扫描／广播检查不能替代这些项目。

完整部署清单、SHA、启动日志及本轮脚本位于 `output/c6-hci-eight-20261027/`；核心证据是 `board-test/p4-deployment.json`、`c6-deployment.json`、`PAIRED-BOOT.json`、`MOUSE-SMOKE.json`、`paired-boot-3-COM4.log` 和 `paired-boot-3-COM5.log`。
