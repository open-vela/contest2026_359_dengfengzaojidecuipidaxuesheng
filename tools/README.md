# tools：构建、检查与维护工具

本目录包含不同阶段的工具，不是所有脚本都适合直接在新机器或任意源码树运行。
返回[仓库首页](../README.md)。

## 先检查运行条件

固件构建主要在 Linux/WSL 中运行。`wsl_build_p4_nsh.py` 通过 `OPENVELA_ROOT` 指定目标，
其工具链路径位于目标下的 `riscv32-esp-elf/bin`。但 `wsl_copy_firmware.py` 的源、目标目录仍
是作者机器路径；`apply_esp32p4_overlay.py` 也保留 Windows/WSL 路径假设。
在审查源码、确认目标树干净前，不要运行会覆盖、重配或归档外部工作区的工具。

## 构建与集成入口

官方 ESP-TTS、AI 家居操控与实板回归见
[完整流程](../app/espdl-quickapp/speech/esp-tts/WORKFLOW.md)。完整 v3 构建启用语音后默认使用
ESP-TTS；`verify_esp_tts.py` 检查依赖、音频回归和实际镜像，`deploy_esp_tts_device.py`
在已有应用区备份及镜像校验报告的前提下完成上板、播报检查和失败恢复。

[apply_final_overlays.sh](apply_final_overlays.sh)是历史发布的固定基线补丁入口，顺序与约束见
[patches](patches/README.md)。它和开发期直接覆盖的 [apply_esp32p4_overlay.py](apply_esp32p4_overlay.py)
不是同一流程。`build_esp32p4_desktop.sh` 要求 `v1.0` 或 `v3.2` 参数及
`ESP_RISCV_TOOLCHAIN`，不能从任意目录无参数执行。

## 只读自检

米家设备控制开发 skill 的可安装源码见
[skills/mijia-device-control](skills/mijia-device-control/SKILL.md)，安装和提交范围见
[源码集成说明](../docs/INTEGRATION_2026-10-19.md)。

在仓库根目录运行：

```bash
python3 tools/check_package.py
python3 tools/check_submission.py
```

前者检查交付结构、固件摘要、补丁及仓库卫生；后者还检查 README 和真实 AI 日志清单。
二者不能代替构建、硬件测试或完整法律合规审查。

## 烧录与可选无线

[flash_p4_nsh.ps1](flash_p4_nsh.ps1)必须提供 `-Port` 和 `-Transport`；先使用 `-DryRun`
预览命令，镜像边界见 [firmware](../firmware/README.md)。串口脚本会打开设备，避免与监视器抢占端口。

C6 源码片段见 [c6](c6/README.md)，集成状态及限制见 [README.c6.md](README.c6.md)，
宿主测试入口见 [tests](tests/README.md)。不要把 C6 离线构建记录当作无线连接的实板证明。
