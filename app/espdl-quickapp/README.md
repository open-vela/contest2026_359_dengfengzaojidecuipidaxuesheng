# ESP-DL 人脸跟随与物体分类

ESPClaw 开发入口：[agent.md](agent.md) · [Harness 框架](HARNESS.md) · [默认 CSI／USB 多模态摄像头](MULTIMODAL-CAMERA.md)。

图标精修与单次交互动效见 [ICONS.md](ICONS.md)：含设计参考、SVG 导出、光学尺寸、
原生 LVGL 入口和宿主验证方法。此项尚未烧录验收。

## LVGL 首页品质升级（待用户审核）

当前首页使用原生 LVGL 对象复刻审核稿，布局固定为 1024×600；保留上游音乐封面，
将重复音乐卡片替换为 AI 对话入口，并组合相机／识别、天气／环境、影音入口。
实现见 `overlay/apps/system/desktop/glass_home_ui.inc`，天气与家居继续读取现有服务，
没有数据时显示占位符。新线性图标字体在原生应用、快应用图标模式和输入法中共用，
保持已有 `LV_SYMBOL_*` 编码；字形与音乐卡片的 RGB565 渐变填充均可重新生成：

```bash
python3 tools/generate_glass_icons.py   # Pillow；已提交生成后的 C 字体，不需运行时依赖
python3 tools/generate_home_gradient.py
python3 tools/render_refined_home.py --apps /path/to/prepared/apps --output /path/to/render-output
```

上述工具从仓库根目录执行。宿主渲染使用真实 LVGL、生产布局／更新函数和已有 OTF／
音乐封面资源；服务数据与入口回调为模拟环境，所以 PNG 不是实板截图。检查覆盖无数据、
示例数据、深浅色、反复重建、组件切换和入口事件，启用 ASan／UBSan。当前 ESP32-P4 v3
固件已编译；本次没有连接、烧录或修改设备，仍等待用户视觉审核。

现有 Node 前端回归中，桌宠打包字节一致性用例因检出文件的 CRLF／LF 边界不同而失败，
对应 JS 和生成资源本次未改动；相机、录音机、离线识别、家居和桌宠行为回归通过。

### 全界面离线审核

控制面板现采用左侧导航、独立设置卡片和共用线性图标。系统性能页提供 CPU／内存
切换、大折线图和侧栏缩略曲线；每秒读取已有监测服务，在页面存活期间最多保留
60 个有效样本，关闭后释放监测会话。内存占用仅统计系统堆。天气详情采用一屏天空
背景和大温度，湿度、AQI、PM2.5 放在轻透毛玻璃上，去掉竖线与彩色刻度；保留
刷新、返回和更新状态，不添加无来源的预报数据。

天气卡片与详情使用本项目原创生成的天空插画：晴天为无云蓝天与明亮太阳，
多云为蓝天与散云，阴天为连续灰云；晴夜和多云夜景单独切换。原图与完整提示词
保存在 `assets/weather/`，由 `tools/prepare_weather_art.py` 裁切压缩为 JPEG。
复用已有解码器，只在背景资产切换时解码；文字按数据变化更新，关闭页面释放
对应像素缓冲。共 30 套原创素材，覆盖毛毛雨、小/中/大雨、暴雨/特大暴雨、阵雨、
雷雨/雷雨伴雹、冰雹、冻雨、小/中/大雪、暴雪、阵雪、雨夹雪、雾霾、浮尘/扬沙、
沙尘暴、大风、龙卷风与台风；强度与混合降水优先匹配。晴和多云使用专门夜景，
其余类型在绘制时统一夜间调色，不增加解码与模糊次数。
详情复用 588×326 的 RGB565 缓冲（383376 字节）
缩放显示，避免增加全屏解码缓冲；温度复用已有 128px 字体。毛玻璃对对应天空区域
降采样、分离模糊并缓存 160×26 ARGB8888 图像（16640 字节，含圆角透明度），只在天空资产变化时
重算。无天气数据时显示中性背景和占位符。宿主验证不代替
实板帧率与功耗测量。

天气性能路径直接从只读 JPEG 数组解码，去掉整张压缩图片的临时副本；同尺寸天空与
毛玻璃缓冲在资产切换时复用。毛玻璃采用滑动窗口原地模糊，仅使用 480 字节行缓冲，
不再分配 12480 字节临时堆缓冲；圆角透明度仅在边角计算。只更新内容有变化的标签，
相同读数的新版本号不触发重绘。254 张原生帧缓冲与优化前逐像素一致；主机基准
记录不能代替实板计时。共享 MJPEG 接口仍保留摄像头缺失 DHT 表的补全路径。

天气详情完全遮挡首页时，暂停首页标签、设备绘制及天气背景解码，保留上次图像供返回
手势预览；网络、天气服务和状态栏继续运行。返回桌面时在本次关闭回调内同步最新快照，
不等待一秒定时器。页面被手势移开或变成半透明时恢复首页更新；替换页面也不保留冻结
状态。该路径不增加图像缓冲。

音乐页底部增加已播放时间、播放进度条和总时长，与现有播放器快照同步。
总时长读取 MP3 首帧中的 Xing／Info／VBRI 帧数；无法读取时显示 `--:--`，
正常播放结束后使用实际播放时长。现有流式播放器没有 seek 接口，进度条仅展示进度。

`tools/render_all_ui.py` 复用现有原生二级页面、媒体页构造函数、快应用资源和生产
QuickJS／LVGL 控件绑定，生成 1024×600 RGB565 帧缓冲截图。输入法也使用生产 C
布局；词库、语音、网络和设备服务保持离线。审核样本覆盖页面入口、详情、输入、
确认、草稿预览和服务数据状态，深浅色各一套。独立应用保留原有配色，媒体页不含
实际相机／视频解码画面。宿主运行启用 ASan／UBSan，不连接或烧录设备。

```bash
# Linux / WSL；先用 render_refined_home.py 准备同配置的宿主 LVGL 缓存
python3 tools/render_all_ui.py --apps /path/to/prepared/apps --cache /path/to/home-render-cache --output /path/to/all-ui-review
# 已复制到本地的截图目录；Python 需有 Pillow，Windows 默认使用微软雅黑
python tools/package_ui_review.py /path/to/all-ui-review
```

打包目录中的 `index.html` 支持功能筛选、深浅色并排对比和原图放大；
`LVGL-全界面审核.zip` 包含全部原始 PNG、分类拼图、清单和离线图册。

同一固件中的 ESPClaw 触屏聊天、服务配置及共用 HTTPS 说明见 [CHAT.md](CHAT.md)。
双网络使用有效有线网关优先、Wi-Fi 备用：网线只有链路但没有 DHCP 地址时，
不抢占无线外网出口。桌面统一管理 `eth0` / `wlan0` 的 DHCP 和 DNS，
构建必须关闭 `CONFIG_NSH_NETINIT`，避免 UDP 68 端口和 DNS 被两个初始化任务争用。
插线后会申请有线租约，断线后 DNS 回到可用的 Wi-Fi；网口启停不直接操作 LVGL。
两种接口均在 DHCP 租期一半时重新申请，失败后每 10 秒重试；原租约到期或链路变化时
清理旧地址、网关和对应 DNS。Wi-Fi 后台任务持续运行，热点恢复后重新获取 IP，
未关联时每 30 秒尝试使用 C6 已保存的配置重连，用户手动断开后停止自动重连。
所有租约期限使用单调时钟，NTP 校时不改变期限。设置页只提交 DHCP 请求，不阻塞 UI。
2026-10-15 的宿主回归可运行 `python3 tools/tests/test_network_lifetime.py`
（在仓库根目录、Linux/WSL、需要 C 编译器），覆盖提前续租、失败/到期、重关联、
过期连接的 ACK、NAK、长时间计时、无限租约及双网 DNS 切换；尚需实板长期运行验证。
内置录音机、GPIO／I²C／SPI／UART／PWM／舵机／音频／BLE 快应用接口与 AI 参考见 [HARDWARE.md](HARDWARE.md)。
Home Assistant 原生米家页的实体范围、分页、状态色、控制确认和布局依据见
[HA-TILE-DESIGN.md](HA-TILE-DESIGN.md)。

桌面“智能家居”显示两个设备入口，手动固定优先，其余位置按设备使用次数排序。
在设备“更多功能”右上角选择“固定到桌面”或“取消桌面固定”，最多固定两台。
固定设置按 HA 家庭地址保存；确认设备已删除后释放固定位置。点击入口只打开对应
设备详情，不直接发送控制命令。桌面可见时每 30 秒异步读取这两个设备的状态，
离开桌面即取消请求；排序变化的持久化合并为每 10 秒一次，状态更新只驻留内存。


大肥鱼桌宠 1.0.1 的原生浮层、`system.pet` API 和快应用源码已接入当前 overlay，
说明与许可证见 [dafeiyu/README.md](overlay/apps/system/desktop/dafeiyu/README.md)。
桌宠已通过宿主 C/JS 测试、固件构建和实板启动测试；`desktop ui pet` 可启动内置
QuickJS 应用。固件优先加载 `/sdcard/dafeiyu/dafeiyu.lvbin` 中预转换的上游原版精灵，
资源缺失或校验失败时回退到内置几何形象。单击、双击、拖拽及原版精灵显示仍须以
当前固件对应的实板记录为准。

面向 ESP32-P4 Function-EV-Board 的内置快应用，使用 USB MJPEG 相机和本地 ESP-DL 模型。仅提供两个模式：

- **人脸跟随**：连续采集、MSR/MNP 两阶段人脸检测、跨帧几何关联、目标框和中心偏移提示。短暂丢失时不切到远处另一张脸，连续三次丢失后重新选取目标。当前是画面内跟踪；没有人脸身份注册/比对，也没有连接云台或舵机。
- **物体分类**：拍摄一帧，使用 ImageNet MobileNetV2 显示前三个类别及分数。类别为模型自带英文名称。

P4 神经网络加速使用 ESP-DL 官方 `xespv` 向量指令、`xesploop` 硬件循环及标量 FPU。NuttX 扩展上下文保存 Q0–Q7、QACC、UA、XACC、SAR、SAR_BYTES、FFT、CFG 和两个硬件循环的状态。异常帧强制 16 字节对齐并记录原始 SP，兼容算子内部暂时不对齐的栈；P4 非懒加载 FPU 路径在异常入口完整保存浮点寄存器与 FCSR。任务可被抢占；中断处理器不运行向量算子。尚未接入 PPA、硬件 JPEG 解码或双核并行算子。

图像链路为 MJPEG 软件解码 → 标准 RGB565 → C 图像缩放/归一化 → P4 加速神经网络。修正了解码器红蓝通道颠倒，以及启用缩放时只缩小输出矩形、未缩小像素块的问题。`640×480 → 320×240` 现在对 MCU 像素块真正取平均；软件解码四种比例均与独立 libjpeg/Pillow 结果对比。

帧率优化版对 JPEG 热点使用 `-O2`，缩放后尺寸匹配时直接解码到工作线程的目标帧，复用每个模型会话的归一化表与检测后处理对象。每帧完整执行 MSR/MNP；检测结果列表在重用前清空。移除慢帧结束后固定的 20 毫秒等待，快帧按 100 毫秒周期调度；界面每 50 毫秒检查新帧，只渲染匹配的图像与检测框。100 毫秒是目标周期，实际帧率以 `perf frames=30 wall=...` 计数为准。

当前工具链是 Espressif GCC 14.2.0_20251107。SDK 根据 GCC 主版本判断广播加载指令步长不可靠，移植改用零步长加载加显式地址累加；`MALLOC_CAP_SIMD` 分配保证 16 字节对齐。实板逐像素对比仍发现 SDK 图像 SIMD helper 存在剩余差异（`evidence/face-simd-9.json`），因此生产预处理明确选用 C 实现；卷积、深度卷积、矩阵运算继续使用原始 P4 SIMD 权重及算子。

源码固定到 ESP-DL v3.2.0、提交 `dc380d450835d42f92777121a0cd4fc67d7c3a8c`。模型包使用该提交原始 P4 权重布局，实际运行前检查每个文件的长度及 SHA-256。没有配置网络识别服务。

## Flash 布局

下述布局记录历史开发过程中对 `/data` 的迁移，不构成对当前设备再次擦除数据的授权。16 MiB flash 的布局如下，地址区间右端不包含在内：

| 用途 | 起始地址 | 结束地址 |
| --- | --- | --- |
| Simple Boot 固件 | `0x2000` | 小于 `0x800000` |
| 模型包 | `0x800000` | 小于 `0xc00000` |
| SmartFS `/data` | `0xc00000` | `0x1000000` |

程序通过 `CONFIG_ESPRESSIF_STORAGE_MTD_OFFSET=0xc00000` 访问新数据区，大小仍为 4 MiB。模型优先从 `/sdcard/espdl/` 按 manifest 文件名读取，否则访问 flash 模型区。请只安装 `models/manifest.json` 列出的三个模型；开发目录里可能保留早期实验产物。

`diagnostics/flash_espdl.py` 默认只检查文件和显示写入范围，`--execute` 才烧录到已识别的 COM23 开发板。已有模型和 SmartFS 的开发板更新请使用 `--execute --firmware-only`，保留模型与 `/data`。首次完整烧录会清空新数据区，需要启动后运行 `mksmartfs /dev/smart0` 和 `mount -t smartfs /dev/smart0 /data`。回退旧固件需要同时重新处理旧 `/data` 所在区域。

## 构建和验证

当前脚本仍依赖 WSL 中已有相机基线 `/tmp/v3-desktop-camera-usb-20260914` 和已安装 Espressif 工具链。按顺序运行：

1. `diagnostics/prepare_espdl_work.py`（WSL）
2. `diagnostics/prepare_espdl_context.py`（WSL）
3. `diagnostics/package_espdl_models.py`（Windows 或 WSL）
4. `diagnostics/build_espdl_library.py`（WSL）
5. `diagnostics/build_espdl.py`（WSL）

隔离树为 `/tmp/v3-desktop-espdl-20260915`。完整固件使用 ILP32F ABI，C++、TLS 和全部系统对象必须匹配；不能把旧软浮点对象混入。

主机测试：`diagnostics/test_espdl.py` 检查真实 C 跟踪/服务代码的内存与未定义行为、取消、断开、重复开始、帧一致性及 SHA-256，并检查实际 MJPEG 解码器四种缩放比例的越界、重复解码与截断输入。`diagnostics/inspect_espdl_pixels.py` 独立对比颜色及缩放像素；`node tests/test_app.js overlay/apps/system/desktop/espdl/app.js` 验证快应用切换和渲染时序。

实板诊断：`desktop espdl test 0` 依次运行同核 FPU/向量抢占与重复 Gemm 自检、图像预处理对照、官方 320×240 人脸 JPEG 的两次完整推理、分类重复推理。终端等待工作线程结束再返回；`desktop espdl` 查询完成状态。自检中的图像 SIMD 差异是诊断记录，所选 C 预处理必须通过独立 RGB565 数值参考检查。`desktop ui espdl` 打开应用，`desktop ui espdl-start` 开始跟随，`desktop ui espdl-stop` 停止。UI 只从完整发布的同一帧读取图像和检测结果，工作线程不访问 LVGL。

证据分别保存在 `evidence/firmware-validation.json`、`evidence/host-validation.json`、`evidence/js-validation.json` 和实板日志。构建或主机测试通过不能代替实板识别准确性、帧率和长期稳定性验证。
## Local verified source release: 2026-09-19

The application source in this directory is synchronized from the locally verified 04-v3-20260913/espdl-quickapp tree. It includes the current Home Assistant card UI, generated resource and authorization digest, BLE HID/touchpad implementation, and frontend regression test. Generated firmware images are documented as evidence and are not treated as source.

The music credential header was intentionally excluded because it contains a hard-coded API key. The runtime keeps its credential fallback behavior and accepts user configuration at runtime.

See the source and hardware work log at ../../docs/WORKLOG_2026-09-19.md and the 2026-09-19 hardware handoff logs at ../../logs/2026-09-19/README.md.

## Grok Ball LVGL port

OuO now launches Grok Ball 2.0.0 using the pinned MIT upstream engine and native SVG contour adaptation. See ../../ouo/README.md for source/license, state migration and validation limits. The CLI/package identifiers remain compatible. Native host render and ESP32-P4 build are review evidence only; no flashing or hardware performance validation was performed.

接口与引脚依据官方开发板手册，见 [BOARD-INTERFACES.md](BOARD-INTERFACES.md)。
