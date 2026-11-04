# app：原生应用与应用开发快照

本目录保存原生应用、QuickJS 内置资源和应用级集成材料，不是完整的 NuttX apps 仓库。
返回[仓库首页](../README.md)。

## 入口

[ProgrammableMatrix / JBOX-S3](ProgrammableMatrix/README.md) 保存 ESP32-S3 + 4×CH446Q
无线 IO 与矩阵连线板的完整独立工程，包括 ESP-IDF 固件、Python SDK、Tkinter 连线工具、
JBoxScript 解释器与宿主测试。使用 ESP-IDF 5.5.x，构建命令见
[仓库首页](../README.md#programmablematrix--jbox-s3-构建)，上游版本及文件摘要见
[UPSTREAM.json](ProgrammableMatrix/UPSTREAM.json)。

[jizura](jizura/README.md) 保存 JIZURA 原版完整功能基线，先在浏览器验证全部效果、编辑器和导出，再进行 ESP 移植。

[espdl-quickapp](espdl-quickapp/README.md)是 v3 应用开发快照，含 overlay、模型、测试和
实板证据；聊天、音乐、硬件 API、Home Assistant 等功能以该目录的专题文档为准。

[homeassistant](homeassistant/README.md)保存根级家居桥、内置 JS 资源、生成脚本与宿主测试；
[pomodoro](pomodoro/README.md)保存番茄钟 C/LVGL/QPK 适配和资源生成脚本。
[hello_app](hello_app/README.md)保留参赛应用示例，与完整桌面固件不是同一个交付对象。

## 修改与集成

快应用可编辑源码位于 [quickapp](../quickapp/README.md)。改动 JS 后必须检查生成资源及
使用它的 overlay 副本，不能只修改最终 C 字符串。应用快照需要按对应基线集成到外部源码树，
不能把目录存在或宿主测试通过当作已经完成固件编译、烧录和外部服务联调。

固件选择见 [firmware](../firmware/README.md)，第三方来源见
[THIRD_PARTY.md](../THIRD_PARTY.md)。不要提交服务令牌、私有配置和临时构建产物。

[网络直播与本地视频](bilibili_live/README.md)：B站、虎牙、网易CC直播，单线程 H.264＋AAC；SD 卡 MP4 H.264/MJPEG＋AAC、全屏显示。
