# ESP32-P4 OpenVela 可复现构建与实机验证报告

报告日期：2026-08-27；分支：`codex/esp32p4-lcd-bringup`；范围：ESP32-P4 Function EV Board 桌面固件 v1.0 与 v3.2。

## 结论

固定源码的双版本干净构建已经完成。v1.0 固件已在 revision v1.0 / ECO2 实板上连续两次硬复位进入 NSH，`desktop_main`、`/dev/fb0`、`/dev/input0` 和 32 MiB PSRAM 均通过；v3.2 在相同干净工作区清理切换后编译成功，但因没有 v3.2 实板，只能作为构建候选。

成员 NuttX/apps 仓按团队决策暂不公开。此阶段的复现使用固定 SHA 与本地只读镜像完成，不改变远端可见性。正式评审前仍必须开启匿名可读，或提供包含完整固定源码的评委可访问快照。

## 固定输入

| 输入 | 固定值 |
| --- | --- |
| NuttX | `cd61ccdd11498e22c058c3b2540828f88e23172e` |
| apps | `93fb5ac72249ae766cbeea9f0e3d484bdd6807f7` |
| LVGL | `59a6b61c9580b65089010c5273f2fcdd6c4d2aae` |
| QuickJS 源码 | `6e2e68fd0896957f92eb6c242a2e048c1ef3cae0` |
| vendor/espressif | `afe1b8c5ec67ff76eda48ee84d9dd116df2814ba` |
| ESP HAL | `8d0a898910084206721a0892ab093021bca1496a` |
| mbedTLS | `582ff482038db6e4010dbf6f943d97b05ad06ea5` |
| GCC | `riscv32-esp-elf 15.2.0_20251204` |
| GCC 可执行文件 SHA-256 | `921cbcc885a69ac3d4eda619b5b0bf1e29b83b2f169754711df8428c4e8a4dca` |

内部最小 manifest 为 `esp32p4-internal-release.xml`。它只固定实际需要的团队仓、LVGL 和 vendor/espressif；QuickJS 与 ESP HAL 的下载固定点由已固定的 apps/NuttX 构建定义控制，并在 `BUILD-METADATA.txt` 中再次记录。

## 清洁复现过程

第一次空目录试验验证了远端权限边界：未登录 GitHub 时无法同步私有团队仓，这是当前保密策略的预期结果，不是源码固定失败。随后从本地只读镜像装入同一提交，公共依赖按固定 SHA 同步，在第二个全新工作区执行完整构建。

构建脚本在复现中暴露并修复了两个问题：受限 `PATH` 丢失 `kconfig-tweak`；重复构建仅用 `configure.sh -S` 会保留失败配置。当前脚本先定位 `kconfig-tweak`，并使用 `configure.sh -E -S` 强制清理后配置，因此可以在同一工作区可靠执行 `v1.0 → v3.2` 切换。

构建命令为：

```bash
export OPENVELA_ROOT=/path/to/openvela-workspace
export ESP_RISCV_TOOLCHAIN=/path/to/riscv32-esp-elf/bin
export BUILD_JOBS=8
bash /path/to/contest-repo/tools/build_esp32p4_desktop.sh v1.0
bash /path/to/contest-repo/tools/build_esp32p4_desktop.sh v3.2
```

两次构建均返回 0。v1.0 日志的历史路径为
`logs/build-repro-v1.0-20260827.log`，可从清理前提交 `f15c0fe4` 查阅；
v3.2 日志仍位于 `logs/build-repro-v3.2-20260827.log`。

## 产物矩阵

| 版本 | 配置 | BIN 大小 | SHA-256 | 状态 |
| --- | --- | ---: | --- | --- |
| v1.0 | `desktop-v1` | 2908784 | `74420c66be6a0298dbbeb21326600d47010612c27a097030df4e47cc90e2c058` | 干净构建、实机双复位通过 |
| v3.2 | `desktop` | 2954848 | `886a9fb5793f6ee17285c8bbe373fefae5450dd5bd95ff19a4e2a634b07fe80e` | 干净构建通过、待 v3.2 实机 |

v1.0 解析配置固定 `CONFIG_ESP32P4_REV_MIN_100=y`、`CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`；v3.2 固定 `CONFIG_ESP32P4_REV_MIN_301=y`，并关闭低于 v3 的兼容选项。两者均启用 MIPI DSI、GT9xx、framebuffer、LVGL、QuickJS 和 PSRAM，但启动时钟与芯片兼容路径相互隔离。

## v1.0 实机证据

连接设备为 ESP32-P4 revision v1.0 / ECO2，MAC `60:55:f9:fa:f4:8b`，COM7。esptool 5.3.1 将镜像写入 `0x2000`，写后校验通过。连续两次自动硬复位均输出 `NuttX 0.0.0 cd61ccdd`，不是 dirty 工作树版本。

运行态确认 `desktop_main` 处于等待信号状态，`/dev/fb0` 与 `/dev/input0` 已注册，
用户堆总量为 33554428 bytes。完整证据的历史路径为
`logs/v1-repro2-smoke-20260827.log` 和 `logs/v1-repro2-reset-cycle2-20260827.log`，
可从清理前提交 `f15c0fe4` 查阅。

## 尚未完成的比赛门槛

当前自动化证据不能代替操作者目视 LCD。v1.0 仍需确认实际画面不是黑屏、颜色与方向正确，并完成触摸四角、拖动和释放测试；还需完成 5 次断电冷启动和 30 分钟刷新/PSRAM 稳定性测试。

v3.2 必须在真实 revision v3.2 芯片上重复芯片 ID、烧录、5 次冷启动、LCD、触摸、PSRAM、桌面与控制台验收。在此之前不得把候选标记为正式发布。

保密期结束进入交付门时，需要让评委匿名访问所有固定提交，或将完整源码快照纳入可访问交付；随后运行敏感信息、许可证、文件大小、manifest 和 AI Coding 日志 gatekeeper，并记录最终提交/tag。
