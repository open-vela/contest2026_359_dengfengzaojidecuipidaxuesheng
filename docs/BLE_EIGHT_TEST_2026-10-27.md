# 八设备 BLE 自动化测试

后续：C6 配套八连接候选现已完成离线编译与自动化，见 [C6 八连接报告](C6_BLE_EIGHT_2026-10-27.md)。下文 C6 三连接为本轮 P4 测试时读取的旧基线配置；设备没有因此自动更新。

2026-10-27。用户要求按八台测试，并确认目前没有八台实体外设，先做自动化。本轮已恢复宿主测试和离线构建，没有烧录、复位或访问实板。

## 结果

| 检查 | 结果与范围 |
|---|---|
| 八设备准入 | 生产 central／角色管理／邮箱加受控 NimBLE 替身；八条模拟连接，第九条与重复设备拒绝 |
| GATT 并发 | 八个独立等待者；取消一个保留另外七个；句柄复用时旧回包被拒绝 |
| 每设备状态 | 独立读、MTU、配对与通知队列溢出检查通过 |
| 鼠标切换 | 十轮暂停八台／恢复八台通过；八十个旧 GAP 回调被拒绝；鼠标与普通设备保持互斥 |
| 取消终态 | 使用生产等待期限，模拟连接无回包、取消已接受但未确认：后续七次发起被阻止；终态到达后显式重试恢复七台 |
| 故障隔离 | 一台重连失败保留其余七台；暂停期间所有者退出只取消对应恢复 |
| Reset | 唤醒八个 GATT 等待者，不重放旧恢复 |
| QPK | 八条会话与恢复结果 JSON；所有权裁剪、清理七个自有连接并保留一个外来所有者通过 |
| 原生 UI | 真实 LVGL、深浅色；八设备计数与第八台选择、暂停状态、取消恢复，以及既有生命周期断言通过；截图为宿主渲染 |
| 兼容回归 | 默认两会话 central、HID 邮箱、QPK 所有权、CLI 参数边界通过 |
| 编译配置映射 | 真实 Makefile＋固定 SDK 的 1／2／3／8 映射通过，未映射的旧单连接反例被拒绝 |
| 固件／ELF | 独立 P4 完整构建成功；目标 ABI 探针确认 Host=8、应用会话=8；最终 ELF 连接池 896 字节与探针匹配 |

上述宿主执行使用 ASan／UBSan。它验证应用代码和受控回调下的边界，不是八台实体设备无线并发、吞吐、射频、Wi-Fi 共存或长期稳定性的证明。

## 构建与制品

正常源码默认仍为两个普通会话。`glass_ble_capacity_config.h` 是所有消费者共用的容量头；仅独立构建显式指定 `--ble-eight-test` 时，准备树改为八个普通会话与八个 Host 槽。单独使用 `--ble-connections 8` 会被拒绝；没有提升正常默认值，也没有修改 C6 固件。

- BIN SHA256：`4c7c78a0ec810eb4bfb32c45e3c29c198a03b9c618052c0d85d812d4af07ffaa`。
- ELF SHA256：`ef69f2fa10a9d43d94b56b2143f9977d9de34c3c1511b25cb97286854c4c8025`。
- 镜像 10,510,976 字节；原应用／4 MiB 数据区边界检查通过。BSS 末地址 `0x4ffad830`，低于保护边界 `0x4ffaefc0`。
- 最终 ELF 静态 XIP 检查通过：复制前直接调用未发现 PSRAM 执行，间接调用集中在原校准函数；保留两核保护和 SPI1 mutex 路径。这不是冷启动或实板 Flash／显示验收。

第一次构建因冻结包漏带 ESP-TTS 的锁文件而在准备阶段失败；补齐三个语音输入文件后构建成功，首次失败日志保留，没有取消输入校验。

完整制品、冻结生产源码、日志和宿主截图位于工作区 `output/ble-eight-20261027/`。仓库内无私密数据的摘要见 [证据索引](evidence/ble-eight-20261027/README.md)。

## 实机边界

现有 C6 配套构建配置 `c6-build-20261025/sdkconfig.c6-fixes` 中，`CONFIG_BT_LE_MAX_CONNECTIONS=3`。这是已读取的配置证据，不是本轮设备回读；本轮没有改动或刷写 C6，也没有宣称芯片最大能力只有三条。

因此当前八槽 ELF 是离线实验制品，**未通过实机八连接资格验证**。后续实机验证需要匹配的 C6 控制器配置／固件，以及八台可控制的测试外设；不能只改 P4 的 Host 数字就宣布设备支持八连接。

复现核心自动化：

```sh
python3 tools/test_ble_central.py --connections 8 --output <eight-host-output>
python3 tools/test_ble_platform.py --connections 8 --apps <prepared-apps> --output <platform-output>
python3 tools/test_nimble_capacity.py --nimble <pinned-mynewt-nimble> --output <mapping-output>
python3 tools/nimble_capacity.py --apps <eight-prepared-apps> --nuttx <eight-prepared-nuttx> --compiler <riscv32-esp-elf-gcc> --elf <nuttx.elf> --output <audit-output>
```
