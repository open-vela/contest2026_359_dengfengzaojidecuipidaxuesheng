# ESP32-P4 v1.0 SC2336 相机 RGB565 实时预览版

本目录是 ESP32-P4 Function EV Board v1.0 的相机实时预览交付物，在 `esp32p4-sc2336-camera-v1.0`（RAW8 取帧）的基础上完成了颜色、显示带宽和缓冲所有权三方面的修复：

- ISP 输出 RGB565，并启用 demosaic、CCM（v1.0 芯片没有白平衡增益模块，用 CCM 对角 `1.70/0.95/1.55` 代替，消除偏绿）和 color 三段；
- CSI DMA 直接写入 V4L2 USERPTR 显示页（零拷贝），删除中间 PSRAM 缓冲和 DW-GDMA 二次拷贝；MIPI-DSI 每帧的 FIFO underrun 从约 33 次/秒降到 0，蓝屏闪烁消失；
- 内核堆从 32 KB 提高到 128 KB，DSI/CSI DMA 描述符与 EMAC 缓冲不再争抢同一个池子，`eth0` 正常注册；
- 三页 framebuffer 下 LVGL 仍保留双缓冲（`lv_nuttx_fbdev.c` 页数判断放宽为 `>=`）；
- DSI 驱动通过 `/dev/dsi-diag0` 暴露 64 位帧数、underrun 和 deferred-worker 失败计数；`dsi_diag` 使用原子 ioctl 快照和复位，不依赖链接地址。

烧录：

```powershell
py -3.13 -m esptool --chip esp32p4 --port COM7 --baud 921600 write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB 0x2000 nuttx.bin
```

启动后从 NSH 控制预览：

```text
desktop camera        # 启动相机预览，串口每 300 帧输出 [qpk] camera health
desktop camera-stop   # 停止预览并回到桌面
dsi_diag --reset      # 原子清零 DSI 诊断计数并立即读取
dsi_diag --json       # 输出带采样 tick 的机器可读统计
```

完整补丁顺序由 `tools/apply_final_overlays.sh` 定义并由 `tools/patches/SHA256SUMS` 校验。NuttX 链为 0001、0003、0004、0006、0007、0008、0011、0013、0014、0015、0016、0018、0020、0022、0024、0026；apps 链为 0002、0005、0009、0010、0012、0017、0019、0021、0023、0025、0027。0006 将 CSI host、sensor 和 board mode 解耦，并收紧 USERPTR、ISR 与 STREAMOFF 的所有权约束；0007 为无 INT 接线的 GT911 增加可取消轮询、启动触摸丢弃窗口，并为具备 RESET/INT 接线的板卡提供标准地址选择回调；0008/0009 将 DSI 统计迁移为正式设备接口和应用命令；0013 固定 HAL 及其补丁，0014 恢复同步换页 API，0015 记录 sensor/panel 的固定上游来源，0016/0017 修复已确认的源代码风格问题，0018 对齐 CMake/Make 的 HAL revision 和 patch 应用行为，0019 对齐 QuickJS 的 CMake/Make 输入，0020/0021 补齐 CSI/HAL、board camera 与 desktop CMake target，0022 补齐 CSI HAL include path。基线、编译器和烧录参数见 `BUILD-METADATA.txt`，新静态构建证据见 `P1_BUILD_VALIDATION.txt`，历史实机证据见 `TEST_REPORT.md` 与两份 acceptance 日志。

注意：本固件针对 revision v1.0/ECO2 样板，启用了低于 rev3 的实验性兼容选项，不应用于量产。CPU 固定 180 MHz；360 MHz 未在本版本中验证。
