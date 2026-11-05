# ESP32-P4 openvela 图形终端与智能家居集成

本仓库是 `contest2026_359_dengfengzaojidecuipidaxuesheng` 的参赛与工程交付仓，
面向 ESP32-P4 Function-EV-Board，保存 openvela/NuttX 板级与芯片 overlay、图形桌面、
QuickJS 快应用、Home Assistant 原生集成、构建工具和验证记录。

仓库另收录 [ProgrammableMatrix / JBOX-S3](app/ProgrammableMatrix/README.md)，
用于 ESP32-S3 与 4 颗 CH446Q 的无线 IO 扩展和矩阵连线，包含固件、Python SDK、
可视化连线工具及 JBoxScript。该工程使用 ESP-IDF 5.5.x 独立构建。

它不是完整的 NuttX/openvela 源码树，也不是 Home Assistant 官方客户端或小米官方米家客户端。
阅读代码可直接克隆本仓库；固件构建还需要匹配的外部源码、工具链和具体硬件。

## 从哪里开始

先阅读[目录说明](#目录导航)，再按目标选择入口：板级适配见 [board](board/README.md)，
芯片驱动见 [chip](chip/README.md)，应用开发见 [app](app/README.md)，
快应用见 [quickapp](quickapp/README.md)，构建与提交工具见 [tools](tools/README.md)。

无线矩阵板入口见 [ProgrammableMatrix](app/ProgrammableMatrix/README.md)，
编译命令见下方 [JBOX-S3 构建](#programmablematrix--jbox-s3-构建)。

当前 v3 应用开发快照位于 [app/espdl-quickapp](app/espdl-quickapp/README.md)。
Home Assistant 的实际入口、网络限制和实板证据见 [HASS.md](app/espdl-quickapp/HASS.md)。
当前固件入口请从 [firmware/v3](firmware/v3/README.md) 开始。v1 固件归档已从当前文件树清理，
仍可从 Git 历史查阅；不要把文件夹日期或“候选”名称当作兼容性保证。

## 版本与验证边界

2026-11-05 的[输入框词库预加载与 CSI 预览退出验收](docs/evidence/ime-camera-preload-20261105/README.md)
记录了已烧录镜像 `7fc37780` 的首字输入、内存及实体返回结果；冻结镜像与主站合并后的宿主验证范围分别记录。

仓库当前保留针对不同 revision 的板级配置，以及 v3 应用 overlay 与应用级证据。
历史 v1.x 固件及其验收记录保存在 Git 历史中，不作为当前交付镜像。

[2026-08-27 v3.2 构建候选](firmware/v3/esp32p4-desktop-v3.2-candidate/README.md)
是独立的旧候选。2026-09-19 的实际 v3 应用实板验证见 [v3 应用记录](firmware/v3/esp32p4-application-v3-20260919.md)，不能把两种镜像混为同一版本。
如需复查 2026-08-30 的 v1.0 验收报告，可从清理前提交
`50e354d2` 的 `firmware/历史测试固件/esp32p4-desktop-v1/TEST_REPORT.md` 读取。

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
app/        原生应用、内置资源、v3 应用快照与 ProgrammableMatrix 独立工程
board/      开发板与公共板级支持
chip/       ESP32-P4 架构、驱动及 Espressif 适配
configs/    桌面配置片段；不能替代板级 configs
quickapp/   QuickJS 快应用的可编辑源码与 manifest
ouo/        OuO 快应用、设计说明和独立许可证
firmware/   当前 v3 镜像和校验文件；v1 归档保存在 Git 历史
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

P4/openvela 构建环境以 Linux/WSL 为主，需要 Python 3、Git、构建依赖和匹配的 `riscv32-esp-elf`
工具链。Node.js 用于宿主 JavaScript 测试。这里不把宿主测试通过称为固件构建通过。

### ProgrammableMatrix / JBOX-S3 构建

源码位于 [app/ProgrammableMatrix](app/ProgrammableMatrix/)，同步自
[streetartist/ProgrammableMatrix](https://github.com/streetartist/ProgrammableMatrix)，
上游 `main` 提交固定为 `2ca78dcb9c9b825b943da3b36e7baf19f558d91d`（2026-11-04 同步）。
完整文件摘要见 [UPSTREAM.json](app/ProgrammableMatrix/UPSTREAM.json)。

目标硬件为 ESP32-S3-WROOM-1-N16R8 + 4×CH446Q。安装 ESP-IDF **5.5.x**，
在已经启用 ESP-IDF 环境的终端中，从本仓库根目录执行 `idf.py build`。
Linux / WSL 首次安装与完整编译可直接按以下顺序操作（已有该 IDF 目录时跳过 clone/install）：

```bash
# 此时位于本仓库根目录；P4 构建虚拟环境与 ESP-IDF 环境请使用不同终端。
espclaw_repo="$(pwd)"
sudo apt-get update
sudo apt-get install -y git wget flex bison gperf python3 python3-pip python3-venv \
  cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0
mkdir -p "$HOME/esp"
git clone --branch v5.5 --recursive https://github.com/espressif/esp-idf.git \
  "$HOME/esp/esp-idf-v5.5"
cd "$HOME/esp/esp-idf-v5.5"
./install.sh esp32s3
source ./export.sh

cd "$espclaw_repo/app/ProgrammableMatrix"
idf.py set-target esp32s3
idf.py build
ls -lh build/programmable_matrix_s3.bin build/bootloader/bootloader.bin \
  build/partition_table/partition-table.bin
```

Windows 原生终端可在 ESP-IDF 5.5.x 安装器提供的 ESP-IDF PowerShell 中执行：

```powershell
cd app/ProgrammableMatrix
idf.py set-target esp32s3
idf.py build
```

`sdkconfig.defaults` 提供 16MB Flash、Octal 8MB PSRAM、BLE 配网和 Wi-Fi 默认配置；
`partitions.csv` 提供 4MB 应用分区。产物位于该子目录的 `build/`，
包括 `programmable_matrix_s3.bin`。需调整设置时运行 `idf.py menuconfig`。

Python 客户端与可视化工具从该子目录启动：

```powershell
python client/jbox_gui.py
```

宿主测试使用 `python -m unittest discover -s tests -p 'test_*.py' -v`；
本机安装 GCC 后，可运行 `python tests/run_native_tests.py` 检查路由与脚本虚拟机。
首次配网、GPIO10 授权按钮、引脚映射和接线方法见该工程的
[README](app/ProgrammableMatrix/README.md)。此处同步保留上游源码，
P4/openvela 固件仍使用下面的构建入口。

### P4 主固件：从干净目录构建

以下命令面向 **Ubuntu 24.04 x86_64 / 对应 WSL2 环境**，目标为 P4 Function EV Board
rev3.2、16 MiB Flash、32 MiB PSRAM、Simple Boot。Windows 请先进入 WSL Bash；
其他 Linux 发行版按自己的包管理器安装相同工具。构建过程不连接串口。

本仓库提供应用与板级 overlay，完整 NuttX/Apps/HAL 输入由锁定附件补齐。
无需先执行上面的 `repo init`，也无需作者电脑上的 `/home/flash` 或 D 盘目录。

**1. 安装宿主工具并获取主线源码**

```bash
sudo apt-get update
sudo apt-get install -y build-essential git curl ca-certificates python3 python3-venv \
  bison flex gperf pkg-config libncurses-dev kconfig-frontends \
  automake autoconf libtool cmake ninja-build unzip xz-utils

mkdir -p "$HOME/espclaw-work"
cd "$HOME/espclaw-work"
git clone --branch dev-ai-contest-2026 \
  https://github.com/open-vela/contest2026_359_dengfengzaojidecuipidaxuesheng.git source
cd source

python3 -m venv "$HOME/espclaw-work/host-tools"
source "$HOME/espclaw-work/host-tools/bin/activate"
python -m pip install esptool==5.2.0
command -v kconfig-conf
esptool.py version
```

Ubuntu 的 [kconfig-frontends 包](https://packages.ubuntu.com/noble/kconfig-frontends) 位于
universe 软件源；安装找不到时先启用该源。Python 需提供 `tarfile.data_filter`。
重复构建时进入已有 `source`，重新激活上述虚拟环境即可，不必重复 clone。

**2. 下载固定工具链和 OpenH264，核对摘要**

```bash
mkdir -p "$HOME/espclaw-work/downloads" "$HOME/espclaw-work/toolchains"

curl --fail --location --retry 3 \
  https://github.com/espressif/crosstool-NG/releases/download/esp-14.2.0_20251107/riscv32-esp-elf-14.2.0_20251107-x86_64-linux-gnu.tar.gz \
  --output "$HOME/espclaw-work/downloads/gcc14.tar.gz"
printf '%s  %s\n' \
  051403b578f5c76ab5fbadb5171ec580df7695b16657a97d27e728b92ba64854 \
  "$HOME/espclaw-work/downloads/gcc14.tar.gz" | sha256sum --check
# 上一条校验成功后再解压；首次安装到新的工具链目录。
tar -xzf "$HOME/espclaw-work/downloads/gcc14.tar.gz" \
  -C "$HOME/espclaw-work/toolchains"

curl --fail --location --retry 3 \
  https://github.com/cisco/openh264/archive/refs/tags/v2.6.0.tar.gz \
  --output "$HOME/espclaw-work/downloads/openh264-2.6.0.tar.gz"
printf '%s  %s\n' \
  558544ad358283a7ab2930d69a9ceddf913f4a51ee9bf1bfb9e377322af81a69 \
  "$HOME/espclaw-work/downloads/openh264-2.6.0.tar.gz" | sha256sum --check

export TOOLCHAIN="$HOME/espclaw-work/toolchains/riscv32-esp-elf/bin"
export PATH="$TOOLCHAIN:$PATH"
riscv32-esp-elf-gcc --version
```

工具链来自 [Espressif 固定 Release](https://github.com/espressif/crosstool-NG/releases/tag/esp-14.2.0_20251107)，
使用 GCC 14.2.0 及其 ILP32F 库；不要替换成宿主 GCC 或其他版本的交叉编译器。
逐条执行并确认下载、摘要检查成功后继续，网络失败时不跳过校验。

**3. 一条命令准备输入并编译基础配置**

```bash
python3 tools/build_p4_from_readme.py \
  --toolchain "$TOOLCHAIN" \
  --archive "$HOME/espclaw-work/downloads/openh264-2.6.0.tar.gz" \
  --cache "$HOME/espclaw-work/input-cache" \
  --inputs "$HOME/espclaw-work/inputs" \
  --workspace "$HOME/espclaw-work/build-base" \
  --output "$HOME/espclaw-work/out/base" \
  --jobs 6
```

新增入口 [build_p4_from_readme.py](tools/build_p4_from_readme.py) 依次调用
[prepare_p4_repro_inputs.py](tools/prepare_p4_repro_inputs.py) 和
[build_p4_repro.py](tools/build_p4_repro.py)，下载三份固定输入并校验长度、SHA-256、
逐文件清单和安全解压路径，再使用当前 checkout 的源码构建。
追加 `--dry-run` 只打印两条命令，不下载、不编译；追加 `--offline` 从已有 cache 准备输入。
独立输入目录与编译目录不可互相嵌套，脚本拒绝混用。

该基础配置启用 USB 相机、PSRAM XIP、硬件 JPEG、以太网共享、BLE 八连接测试配置和
LCD 背光；**不是带齐离线双 AI/ASR/TTS 模型的 board109 完整运行环境**。
完整锁定清单、离线准备及配置说明见 [P4 复现文档](docs/P4_BUILD_REPRO_2026-10-27.md)。

**4. board109 完整功能配置**

已有兼容的完整输入时，使用下面的完整命令。先将四个输入路径改为实际准备目录；
`P4_BASELINE` 应指向兼容的已准备 NuttX/Apps 基线根目录，不能指向本 overlay 仓库。
语音 SDK、离线模型和配套 C6 固件需另外准备，基础输入下载器不会自动提供这些全部材料。

```bash
export P4_BASELINE=/path/to/compatible-prepared-p4-baseline
export SPEECH_RUNTIME=/path/to/speech-port/prepared
export ESP_TTS_PREPARED=/path/to/esp-tts-prepared
export LOCAL_VOICE_SDK=/path/to/local-voice/wake-sdk

python3 tools/build_board109.py \
  --baseline "$P4_BASELINE" \
  --workspace "$HOME/espclaw-work/build-board109" \
  --toolchain "$TOOLCHAIN" \
  --archive "$HOME/espclaw-work/downloads/openh264-2.6.0.tar.gz" \
  --speech-runtime "$SPEECH_RUNTIME" \
  --esp-tts-prepared "$ESP_TTS_PREPARED" \
  --local-voice-sdk "$LOCAL_VOICE_SDK" \
  --source-revision "$(git rev-parse HEAD)" \
  --output "$HOME/espclaw-work/out/board109" \
  --jobs 6
```

[build_board109.py](tools/build_board109.py) 展开完整语音/ASR 加速、PIE、SD SPI DMA、
显示、USB、网络和 BLE 参数，调用 [build_bilibili_v3.py](tools/build_bilibili_v3.py)。
现有 Windows/WSL 准备环境的展开命令见 [BOARD109.md](docs/BOARD109.md)；
SD 模型安装约束见 [LOCAL-VOICE.md](app/espdl-quickapp/LOCAL-VOICE.md)。
`--source-revision` 记录实际源码提交，不会替你切换到历史109源码。

**5. 查收产物与准备 SD 资源**

两种 P4 入口的输出目录均包含 `nuttx.bin`、`nuttx.elf`、`resolved.config` 和
`BUILD-METADATA.json`。例如完整配置构建后：

```bash
ls -lh "$HOME/espclaw-work/out/board109/"{nuttx.bin,nuttx.elf,resolved.config,BUILD-METADATA.json}
sha256sum "$HOME/espclaw-work/out/board109/nuttx.bin"
grep -E '^CONFIG_ESP32P4_PSRAM_XIP=|^CONFIG_ESP32P4_FLASH_AUTO_SUSPEND=' \
  "$HOME/espclaw-work/out/board109/resolved.config"
python3 tools/generate_app_store.py --check
```

构建器保留 BSS、启动复制、分区及链接检查；新构建会生成新的 BLE 测试身份，
镜像摘要不必与历史候选相同。SD 界面资源按 [board109 安装说明](docs/BOARD109.md#sd-resources-and-models)
复制 `materials/board109-ui/desktop-assets/v2/` 的内容到卡上对应目录，保留已有数据。
模型权重单独按说明安装；仅复制 UI 素材不能启用离线推理。
本节不自动烧录，烧录位置与应用/数据边界须按下文核对。

### App Store 云端服务构建

此目标运行在 Linux 服务器，与 P4 固件分别构建。在仓库根目录执行：

```bash
# 在当前终端内生成管理员令牌，不把令牌写入仓库或打印到日志。
export STORE_ADMIN_TOKEN="$(python3 -c 'import secrets; print(secrets.token_urlsafe(48))')"
docker compose -f cloud/app_store/compose.yaml build
docker compose -f cloud/app_store/compose.yaml up -d
curl --fail http://127.0.0.1:8080/healthz
```

Compose 默认仅监听本机 `127.0.0.1:8080`，SQLite 数据卷持久化。管理员页面为
`http://127.0.0.1:8080/admin`；设备联网使用自己的 HTTPS 服务根地址。
令牌需另存于操作者的私有凭据位置，重启服务时复用，不要每次重新生成。
发布参考应用、管理页操作和服务端测试见 [云端商店说明](cloud/app_store/README.md)；
已有反向代理的 systemd 接入脚本在 [deploy/](cloud/app_store/deploy/)，其中
`connect_existing_proxy.py` 绑定本项目已部署服务器的具体路径，不能原样用于任意主机。

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
历史 v1.0 镜像不在当前文件树；不同 revision 的镜像不能混刷。端口号必须替换为实际设备端口。

下面只预览命令，不执行烧录：

```powershell
powershell -File tools\flash_p4_nsh.ps1 -Variant v3.2 -Port COM7 -Transport uart-bridge -DryRun
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
