# C6 SoftAP 后端与 STA 试连事务

本文件记录底层代码与宿主验证，不能代替固件构建、烧录或真实热点连接验收。

## 实现

- `c6_softap_start(ssid, password)`：通过现有网络 worker 执行阻塞操作；先停止 Wi-Fi，再设置 AP+STA、安装 WPA2-PSK/CCMP 配置并读回核对，注册独立 `ap0`，最后启动 Wi-Fi。避免启用 AP 模式后先广播旧配置的窗口。保留 STA 凭据、用户手动离线语义及 C6/BLE 所有者。
- `c6_softap_stop()`：始终查询 C6 实际模式，不再以 P4 的 AP 对象为空/本地未启动作为已停依据；远端 AP+STA 转 STA，AP-only 转 NULL，已有 STA/NULL 直接确认。停止路径不初始化/复位 C6、不启动或连接 STA。超时或清理失败显式标记 `state_unknown`，不得当成已确认停用。
- P4 新生命周期默认远端未知，正常网络初始化须实际设置并读回 STA 模式后才发布完成。
- `c6_softap_status()`：只返回活动状态、错误、是否短暂中断 STA、接口名及 SSID，不返回密码。
- AP/STA 收包、发包与协议栈即时响应使用各自的 Hosted interface type。AP 停用时拒绝接口开启和数据收发；拒绝非零 Hosted interface number。
- AP 缓冲区按需分配，复用已有 `c6net` 轮询任务，不添加常驻网络任务。DHCP、IP 和 portal 由上层处理。
- 已核对当前 NuttX：新 netdev 追加到列表末尾；IPv4 默认出口优先已有非零网关。AP 网关为零时不会覆盖可用的 eth0/wlan0 外网出口。
- `c6net_trial_connect()` / `c6net_trial_finish()` 与现有连接锁共用所有权。试连期间抑制自动重连和普通连接动作；成功提交启用新网络重连，回退恢复原策略。底层 `c6_wifi_trial_*` 的完整旧配置快照与 RAM/FLASH 提交由独立 RPC 片段维护。上层必须等本代次 DHCP 后才能提交。

## 官方源码核对

当前 C6 使用的官方 Hosted 提交 `0c4aa5f40ce0feebb454c1d671c39329c795b037` 已有独立 AP 帧接口、AP_START/AP_STOP 收包回调注册与 AP 配置 RPC。参见 [slave_control.c](https://github.com/espressif/esp-hosted-mcu/blob/0c4aa5f40ce0feebb454c1d671c39329c795b037/slave/main/slave_control.c) 与 [app_main.c](https://github.com/espressif/esp-hosted-mcu/blob/0c4aa5f40ce0feebb454c1d671c39329c795b037/slave/main/app_main.c)。本实现不升级 C6 固件，也不替换 protobuf ABI。

## 配置持久性

每次启用会生成新热点身份/密码，但当前 C6 Wi-Fi 全局存储默认是 FLASH。AP `WifiSetConfig` 和模式设置可能写入 C6 NVS；停止热点只改变并确认模式，不擦除 AP 配置。这里的“临时热点”只指按次生成、默认不开启的运行生命周期，不代表密码从未写入 Flash。存储加密、eFuse 与 RAM-only AP 配置不在这次功能补丁范围内。

## 已执行的验证

`tools/test_c6_softap.py --apps <prepared>/apps --output <host-output>` 通过，使用 GCC 的 ASan/UBSan：

| 维度 | 结果 |
|---|---|
| 初始化 | 独立 AP MAC、接口注册、首次启动 8 个步骤逐一失败，清理与退回 STA 均检查 |
| 总线/协议 | 实际 protobuf 编解码；请求大小受现有 256 字节上限约束；响应类型、ID、payload、错误码及字段读回检查 |
| 所有权 | AP/STA 帧与即时响应严格分离；非零接口号丢弃；试连期连接/断开/重连/AP 操作拒绝 |
| 并发/任务 | 复用连接 mutex 和 RPC 唯一 UID 所有者；RX 不执行同步 RPC；AP 可见性通过原子发布和 netdev 锁协调 |
| 错误恢复 | 逐步超时、错误/畸形响应、清理失败的未知状态、显式重试停止、用户手动离线、试连提交失败保留/回退 |
| 代码格式 | `git diff --check` 通过；checkpatch 仍报告既有压缩格式、生成 protobuf 的 CamelCase、NTFS 执行权限及默认编译器缺失，未报告为全项通过 |

尚需整机固件构建与真实 AP 连接、DHCP、portal 新凭据试连/失败回退、STA/AP 共存和屏幕/Flash 验收。没有进行串口操作、烧录、eFuse 或 Flash 加密操作。
