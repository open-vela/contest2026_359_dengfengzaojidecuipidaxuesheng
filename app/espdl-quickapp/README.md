# ESP-DL 人脸跟随与物体分类

同一固件中的 ESPClaw 触屏聊天、服务配置及共用 HTTPS 说明见 [CHAT.md](CHAT.md)。
内置录音机、GPIO／I²C／SPI／UART／PWM／舵机／音频／BLE 快应用接口与 AI 参考见 [HARDWARE.md](HARDWARE.md)。
Home Assistant 原生米家页的实体范围、分页、状态色、控制确认和布局依据见
[HA-TILE-DESIGN.md](HA-TILE-DESIGN.md)。

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
