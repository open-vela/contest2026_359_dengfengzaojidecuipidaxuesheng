# ESP32-P4 openvela 图形终端与智能家居集成

本仓库是 `contest2026_359_dengfengzaojidecuipidaxuesheng` 的参赛与工程交付仓，
面向 ESP32-P4 Function-EV-Board，保存 openvela/NuttX 板级与芯片 overlay、图形桌面、
QuickJS 快应用、Home Assistant 原生集成、构建工具和验证记录。

它不是完整的 NuttX/openvela 源码树，也不是 Home Assistant 官方客户端或小米官方米家客户端。
阅读代码可直接克隆本仓库；固件构建还需要匹配的外部源码、工具链和具体硬件。

## 从哪里开始

先阅读[目录说明](#目录导航)，再按目标选择入口：板级适配见 [board](board/README.md)，
芯片驱动见 [chip](chip/README.md)，应用开发见 [app](app/README.md)，
快应用见 [quickapp](quickapp/README.md)，构建与提交工具见 [tools](tools/README.md)。

当前 v3 应用开发快照位于 [app/espdl-quickapp](app/espdl-quickapp/README.md)。
Home Assistant 的实际入口、网络限制和实板证据见 [HASS.md](app/espdl-quickapp/HASS.md)。
当前固件入口请从 [firmware/v3](firmware/v3/README.md) 开始；v1 和早期实验材料集中在
[firmware/历史测试固件](firmware/历史测试固件/)，不要把文件夹日期或“候选”名称当作兼容性保证。

## 版本与验证边界

仓库中并存三类材料，不能互相替代：历史 v1.x 固件及其验收记录、针对不同 revision 的
板级配置，以及 2026-09-17 整理的 v3 应用 overlay 与应用级证据。

[2026-08-30 v1.0 测试报告](firmware/历史测试固件/esp32p4-desktop-v1/TEST_REPORT.md)记录了对应镜像的
烧录校验、冷启动、PSRAM 和桌面/QuickJS 状态；[2026-08-27 v3.2 构建候选](firmware/v3/esp32p4-desktop-v3.2-candidate/README.md)
是独立的旧候选。2026-09-19 的实际 v3 应用实板验证见 [v3 应用记录](firmware/v3/esp32p4-application-v3-20260919.md)，不能把两种镜像混为同一版本。

`483a750` 是 Home Assistant 原生集成的一次内容基准，不是把整个仓库固定到旧提交的要求。
旧源码集合及独立 worktree 的取回方法见 [docs/HISTORY.md](docs/HISTORY.md)。
请勿把历史快照整体覆盖回当前参赛结构。

## 适配亮点

板级工作包括 Simple Boot、PSRAM、MIPI-DSI framebuffer、GT911 触摸和 LVGL 桌面。
不同芯片 revision 的时钟、内存布局和显示参数不能混用；具体配置与历史验证状态见
[板级说明](board/esp32p4-function-ev-board/README.md)和[配置索引](configs/README.md)。

应用层包含 OuO、相机、番茄钟和 Home Assistant 快应用；v3 overlay 另保存 ESP-DL、
聊天、音乐、录音机、设备门户、桌宠和硬件 API。功能存在于源码中不等于已完成外部服务或实板验收，
必须同时阅读[应用文档](app/espdl-quickapp/README.md)与[验证证据说明](app/espdl-quickapp/evidence/README.md)。

Home Assistant 原生页面使用共享服务；米家设备需要先接入用户自己的 Home Assistant。
该路径只支持受限局域网 HTTP，令牌会明文传输，仅应在可信隔离网络使用。
旧兼容桥的 HTTPS 尚未实现，不能把两条后端的文档混为一谈，详见
[Home Assistant 源码说明](app/homeassistant/README.md)。

## 目录导航

```text
app/        原生应用、内置资源与 v3 应用开发快照
board/      开发板与公共板级支持
chip/       ESP32-P4 架构、驱动及 Espressif 适配
configs/    桌面配置片段；不能替代板级 configs
quickapp/   QuickJS 快应用的可编辑源码与 manifest
ouo/        OuO 快应用、设计说明和独立许可证
firmware/   按版本归档的镜像、校验文件和历史报告
tools/     构建、overlay、补丁、自检、烧录和宿主测试工具
docs/      文档导航、历史边界及专题证据
logs/      AI Coding 会话、清单以及历史构建/串口日志
```

[app](app/README.md) · [board](board/README.md) · [chip](chip/README.md) ·
[configs](configs/README.md) · [quickapp](quickapp/README.md) · [OuO](ouo/README.md) ·
[firmware](firmware/README.md) · [tools](tools/README.md) · [docs](docs/README.md) · [logs](logs/README.md)

## 获取工程

只查看本仓库源码与文档：

```bash
git clone --branch dev-ai-contest-2026 https://github.com/open-vela/contest2026_359_dengfengzaojidecuipidaxuesheng.git
cd contest2026_359_dengfengzaojidecuipidaxuesheng
```

建立完整 openvela 工作区时，在独立目录使用仓库提供的 manifest：

```bash
repo init -u https://github.com/open-vela/contest2026_359_dengfengzaojidecuipidaxuesheng \
  -b dev-ai-contest-2026 \
  -m contest2026_359_dengfengzaojidecuipidaxuesheng.xml
repo sync -c -j8
```

注意：该 manifest 的团队项目 `revision` 当前写为 `codex/espclaw-final`，而不是
`dev-ai-contest-2026`。上述命令选择的是 manifest 分支，不保证团队源码自动切到本 README
对应的提交。同步前请检查 [团队 manifest](contest2026_359_dengfengzaojidecuipidaxuesheng.xml)
和 [openvela.xml](openvela.xml)，同步后核对实际提交，不要直接重置已有开发分支。

## 构建

构建环境以 Linux/WSL 为主，需要 Python 3、Git、构建依赖和匹配的 `riscv32-esp-elf`
工具链。Node.js 用于宿主 JavaScript 测试。这里不把宿主测试通过称为固件构建通过。

### 板级 NSH 与开发 overlay

`tools/wsl_build_p4_nsh.py` 接受 `nsh`、`nsh-v3`、`nsh-v3-usb`，通过
`OPENVELA_ROOT` 选择外部工作区。例如在本仓库根目录运行：

```bash
export OPENVELA_ROOT=/path/to/vela-p4
python3 tools/wsl_build_p4_nsh.py nsh
```

这些开发脚本会改动外部编译树。部分 overlay/归档脚本仍包含作者机器路径，运行前必须检查
源目录、目标目录及本地改动；尤其不能把 `wsl_copy_firmware.py` 当作已完成路径参数化的通用工具。
参见[工具说明](tools/README.md)。

### 历史 v1.x 桌面与相机补丁链

[apply_final_overlays.sh](tools/apply_final_overlays.sh)固定要求 NuttX
`2f1387d56eb04ad2599baca58a3fa2380cdaaedb` 和 apps
`88827afd368d4bbb4802b96ed44d9582f85b2f92`。这不是向任意最新 openvela 树应用补丁的入口，
也不等价于把 v3 应用快照全部集成进固件。

仅在独立工作区的基线匹配、依赖齐备并确认无未保存修改后，从本仓库根目录执行：

```bash
bash tools/apply_final_overlays.sh
cd ../nuttx
tools/configure.sh esp32p4-function-ev-board:desktop-v1
make CROSSDEV=/path/to/riscv32-esp-elf/bin/riscv32-esp-elf- -j16
```

补丁顺序、摘要和幂等检查以脚本及 [patches 说明](tools/patches/README.md)为准。
[内部发布 manifest](esp32p4-internal-release.xml)包含另一组固定提交及团队仓库依赖；
不要假定它与上述补丁基线一致或所有依赖都已公开。公共上游映射的待办见
[UPSTREAM_PLAN.md](UPSTREAM_PLAN.md)。

## 镜像选择与烧录

先按[固件索引](firmware/README.md)核对芯片 revision、板卡、镜像来源和 `SHA256SUMS`。
历史 v1.0 镜像不能直接用于 v3.2，反之亦然。端口号必须替换为实际设备端口。

下面只预览命令，不执行烧录：

```powershell
powershell -File tools\flash_p4_nsh.ps1 -Variant v1.x -Port COM7 -Transport uart-bridge -DryRun
```

该脚本针对其 NSH Simple Boot 镜像，将应用写到 `0x2000`，并按 `/data` 位于 `0x400000`
做大小边界检查。不要用它直接烧录采用不同分区布局的 v3 应用或模型镜像，也不要从其他版本
照抄地址、Flash 参数或烧录命令。真正烧录前必须完成硬件、分区和镜像确认。

## 验证

在仓库根目录执行已有的包检查与提交检查：

```bash
python3 tools/check_package.py
python3 tools/check_submission.py
node --test app/homeassistant/tests/homeassistant.test.cjs
node --test app/espdl-quickapp/tests/dafeiyu.test.cjs app/espdl-quickapp/tests/package.test.cjs
```

`check_submission.py` 会调用包检查，并检查 README 必需章节、会话清单及日志格式。
包检查还会核对固件摘要、补丁格式和仓库卫生；这些检查不能替代全量许可证审查或实板验收。
测试所需环境及换行符注意事项见[测试说明](app/espdl-quickapp/tests/README.md)。

板上验收至少应区分：构建与链接成功、烧录写入校验、冷启动、显示/触摸、应用功能以及
外部服务联调。历史证据入口为 [docs/evidence](docs/evidence/README.md)、
[应用 evidence](app/espdl-quickapp/evidence/README.md)和各固件目录的报告。
文档重写不构成一次新的硬件验证。

## AI Coding 使用说明

AI 用于需求拆解、移植、问题定位、测试和文档整理。选定的真实会话位于
[logs/flash555588](logs/flash555588/README.md)，以该目录的 `manifest.json` 为清单入口。
这里还保留历史构建和串口记录，它们不是 AI 对话日志，不能互相替代。

提交日志前应检查个人信息、凭据和第三方内容；不要编造会话、补写假的工具结果，
也不要把整个用户配置目录或设备 `/data` 上传到仓库。

## 许可证、第三方内容与品牌

当前仓库没有统一的根级 `LICENSE` / `NOTICE`，不能把整个仓库直接标成
“全部 Apache 2.0”或据此作出完整商用授权承诺。请按文件声明、组件自带许可证和
[THIRD_PARTY.md](THIRD_PARTY.md)核对实际来源与分发义务；台账也需要随依赖更新复核。

Apache 2.0 包含特定范围的专利许可，但不授予一般商标使用权；再分发时还需遵守许可证、
适用声明、修改说明及 NOTICE 等要求，见 [Apache 官方条款](https://www.apache.org/licenses/LICENSE-2.0)。
调用 Home Assistant 不代表获得其品牌、其他厂商 Logo、文档或所有依赖的统一授权。
本项目不代表 Home Assistant、小米或其他厂商的官方背书。

## 维护与提交

新增功能应同时更新最近一层目录 README、配置、测试及必要的来源记录。源码、生成文件、
实板证据应明确区分；不要为了缩小 diff 而删除许可证，也不要把凭据或临时 ELF/MAP 塞入源码提交。

本目录结构用于清晰交付，不要求在第三方库、生成缓存和每个日期日志目录下重复添加 README。
历史版本查看[版本说明](docs/HISTORY.md)，后续公共仓拆分查看[上游计划](UPSTREAM_PLAN.md)。

## 2026-09-19 Validation Records

This submission follows the existing cloud repository layout and archives the latest local work, experiments, flash logs, and device tests. The source workspace was not moved, deleted, or modified.

See [the work log](docs/WORKLOG_2026-09-19.md) and the raw [application evidence](app/espdl-quickapp/evidence/README.md). The final image is 7096868 bytes with SHA-256 `25f42781e8cd0dfe3a814f0a7b9e83b7b86910887f090860655e310275fdd6fe`; only the program partition was flashed, preserving models and `/data`.
The current application source is synchronized from the locally verified 04-v3-20260913/espdl-quickapp tree, including the Home Assistant UI and BLE touchpad implementation. The old cloud v1 snapshot is not the release baseline. See the 2026-09-19 handoff logs at logs/2026-09-19/README.md.
