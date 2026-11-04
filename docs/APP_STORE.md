# 应用商店（首版）

入口：桌面「全部应用 → 应用商店」。原生 LVGL 页面复用 `glass_app_frame.h` 的 164 px 侧栏、主题、安全区、左下返回及拖动防误触；已安装快应用使用同一 QuickJS/LVGL 框架。

精选首页采用不同尺度的两张应用海报和精简工具列表；目录页面保留紧凑应用卡片、彩色几何图标与获取/管理胶囊按钮。保持统一侧栏、主题与间距，浅色/深色均有对应配色；海报、图标与预览直接用 LVGL 控件绘制，不引入整屏图片或连续动画。启动字库补充商店和精选应用文案，保留旧字库全部字符，无 SD 时也可阅读。

## 当前能力

- 精选、分类、搜索、详情、SD 仓库、已安装列表与分页。
- 内置精选：轻量计算器、专注计时、随手计数，均有可运行源码。
- 明确选择内置存储或 SD 安装；不自动更换介质。首次使用的新 SD 卡无需预先创建 `qpk` 目录。
- 后台读取、SHA-256 校验、真实 QuickJS **只编译不执行**的语法检查、暂存写入、发布及卸载。页面关闭取消尚未提交的工作，收尾后释放目录快照与线程；界面不等待 worker join。
- 一次一个操作，关闭/取消不自动重试；被不透明全屏页遮挡时继续接收完成结果，延后绘制。稳定状态的轮询不重建页面。
- 已安装应用可以打开、明确确认后卸载。卸载仅移除由此商店管理且未被修改的两个应用文件，**保留 `.data/<package>`**；可在「全部应用管理」使用原有完整管理功能。
- 首版不覆盖、不升级已有同包名应用；其他来源的应用、创作器草稿、`.examples`、Portal 上传暂存区独立保留。安装中断遗留的 `.store-*` 目录保留待检查，不递归清理未知文件。

本次增加商店入口，不修改云端 AI、联网服务、本地双 AI、ASR/TTS、音乐、锁屏、桌宠及 JBOX 通信配置。云端商城接口已接入；服务端、管理员页面及启动材料见 [cloud/app_store](../cloud/app_store/README.md)。原有云端 AI 功能继续保留。

## SD 本地仓库

默认只读目录 `/sdcard/app-store`：

```text
app-store/
  index.json
  store.my_tool/
    app.js
```

`index.json` 格式：

```json
{
  "schema": 1,
  "apps": [{
    "package": "store.my_tool",
    "name": "我的工具",
    "version": "1.0.0",
    "category": 0,
    "summary": "应用的实际用途说明",
    "bytes": 1234,
    "sha256": "替换为 app.js 原始 UTF-8 文件的 64 位小写 SHA-256"
  }]
}
```

`bytes`、`sha256` 必须对应文件原始字节，不能直接使用示例占位值。包名为 `store.` 加小写 ASCII 字母、数字或下划线，最长 47 字节；不能与精选或其他仓库条目重复。分类 `0` 实用工具、`1` 专注效率、`2` 轻松趣味。名称、版本、简介分别最多 47、23、191 个 UTF-8 字节。单脚本最多 64 KiB、索引最多 24 KiB、总目录最多 24 条（含三个精选）。不接受任意文件路径、模块依赖、符号链接、空源码或含 NUL 的文本。

## 数据驱动的首页与海报

推荐应用、海报文案、首页标题、应用图标和展示模板来自目录数据，UI 不绑定某个包名或列表下标。内置默认内容位于 `quickapp/store/catalog.json`；SD `index.json` 可以添加以下字段，点击「刷新」或重新打开商店后生效，无需改 UI 或重新烧录：

```json
{
  "schema": 1,
  "apps": [],
  "headline": "让日常，轻松一点。",
  "tagline": "小而精的应用，恰好满足你的需要。",
  "featured": [{
    "package": "store.focus",
    "title": "让专注，\n自然发生。",
    "subtitle": "25 分钟，只做好一件事。",
    "art": "timer",
    "tone": "forest",
    "value": "25",
    "unit": "MINUTES"
  }]
}
```

可直接参考 `quickapp/store/home.example.json`。已有本地仓库时合并首页字段并保留原来的 `apps`，不要用空数组覆盖已有应用清单。推荐可以引用内置或同一索引中的 SD 应用，不需要重复声明内置包。点击海报按包名解析当前条目，换顺序也不会点开错误应用。

- `featured` 最多 2 项；1 项自动使用宽海报，空数组明确关闭海报并显示目录。不提供该字段时保留内置推荐。
- `title` 必填，最多 95 UTF-8 字节，可含一个换行；`subtitle` 最多 127 字节；`value` / `unit` 最多 23 字节。长文案按模板省略，字符覆盖沿用系统字体。
- `art` 支持 `timer`、`bars`、`none`；`tone` 支持 `forest`、`sand`、`lavender`。这些是有界原生样式模板，目录不能注入脚本、HTML 或任意路径。
- `headline` 最多 95 字节、`tagline` 最多 127 字节。应用条目的 `icon` 可选 `calculator`、`timer`、`counter`、`grid`，缺省按分类选择通用图标。
- 未知包名、重复推荐、超限或非法样式拒绝整份 SD 索引，并保留内置回退；不会展示无法解析目标的推荐。首版不轮播、不联网下载海报图片。

这是可由用户更换的本地脚本仓库；SHA-256 用于完整性检查，**不是发布者签名或安全认证**。浏览不执行脚本；安装通过检查后，用户点「打开」才在既有 QuickJS 运行时执行，仍受原有 C 层 API 权限约束。

从维护者电脑生成自有应用的校验字段：

```python
from pathlib import Path
import hashlib
data = Path('app.js').read_bytes()
print('bytes:', len(data))
print('sha256:', hashlib.sha256(data).hexdigest())
```

内置资源源文件在 `quickapp/store/`，修改后运行：

```sh
python3 tools/generate_app_store.py
python3 tools/generate_app_store.py --check
```

`--export <目录>` 可导出这三个精选的标准仓库格式供开发参考。它们与固件内精选同名，不应同时放入活动 SD 仓库；改为独立包名并重算元数据后才作为新的 SD 应用使用。

## 验证与构建

```sh
python3 tools/test_app_store.py --apps /path/to/prepared/apps --quickjs-cache /path/to/quickjs-asan-objects
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 PAGE_AUDIT_MOTION=1 \
 python3 tools/render_all_ui.py --apps /path/to/prepared/apps \
 --cache /path/to/lvgl-host-cache --output /path/to/native-store \
 --harness-source app/espdl-quickapp/tests/render_app_store.c
```

服务测试使用真实文件系统、SHA、QuickJS，注入满盘、写失败和发布失败，检查损坏/重复索引、语法错误、拔卡不回退、已修改应用不删除、应用数据保留、取消与关闭。UI 测试使用真实 LVGL/QuickJS 和离线设备替身，覆盖两种主题、动效模式、触摸防误触、键盘、遮挡恢复和退出计数；不代表实板屏幕与 Flash 并发已验收。

完整 P4 构建使用 `tools/build_bilibili_v3.py`，继承当前兼容基线和最新功能选项，保持 12 MiB 程序 / 4 MiB 数据分区与 BSS 检查。实际模型、Wi-Fi、SD 热插拔、实体触摸和显示效果需要后续上板验收。此版本不自动烧录。

## 2026-11-04 验证记录

板端商店已完成真实文件系统、SHA 与 QuickJS 的 ASan／UBSan 服务测试（含泄漏检测），覆盖故障、取消、介质选择与 30 次重开。原生 LVGL 页面完成两种主题 × 两种动效模式共 72 张截图与 48 次页面／键盘退出计数检查；UI 测试未开启 ASan 泄漏检测，退出计数不能替代完整堆分析。

包含商店的 P4 候选完整编译、链接与镜像审计通过：镜像 8,287,744 字节，SHA-256 `895ab3c0be9da45caf54683d974179d24218bd75f826af5f4d4a046152464791`，`_ebss=0x4ffa3ebc`，保留 12 MiB 程序／4 MiB 数据布局。双 AI、109 版 ASR／TTS、音乐、锁屏、小鸟和 JBOX 功能输入保留。本次没有烧录，不能据此声称实板商店或 JBOX 已验收。

云端服务代码及启动步骤见 [云端商店说明](../cloud/app_store/README.md)。24 项 API 测试通过；实际 Uvicorn 回环 HTTP 的发布、目录与下载 SHA 校验通过；生产 C HTTP 策略和实际 API 响应驱动的原生安装器测试通过。管理员页面还在隔离数据库中实际完成登录、应用发布、编辑新版本并保留旧版、首页保存，以及无效 JSON 拒绝与刷新时草稿保留；实时预览、海报配色与排序、卡片详情、明暗主题、放大／Esc 关闭和 1440／390 像素响应式布局检查通过。

最新管理页仅调整服务端静态页面，不改变上述固件镜像。预览展示浏览器模拟的商店目录和海报外观，不执行上传脚本。正式本机目录保留三款云端示例应用、四个版本，测试发布在隔离数据库中完成；令牌、数据库与临时运行状态不入仓。公网部署、Docker 构建与实板验收尚未进行。
