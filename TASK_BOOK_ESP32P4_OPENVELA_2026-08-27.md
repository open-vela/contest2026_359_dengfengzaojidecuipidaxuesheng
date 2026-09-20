# ESP32-P4 OpenVela 大赛移植任务书

文档日期：2026-08-27；正式交付仓：`flash555588/contest2026_359_dengfengzaojidecuipidaxuesheng`；工作分支：`codex/esp32p4-lcd-bringup`；目标硬件：ESP32-P4 Function EV Board，芯片 revision v1.0 与 v3.2。

## 一、任务目标

基于 OpenVela 大赛指定上游和团队已经跑通的 ESP32-P4 NuttX/apps 仓，形成源码固定、双芯片版本隔离、可重复编译、可烧录、带实机证据的 OpenVela 桌面发行包。最终交付必须包含固定源码入口、v1.0/v3.2 defconfig、统一构建脚本、固件与哈希、烧录说明、测试矩阵、复现报告和 AI Coding 日志，不能依赖构建时覆盖源码的临时脚本。

## 二、当前状态摘要

截至 2026-08-27，NuttX/apps 固定提交已推送；v1.0 与 v3.2 已在第二个干净工作区全量构建成功；v1.0 的干净固件已烧录到 COM7 上的 revision v1.0 / ECO2 实板，并连续两次硬复位进入 NSH。设备当前运行 `cd61ccdd` 干净版本，`desktop_main`、`/dev/fb0`、`/dev/input0` 和 32 MiB PSRAM 已由串口确认。

2026-08-27 的 v3.2 桌面候选已完成无残留配置切换和全量编译，当时没有 revision v3.2 板，因此原记录仍是“构建候选”。后续 2026-09-19 的本地 v3 应用镜像已在 v3.2 实板完成烧录、启动、Home Assistant UI 和 BLE 触摸板验证，详见 `firmware/v3/esp32p4-application-v3-20260919.md`。

成员仓当前按团队决定保持私有，以保留领先进度。内部复现使用固定 SHA 和只读本地镜像；没有修改任何 GitHub 仓库可见性。比赛正式交付前必须再执行公开/评委可访问 gate。

## 三、固定基线

| 组件 | 固定版本 | 状态 |
| --- | --- | --- |
| 团队 NuttX | `cd61ccdd11498e22c058c3b2540828f88e23172e` | 已推送；含 v1 早期 WDT、时钟/PSRAM兼容路径及双版本配置 |
| 团队 apps | `93fb5ac72249ae766cbeea9f0e3d484bdd6807f7` | 已推送；含桌面、QuickJS/LVGL 适配 |
| LVGL | `59a6b61c9580b65089010c5273f2fcdd6c4d2aae` | v9.2.1 固定提交 |
| QuickJS 源码 | `6e2e68fd0896957f92eb6c242a2e048c1ef3cae0` | apps 构建定义固定下载提交 |
| vendor/espressif | `afe1b8c5ec67ff76eda48ee84d9dd116df2814ba` | 固定提交 |
| ESP HAL | `8d0a898910084206721a0892ab093021bca1496a` | 固定提交并应用 NuttX 仓内芯片补丁 |
| mbedTLS | `582ff482038db6e4010dbf6f943d97b05ad06ea5` | HAL 子模块固定点 |
| 工具链 | `riscv32-esp-elf` GCC `15.2.0_20251204` | 编译器 SHA-256 `921cbcc…4dca` |

内部最小同步入口为 `esp32p4-internal-release.xml`。大赛完整 manifest 仍保留在 `openvela.xml`，但日常保密复现优先使用最小固定入口，减少无关浮动项目和网络失败面。

## 四、已经完成的工作

### 1. 源码和芯片版本隔离

ESP32-P4 板级、芯片和桌面源代码已进入完整 NuttX/apps 成员仓，不再依靠大赛仓 linkfile 覆盖 NuttX 已跟踪路径。`desktop-v1` 固定 revision v1.0 兼容路径；`desktop` 固定 `CONFIG_ESP32P4_REV_MIN_301=y`，对应 v3.x。两套解析配置分别保存到固件包，防止烧错芯片。

### 2. 黑屏启动根因修复

早期清洁候选在 ROM `B3` 后循环 `CHIP_LP_WDT_RESET`，只有黑屏。修复已进入 NuttX 固定提交：Simple Boot 入口关闭 timer-group 与 LP flashboot watchdog；低于 v3 的配置使用实机验证的 v1.0 bias、90 MHz CPLL 和 PSRAM 初始化顺序；v3.x 保持上游式 400 MHz 路径。修复后不再依赖临时补丁脚本。

### 3. 构建脚本可靠性

`tools/build_esp32p4_desktop.sh` 支持 `v1.0`、`v3.2` 两个参数，强制 GCC 15.2.0，自动定位 `kconfig-tweak`，用 `configure.sh -E -S` 清理并切换配置，输出 BIN/ELF/HEX、defconfig、解析配置、完整依赖元数据和 SHA256SUMS。脚本已经实际执行 `v1.0 → v3.2` 顺序并通过。

### 4. 干净目录复现

匿名同步私有团队仓会因权限失败，已留作保密期权限证据。随后使用本地只读镜像装入相同固定提交，在新的 `.repo` 工作区同步公共依赖并完成两次全量构建。第一次试验还发现并修复了 `kconfig-tweak` 路径和配置残留问题；最终成功过程见 `REPRODUCIBILITY_REPORT_2026-08-27.md`。

### 5. v1.0 实机验收

| 文件 | 大小 | SHA-256 |
| --- | ---: | --- |
| `firmware/历史测试固件/esp32p4-desktop-v1.0-release/nuttx.bin` | 2908784 | `74420C66BE6A0298DBBEB21326600D47010612C27A097030DF4E47CC90E2C058` |

esptool 5.3.1 识别芯片为 revision v1.0 / ECO2，MAC `60:55:f9:fa:f4:8b`，镜像写入 `0x2000` 并校验通过。连续两次硬复位均进入 NSH，版本为 `NuttX 0.0.0 cd61ccdd`，无 dirty 标记。证据位于 `logs/v1-repro2-smoke-20260827.log` 和 `logs/v1-repro2-reset-cycle2-20260827.log`。

### 6. v3.2 构建候选

| 文件 | 大小 | SHA-256 |
| --- | ---: | --- |
| `firmware/v3/esp32p4-desktop-v3.2-candidate/nuttx.bin` | 2954848 | `886A9FB5793F6EE17285C8BBE373FEFAE5450DD5BD95FF19A4E2A634B07FE80E` |

v3.2 解析配置为 revision ≥3.1、400 MHz，低于 v3 的兼容选项关闭。构建日志为 `logs/build-repro-v3.2-20260827.log`。由于没有对应实板，本包不能升级为 release。

## 五、剩余工作和验收标准

| 优先级 | 工作包 | 详细动作 | 完成证据/验收标准 |
| --- | --- | --- | --- |
| P0 | v1.0 实际画面 | 操作者检查桌面是否点亮；确认无全黑/全白、花屏、偏色、撕裂；记录屏幕照片 | 照片能识别桌面；颜色、方向、分辨率与设计一致；若仍黑屏，采集背光、电源、DSI 初始化和 framebuffer 首帧证据 |
| P0 | v1.0 触摸 | 依次点击四角与中心，执行按下、拖动、释放；核对旋转/镜像和坐标边界 | 五点可达；不越界、不镜像；按下/移动/释放事件完整；保存串口或视频记录 |
| P0 | v1.0 冷启动 | 完全断电后启动 5 次，每次记录 NSH、桌面任务、fb0/input0 和复位原因 | 5/5 成功；无 WDT、panic、异常或偶发黑屏 |
| P0 | v1.0 稳定性 | 桌面持续刷新 30 分钟；周期记录 `ps`、`free`；执行 PSRAM 分配/释放压力 | 无死机、WDT、明显泄漏、触摸失效或 framebuffer 停止刷新 |
| P0 | v3.2 实机 | 获取真正 revision v3.2 板；先读芯片 ID，再烧 v3.2 候选；完成 5 次冷启动和全外设矩阵 | 芯片 ID 正确；UART/USB、LCD、GT911、PSRAM、桌面全部通过；形成独立日志、照片和测试报告 |
| P0 | 最终访问策略 | 保密期结束后选择公开成员仓固定提交，或把完整源码快照纳入评委可访问交付 | 未登录环境可 clone/fetch 所有 manifest 固定提交；不能只依赖本机镜像 |
| P1 | v3.2 USB 变体 | 验证 USB console 枚举、断开重连、长输出和复位恢复 | 重连后可进入 NSH；无永久卡死；失败时仅发布 UART 版 |
| P1 | 发布目录收敛 | 隔离历史失败候选；校验每个 README、flash 脚本、TEST_REPORT、元数据和 SHA256SUMS | 正式目录无状态不明固件；BIN 可反查源码、配置、工具链和实机日志 |
| P1 | AI Coding 日志 | 在 `.repo` 工作区结束本次会话后运行大赛日志清单校验，补齐 contest collector 产物 | `logs/flash555588/manifest.json` 中每个会话存在且哈希匹配；无密钥和隐私数据 |
| P1 | 最终 gatekeeper | 检查 git 状态、文件大小、许可证、敏感信息、manifest XML、构建/烧录命令和链接 | 全部检查通过或有书面豁免；记录最终提交号和 release/tag |

## 六、下一执行顺序

第一步由操作者立即确认当前 v1.0 板的 LCD 是否已点亮；若仍黑屏，保持当前可进入 NSH 的固件不回退，从背光使能、DSI 数据输出和 framebuffer 内容三层定位。第二步完成五点触摸、5 次断电冷启动和 30 分钟稳定性。第三步在拿到 v3.2 实板后完成独立硬件矩阵。最后才开启评委访问、收敛 AI 日志并打最终 release/tag。

本地开发余量的提交边界、历史硬件证据和 framebuffer 诊断工具使用方法见 `DEVELOPMENT_RESERVE_2026-08-27.md`。这些资产用于后续排障，不改变 v1.0/v3.2 当前发布等级。

## 七、当前风险边界

当前最高技术风险已经从“无法启动”降为“LCD/触摸缺少操作者目视证据”。当前最高交付风险是成员仓仍为私有；这是主动保密选择，不是被忽略的问题，但必须在比赛交付门前解决。v3.2 没有实板，任何文档都必须继续使用“构建候选”措辞。大赛仓存在用户历史修改和诊断文件，提交时必须继续按精确文件清单暂存，不能把无关二进制或回退代码带入正式提交。
