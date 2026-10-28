# P4 rev3.2 构建复现输入（2026-10-27）

`ble-foundations-20261025` 是准备好的构建基线目录名。当前应用仓库是 overlay，构建时会复制基线，再覆盖应用源码。只提供 `.config` 无法重建其中的 NuttX/HAL、头文件、板级适配和 TLS/ESP-DL 库。

这次提供三份去除旧对象缓存的完整构建输入快照，以及逐文件清单、归档 SHA256、原始许可证和获取工具。大文件放在 [构建输入预发布附件](https://github.com/flash555588/contest2026_359_dengfengzaojidecuipidaxuesheng/releases/tag/p4-build-inputs-20261027)，Git 中保存锁定清单和复现入口。其内容不是从任意“最新 SDK”临时拼成的。

| 输入 | 内容 | 获取约束 |
|---|---|---|
| `ble-foundations-20261025` | 匹配的 NuttX、Apps、HAL、板级配置、ESP-DL 源码及构建所需 TLS/ESP-DL 静态库 | 快照内逐文件校验，原有绝对目录链接转为相对链接 |
| `speech-port-20261015-prepared` | `vendor` 中的音频依赖及 `sdcard/speech` 锁定资源、许可证 | 不包含录音、串口日志、候选固件或部署目录 |
| `esp-tts-20261017` | 锁定的官方 `upstream` 输入和 `upstream.lock.json` | 构建前再次执行仓库的官方输入校验 |

各包的精确长度与 SHA256 以 [`inputs.lock.json`](../configs/p4-repro-20261027/inputs.lock.json) 为准。原始 Flash 备份、私有凭据头、Git 元数据、日志及可重建的 `.o`/NuttX `.a` 缓存不在附件里；构建所需的预编译输入库及其源码/许可证保留。

## 运行条件

- Linux / WSL、推荐 Python 3.12+，Python 必须提供 `tarfile.data_filter`。
- GCC 14.2.0 的 `riscv32-esp-elf` 工具链，RV32 ILP32F。工具链的标准库也参与链接。
- `esptool==5.2.0`，其 `esptool.py` 入口用于生成镜像（准备与编译命令不连接设备）。
- `make`、`gcc`、`g++`、`python3`、`kconfig-conf` 等构建工具可从 PATH 调用。若 kconfig-conf 使用独立主机库，还须设置对应的 `LD_LIBRARY_PATH`。
- OpenH264 2.6.0 原始源码归档，SHA256 必须是 `558544ad358283a7ab2930d69a9ceddf913f4a51ee9bf1bfb9e377322af81a69`。
- 保留足够空间存放约 311 MiB 归档、解压输入及另一个独立编译工作区。

## 获取和校验

在本仓库根目录执行，目录可自行选择：

```bash
python3 tools/prepare_p4_repro_inputs.py \
  --cache "$HOME/p4-input-cache" \
  --output "$HOME/p4-inputs-20261027"
```

工具只从锁定的 GitHub Release 附件获取材料；下载完成先核对长度和摘要，再使用安全 tar 过滤解压。解压到临时目录、校验文件清单后才发布；不会覆盖不同版本或已经损坏的准备目录。生成的 `P4-INPUTS.json` 记录三个准备目录。

同事也可先把三份 `.tar.gz` 放到 `--cache` 目录，再完全离线准备：

```bash
python3 tools/prepare_p4_repro_inputs.py \
  --cache /path/to/three-archives \
  --output "$HOME/p4-inputs-20261027" --offline
```

准备过程只重定位生成 Kconfig 中原作者的基线路径；变更摘要记入 READY 文件。源文件、库或链接被意外改动时会拒绝复用。若要换存放位置，应在新目录重新准备，不要手动移动已经准备好的目录。

## 编译同参数版本

```bash
python3 -m venv "$HOME/p4-host-tools"
source "$HOME/p4-host-tools/bin/activate"
python -m pip install esptool==5.2.0

export TOOLCHAIN=/path/to/gcc14/bin
export PATH="$TOOLCHAIN:/path/to/kconfig/bin:$PATH"
# 若主机 kconfig 库需要单独路径，再按实际设置 LD_LIBRARY_PATH。

python3 tools/build_p4_repro.py \
  --inputs "$HOME/p4-inputs-20261027" \
  --workspace "$HOME/p4-build-20261027" \
  --toolchain "$TOOLCHAIN" \
  --archive /path/to/openh264-2.6.0.tar.gz \
  --output out/p4-repro --jobs 6
```

`--workspace` 必须与输入目录分开。`--print-command` 只校验准备输入并输出完整底层命令。源码压缩包没有 `.git`，或 Windows 挂载工作树无法由 WSL Git 查询版本时，可显式传 `--source-revision <40位提交号>`。

该入口明确传入 `--offline-inputs --usb-camera --factory-header --psram-xip --hardware-jpeg --ethernet-share --share-forward-slots 16 --share-net-stats --ble-eight-test`，保持与本轮参考配置一致。它不改变通用构建器的默认行为。特别是 `build_bilibili_v3.py` 默认不开启 PSRAM XIP，单纯拉取相同应用源码不保证得到同配置固件。

2026-10-28 的 LCD 调光更新为此完整入口额外加入 `--lcd-backlight`，将 PWM0 从通用 GPIO2 改为屏幕专用 GPIO26。接线为 LCD 适配板 J6 PWM 到开发板 J1 GPIO26；应用可用 PWM 通道由 capabilities 排除屏幕通道。该改动相对上面的历史参考配置需要单独构建与实板调光验收，不能沿用旧镜像摘要或屏幕观察结果。详见 [P4_BACKLIGHT.md](P4_BACKLIGHT.md)。

输出包括 `nuttx.bin`、`nuttx.elf`、`resolved.config` 和 `BUILD-METADATA.json`。应在配置中确认 `CONFIG_ESP32P4_PSRAM_XIP=y`，并核对元数据的 PSRAM XIP 状态、源码/依赖摘要和分区边界。

每次构建会生成新的合法蓝牙测试身份，因此即使源代码与依赖相同，镜像 SHA256 也会改变；不应要求重编译后等于旧镜像 `32e718bf…`。

## 硬件及验收边界

此入口对应已经核实的 P4 Function EV Board rev3.2、Simple Boot、32 MiB PSRAM、16 MiB Flash，以及配套 Hosted C6 固件；八连接配置仍需与实际 C6 能力匹配。它不是任意芯片或开发板的通用配置。

脚本不访问串口，不烧录、不擦除、不格式化、不迁移任何设备数据。仅应用镜像的写入位置与旧数据区必须在目标板上另行确认。`--psram-xip` 涉及启动复制与两核保护，应继续保留最终 ELF、启动、实际内部 Flash 写擦与实体屏幕验收；不能以本机重新编译成功代替反馈用户的蓝牙/录音蓝屏验收。

本次补充 PR 只交付构建复现材料、入口和验证记录，不包含新的手势或独立浏览器 App 修改。

## 本轮验证

从上述三个导出包解压到独立目录后，离线完成全量构建，镜像 SHA256 为 `e2d2cc28ef213ece1358c507a3e558f603a538f6f9ec22a9b818a86c1579c7be`。基线配置与参考镜像一致；源码摘要、ELF 启动复制前依赖及应用/数据边界审计通过。16 项离线包校验、路径安全、缓存排除及凭据拒绝测试通过。

记录见 [构建验证](evidence/p4-build-repro-20261027/build-validation.json) 与 [宿主用例](evidence/p4-build-repro-20261027/host-tests.json)。本轮没有连接串口或烧录，新生成镜像未作实体蓝屏/闪烁验收。
