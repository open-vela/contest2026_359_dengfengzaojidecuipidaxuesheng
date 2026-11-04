# App SD 素材包

这里提供可直接解压的成品资源，收录识别模型、全局及歌词字体、JIZURA 离线素材、Grok 动画、大肥鱼精灵、语音运行资源、浏览器公共 CA 和桌面图标。2026-10-27 从当前连接的 ESP 设备补齐语音和歌词字体，并核对仓库锁定来源；浏览器 CA 单独取自固定版本的公共上游。2026-11-04 补入已发布的 SD 图标资源，组装时自动安装到对应目录。照片、视频、聊天记录、采集录音、用户配置、测试临时文件和固件不在包内。

当前共 9 个 ZIP、85 个安装文件，压缩后约 106.7 MiB，展开约 740.3 MiB。本次新增 7 个图标运行文件、来源清单和许可证；原有八个 ZIP 保持原摘要。

| 资源包 | 解压到 SD 根目录后的路径 | 内容 |
| --- | --- | --- |
| [espdl.zip](espdl.zip) | `espdl/` | 清单指定的 3 个识别模型、模型清单和许可证 |
| [fonts.zip](fonts.zip) | `fonts/` | 成品 `VelaGlobal-Regular.ttf`、字体清单和 4 份 OFL 许可证 |
| [grok.zip](grok.zip) | `grok/` | 首页深浅主题、聊天头像、独立应用深浅主题，共 5 个 `.gbs` 文件及清单、许可证 |
| [jizura.zip](jizura.zip) | `jizura/` | 完整离线引擎、27 套风格、10 个字体文件、预览入口、原创演示项目和许可证 |
| [dafeiyu.zip](dafeiyu.zip) | `dafeiyu/` | 已转换的 `dafeiyu.lvbin` 和上游许可证；含 4 个方向、3 种尺寸，共 12 张 RGB565A8 图像 |
| [speech.zip](speech.zip) | `speech/` | ESP-TTS 小新完整、小乐、小新精简三套声库，MultiNet7 中文模型、命令词表、自定义唤醒模型和 ESP-SR 许可证 |
| [lyric-fonts.zip](lyric-fonts.zip) | `fonts/lyrics/` | 思源黑体、思源宋体、马善政楷书、站酷快乐体、站酷黄油体、站酷小薇体六套成品字体、字体清单及各自 OFL 许可证 |
| [browser-ca.zip](browser-ca.zip) | `netsurf/` | certifi 2026.07.22 的 Mozilla 公共 CA 根证书和原始许可证；用于新卡上的浏览器 HTTPS，不含本机额外信任项 |
| [desktop-icons.zip](desktop-icons.zip) | `desktop-assets/v2/icons/` | 14、16、20、24、32、64 六档位图和动效遮罩，共 7 个 `.gac` 运行文件，附来源清单及 Material Symbols 的 Apache-2.0 许可证 |

## 组装与安装

在本目录运行，仅需 Python 标准库：

```sh
python assemble.py --verify
python assemble.py --output /path/to/sdcard-ready
```

`--output` 必须是新目录或空目录。工具先核对各 ZIP 的 SHA-256，再逐文件核对长度和摘要；实际组装会在写出时再次校验。将 `sdcard-ready` 里的 `espdl`、`fonts`、`grok`、`jizura`、`dafeiyu`、`speech`、`netsurf`、`desktop-assets` 目录复制到 SD 根目录。也可直接把各 ZIP 解压到同一个空目录。

已安装旧版资源包的 SD 卡，可单独解压 `desktop-icons.zip` 到 SD 根目录补齐图标；保留 ZIP 内的 `desktop-assets/v2/icons/` 目录层级和 `.gac` 文件，不需再次解压这些运行文件。

设备挂载后，路径应为 `/sdcard/espdl/`、`/sdcard/fonts/VelaGlobal-Regular.ttf`、`/sdcard/grok/`、`/sdcard/jizura/` 和 `/sdcard/dafeiyu/dafeiyu.lvbin`。复制到已有 SD 卡时请自行保留原有同名资源和个人文件。

语音文件位于 `/sdcard/speech/esp-tts/`、`esp-sr/` 和 `custom/`；歌词字体位于 `/sdcard/fonts/lyrics/`。目标 SD 使用设备原有 VFAT 文件系统；包内统一使用固件约定的路径大小写，不在已有卡上新建仅大小写不同的目录。

## 版本与兼容范围

- 模型采用仓库 `models/manifest.json` 中的原始布局和摘要。模型清单的 Flash 偏移是历史信息；本包只提供 SD 文件，不执行烧录。
- 全局字体与 `glass_sd_font_asset.h` 的长度和 SHA-256 一致，覆盖 45,695 个 Unicode 映射。字体覆盖不等于复杂文字排版已实现。
- Grok 文件按清单逐一核对 SHA-256，并检查 `GROKSD1` 文件头、状态表和全部帧区间。播放器需要这些未经压缩的 `.gbs`，不能把 ZIP 直接放到设备上使用。
- JIZURA 包是完整离线预览资源，可在电脑上通过本地 HTTP 服务器打开 `jizura/index.html`。当前工作区原生音乐 UI 已移除旧 SD PV 帧库入口，使用聚焦、强调、逐字三套实时 LRC 效果；此包不会启用已移除的入口，也不包含历史渲染帧库。固件是否读取离线目录取决于所用版本。
- 大肥鱼由上游固定提交 `5b0e01856116bd2bae82df1f43c32faa5f056196` 的正面、背面、侧面 PNG 转换，转换程序检查整包与各图片 CRC32。
- 语音声库和 MultiNet 模型逐文件匹配仓内 ESP-SR v2.4.6 的锁定摘要。自定义唤醒模型与仓内已有 `models/nihao_openvela.gkw` 完全相同，不训练新模型、不修改固件认证头；命令词表与仓内已有版本逐字节一致。包内带原始 ESP-SR 许可证。网络转写及 AI 仍需用户自行配置。
- 六套歌词字体逐文件校验 SHA-256，并核对 `glass_lyric_font_assets.h` 的长度和 FNV 校验值。四套从设备完整读回；思源黑体和宋体使用本地完整生成成品，其摘要与板上字体清单一致，避开门户 120 秒请求截止造成的大文件截断。各 OFL 许可证匹配 `lyric-fonts.lock.json` 锁定的 Google Fonts 输入；字体生成器为 `tools/prepare_lyric_fonts.py`。
- 浏览器公共 CA 来自 certifi `2026.07.22` 的固定提交 `f4bc676bc101fe2235846e37044e8c693d6cbaf4`，与上游原文件逐字节一致，包含 121 个证书，240,216 字节，低于当前装载器的 256 KiB 上限。保留 MPL-2.0 原始声明。本包没有复制设备上从 Windows 导出的信任库，不包含本机额外证书；如现有卡有自定义信任设置，请保留原文件。此次仅核验主机解析和完整性，未将此公共 CA 包部署到板上或验收真实站点 HTTPS。
- 桌面图标逐字节取自 `materials/board109-ui/desktop-assets/v2/icons/`，核对该目录上级 `MANIFEST.json` 的长度和 SHA-256；对应固件读取路径为 `/sdcard/desktop-assets/v2/icons/`。图形源为锁定提交 `10ecc14790612ca8fd164e3f4ffe9c16fc50d5b7` 的 Google Material Symbols Rounded，经 ESPClaw 适配为 LVGL 位图和动效遮罩，包内保留来源、改动说明和原始许可证。本次只补图标；该版本桌面使用的字体、壁纸、天气和拼音资源仍按 [BOARD109.md](../../../../docs/BOARD109.md) 安装完整 `desktop-assets/v2/` 目录。

## 本次补齐范围

新增包只从明确选定的运行资源生成。旧 SanoTTS 后端、未使用的 WakeNet 模型、已被全局字体替代的 `VelaCJK-Regular.ttf`、JIZURA 的各类历史 PV 帧库、浏览器程序及其候选备份、音乐缓存、草稿、历史记录、照片、视频、采集文件、隐藏备份和临时文件不进入新增包。浏览器应用由其配套安装包及装载器管理，不把板上开发候选混入通用资源安装包。

[manifest.json](manifest.json) 包含全部安装路径、字节数、SHA-256 和上游来源。2026-10-27 补齐时仅通过设备文件门户读取选定资源，未替换板上文件、修改配置或烧录固件。校验与解压结果见下方验证记录。

## 验证记录（2026-11-04）

- `python assemble.py --verify --output <新的空目录>`：9 个 ZIP、85 个文件全部通过 SHA-256 校验并成功解压，展开总计 776,227,888 字节。
- 7 个图标运行文件全部安装到 `desktop-assets/v2/icons/`，与已发布的 `materials/board109-ui/` 资源逐字节相同；单独解压 `desktop-icons.zip` 的目录结构也已核验。
- 原有八个 ZIP 及其清单条目保持不变，全部 SHA-256 复核通过。本次验证为宿主组装与解压，未写入实体 SD 卡或重新烧录固件。

## 历史验证记录（2026-10-27）

- `python assemble.py --verify --output <新的空目录>`：8 个 ZIP、76 个文件全部通过 SHA-256 校验并成功解压，展开总计 775,844,194 字节。
- 新增 26 个安装路径通过明确的资源白名单检查，不含照片、视频、聊天、音频采样、测试中间结果、临时文件、历史候选或私钥；语音与字体的来源校验见上节。
- [SHA256SUMS](SHA256SUMS) 提供全部 ZIP 的 SHA-256，供独立核对。此记录确认资源完整性和宿主解压，不代表新固件构建或实板功能重新验收。
