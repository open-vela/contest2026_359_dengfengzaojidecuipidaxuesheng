# 同电脑 HID 与业务 GATT 共连接

后续候选 19 已保留本功能并修复 NuttX NPL 到期覆盖和 ENOTCONN 清理路径，
完成 20 轮重连及重启前后收发检查。最新部署与边界见 [定时器／断连修复](BLE_TIMEOUT_2026-10-27.md)；
以下保留候选 18 的原始范围。

本轮沿用项目已固定的 NimBLE：在同一 GATT server 注册 HID 和一个业务服务，
使用官方 `ble_gatts_count_cfg`、`ble_gatts_add_svcs`、CCCD 和 `ble_gatts_notify_custom`。
没有增加第二个蓝牙栈、HCI 通道或电脑 ACL 连接；普通 central 外设仍保留两个会话名额。
服务 UUID 和业务帧格式由本项目定义，不能称为官方固件原有的业务协议。
参考 [NimBLE GATT server API](https://mynewt.apache.org/latest/network/ble_hs/ble_gatts.html)、
[官方服务注册示例](https://mynewt.apache.org/master/tutorials/ble/bleprph/bleprph-sections/bleprph-svc-reg.html)、
[乐鑫 GATT 数据交换](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/ble/get-started/ble-data-exchange.html)。

## 协议与生命周期

| 对象 | UUID | 行为 |
|---|---|---|
| 服务 | `69d14e9d-5e2a-4564-8b03-7acf16a046bc` | 与 HID 同一 GATT server |
| RX | `fe295b09-ba85-4b80-aec3-095121e8ae2f` | 加密 write-with-response；1–244 字节，不解释为设备命令 |
| TX | `9fc89159-b5c1-4797-8f7a-0f375f7e8138` | 加密读取最后发送帧；订阅后 notify |
| 信息 | `e86cb6ea-0566-4ea9-b513-1d94e99c9a54` | 加密读取 8 字节：`01 00 f4 00 04 00 03 00` |

信息字段依次为小端 16 位版本 1、最大帧 244、RX 队列深度 4、协议标志 3
（bit 0 表示要求加密，bit 1 表示共享 HID 连接）。
TX 单帧上限为 `min(ATT_MTU - 3, 244)`，不自动分片或重传。
RX FIFO 满时返回 ATT `0x11`，不覆盖未读帧；业务端需要处理背压。
只有当前 HID 连接句柄／代次且链路已加密才能访问数据；使用现有 HID 的 16 字节密钥状态。
断开、reset 或新连接代次会清空收发数据和订阅，拒绝旧代次操作。

发送使用有界副本和常驻蓝牙任务处理，调用方不直接操作跨任务组 HCI/NPL 描述符。
最多一个待发送请求，支持取消，默认 3 秒超时；未执行请求取消后不会迟到重放。
发送成功表示 NimBLE 接纳，不表示对端应用已经读取；通知的真实收达在自动实机测试中独立比较。

QPK 增加 `ble.channelStatus`、`ble.channelSend({epoch,data})`、`ble.channelReceive({epoch})`。
保持现有 BLE 应用权限／所有权策略；应用退出只清理自己使用代次的数据，不断开系统 HID。
NSH 增加 `c6ble channel status/receive/send`，接收诊断只打印长度及 FNV 校验值。
NSH 行长限制为 80 字节，实机 CLI 发包测试限制在 20 字节；它不是完整 244 字节发送接口的替代品。

同时修复 HID 的周期保存定时器：邮箱轮询不再重置 50 ms watchdog 截止时间，
由 watchdog 自行续期，防止高频业务请求让配对用途记录一直无法持久化。
回归实际编译生产 poll/watchdog/persist 路径，以每虚拟毫秒一次请求确认定时保存仍发生。

## 制品与验证

候选 18，镜像 SHA256：
`7d9715148a1ddcac9a534b5feea1e4ae8d70592e4e5573c44589340daff4ed48`。
ELF：`431188fe17171aee9e680d0523dfd3a1ce3f9f81e5ba2a9ea46a2c923dcd9cd6`。
新测试身份：`DB:9B:EB:2E:71:EF / OpenVela Mouse-EB2E71EF`。
候选 17 从未部署，由包含 watchdog 修复的候选 18 取代。

15 组宿主／原生 LVGL 回归及完整固件构建通过，包含实际 channel 生产代码的 ASan/UBSan 测试。
覆盖加密及句柄归属、UUID 字节序、RX 边界／FIFO／满队列、独立订阅、MTU、发送副本、
取消／超时不重放、断开唤醒、复用句柄旧代次拒绝，以及实际 QPK 参数及退出清理。
UI 测试的 103 个实际输入在候选 18 中逐项核对不变，保留原生双主题／60 次重开的结果及复用清单。
宿主的 NimBLE 传输替身不等同于无线实机验收。

最终 ELF/XIP 闭包和源码哈希已审查：BSS `0x4ffad25c` 小于 `0x4ffaefc0`，连接池 3／336 字节。
预期 XIP FNV 为 text `bbb6f07b`、rodata `0cde17e3`；配置相对已安装候选 16 不变。
保留应用偏移 `0x2000`、数据区 `0xc00000..0x1000000`，回退镜像为候选 16。
C6 固件、eFuse、分区、加密配置以及已拒绝的 IRQ／自动挂起／DSI reload 状态均不变。

应用区已烧录并独立回读一致，烧录前后整个 4 MiB 数据区哈希一致；暖启动两核 XIP 通过。
新身份 Windows 首次配对为 `PAIRED`，API protection-level-used 为 `ENCRYPTION`，板端 HID 加密／订阅就绪。
暖重启后显式开启鼠标，Windows 复用已有配对恢复连接，不需要重配；这不是开机自动启用策略的验收。

Windows 系统 HID 与业务 GATT 客户端共用实际一条连接，两轮完整测试各 8 轮、合计 **16 轮**通过。
每轮先由电脑写 RX，板端核对长度／FNV；再由板端发送，电脑严格比较通知和读取回执的全部字节，
同时发送零位移鼠标释放报告并确认 HID 加密、订阅仍在。没有对用户桌面发送位移或点击。
两轮均验证 244 字节 RX、四帧 FIFO 顺序及第五帧返回 ATT `0x11`，取消业务通知后 HID 仍连接。
NSH 行长限制下，真实 TX 最大验证 **20 字节**；244 字节 TX 只有宿主验证，不能混写为实机已通过。

测试客户端的失败记录均保留：UUID 过滤发现返回 `ERROR_BAD_COMMAND`，改用官方完整服务发现后选择精确 UUID；
直接 Python 字节缓冲写入返回 `ACCESS_DENIED`，服务访问状态为 `ALLOWED`，改用官方样例的
`DataWriter.detach_buffer()` 后，同一固件和连接代次成功。只设置 `ENCRYPTION_REQUIRED` 并未解决该缓冲问题。
参考 [微软 GATT 客户端样例](https://github.com/microsoft/Windows-universal-samples/blob/main/Samples/BluetoothLE/cppwinrt/Scenario2_Client.cpp)。
首次重连诊断误等已连接设备的广播而超时，已改为查询精确已知身份与现有配对。
重启后第一轮在第 5 次串口前缀交错时提前结束，未计入完整 16 轮；修正前缀解析后完整重跑通过，
对端全部字节比较没有放宽。

断连清理的首次尝试出现一次 `encryption status=7`、`disconnect reason=520`，随后从代次 3 自动恢复到 5。
固定 SDK 中 `7` 为 `BLE_HS_ENOTCONN`，`ble_sm_connection_broken()` 会发出该状态；
`520=0x200+0x08` 是 HCI connection supervision timeout，不能直接认定密钥丢失。
此超时根因仍未定位。最初脚本将 `-116` 误匹配为 `-11`，原结果已明确作废，见 INVALIDATED 记录。
修正为完整行断言后的独立复测通过：代次 5→7，排入的未读帧清空，旧代次收发均返回 `-116`，
新代次空队列严格返回 `-11`，HID 保持加密／订阅。有限次数恢复不代表该瞬态已修复。

最终实际状态为 `connected=1 encrypted=1 notify=1 epoch=7 store_error=0`，
业务通知已取消，业务队列为空，普通连接数为零；连接池仍为 3。
软件 UI 帧数继续增加，不作为实体显示验收。
最后只读检查还捕获后台 `I2S transfer failed: -12` 与 `wake_release -5`，未定位且未归因于业务 GATT；
不将本轮蓝牙验收扩展成音频稳定性通过。
实机状态与原始记录见 [RUNTIME.json](evidence/ble-shared-gatt-20261027/RUNTIME.json) 及
[证据索引](evidence/ble-shared-gatt-20261027/README.md)。
完整 Flash 备份含用户数据，只留工作区，不放入仓库。

## 验证边界

两个实体普通 BLE 外设加 HID 的三连接同时传输尚未验收；本机此前的 Windows 可连接 GATT 广播未能启动，
不能用非连接广播或宿主替身冒充外设。当前共连接测试独立验证另一种拓扑：电脑 GATT 客户端与系统 HID。
不宣称全部官方固件能力已经等价，也不宣称 MITM、持续鼠标时延、长期射频共存或冷断电启动通过。
实体显示不以软件帧数代替验收；按用户偏好不发起本人参与测试。
稳定下载 5 Mbps 仍是设计目标，吞吐验收保持 `skipped_by_user`，不安排测速。
