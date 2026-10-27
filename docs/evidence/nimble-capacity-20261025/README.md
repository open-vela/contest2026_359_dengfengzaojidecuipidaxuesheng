# 难度排序第 1 项：NimBLE 连接数配置映射

2026-10-25。已修复并部署：`68013af36b8d2678d321452efcbc6840146d8fd40297284702cb89bb6865f57d`。
基线：已验收 PSRAM XIP 固件 `77845a88158ea9e737cbb1230aeed3b0550f9cc48ff0c77d925c8e6ca6ab0030`。

| 验证层 | 修复前 | 当前结果 |
|---|---|---|
| 配置 | 1；历史候选即使写 2 也未传入上游 | 本候选显式选择 2 |
| syscfg 实际值 | 示例默认 1 | 真实 Makefile CFLAGS 传递后为 2 |
| 最终 ELF 连接池 | 112 字节 / 1 个槽 | 224 字节 / 2 个槽，与相同 ABI 探针一致 |
| 板上查询 | 缺少双值校验入口 | c6ble status：configured_connections=2 effective_connections=2 |
| 主机初始化 | 旧单连接版本 | 两槽候选实际 host_ready=1，界面应答通过 |
| XIP | 已启用 | 新镜像重新验证 ready=1、protected_cpus=3，底层准备源码完全相同 |
| 烧录 / 数据 | 原镜像已备份 | 独立完整读回 SHA 一致；烧录前后 4 MiB 数据区 SHA 一致 |

根因是导入的 NimBLE 示例头文件用 `#ifndef` 提供默认单连接值，Makefile 没有传入 CONFIG_NIMBLE_BLE_MAX_CONN。修复从公共 CFLAGS 覆盖该值，作用于连接池等所有 NimBLE 编译单元；应用增加编译一致性检查。builder 提供 --ble-connections 1/2/3，保留默认继承配置，并在最终链接后强制验证实际池大小。新镜像只有连接数配置从 1 变为 2，角色、任务组、邮箱、配对及 HID 会话行为未改变。

回归：真实 Makefile 与固定 SDK 的 1/2/3 容量映射通过；去掉映射的反例被拒绝。central、HID 邮箱 ASan/UBSan 通过；XIP 验证器、SDK 100 次 SPI1 mutex 配对/零 cache-off 通过。完整构建和当前 ELF 启动闭包、分区、BSS 检查通过。BSS 结束 0x4ffabf9c，小于 0x4ffaefc0。现有文件的 checkpatch 尚未全量通过，原因见 REVIEW.md；没有取消编译或链接保护。

当前镜像的显示/Flash 保留验证：2048 与 CSI 各 2 轮，合计写 1692 次、擦 169 次；最大显示 IRQ 间隔 16624 / 16721 us。实体屏幕观察：用户确认：这一版两个场景都正常；随后同意暂停手动切页以完成持续 CSI 写擦验收。断电冷启动和双连接未验收。

中途手动切页或停在“开始预览”页面的准备记录另存，未计为活跃 CSI 写擦验收；其中 camera-interrupted.json 已明确改为 passed=false。最终 CSI 记录在每轮前后及结束时都确认 busy=1、alive=1 且帧数增长。相机和蓝牙同时运行时内核堆余量偏小（本轮观测约 4–6 KiB），后续扩展仍需控制常驻资源并单独压测。

实际 BLE 主机、UI、XIP 和 Flash 写擦日志均来自本候选；不套用旧固件实测。完整制品位于 C:/Users/11937/Documents/ChatGPT/vela/output/nimble-capacity-20261025/，含冻结源码 source/、构建脚本 build.sh、firmware/、rollback/、最终容量审计及独立读回。C6 固件未改变；保留 XIP 的显式 --psram-xip。没有覆盖用户界面和其他未提交改动，也未自动提交/推送。

本项不等于蓝牙设备与鼠标已经能够并发：应用角色互斥仍存在。下一个顺序项目是第 2 项“分离设备连接与鼠标状态”，之后是 HID 连接归属和配对策略/保存失败反馈。

补充 CSI 复验：鼠标保持连接，预览帧 214→445；内部 Flash 写 755 次、擦 75 次，欠载/翻页错误为 0，IRQ 最大 16721 us。用户确认“全程正常”。对应 camera-recheck.log/json。
