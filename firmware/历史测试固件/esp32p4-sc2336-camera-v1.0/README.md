# ESP32-P4 v1.0 SC2336 MIPI-CSI 移植版

本目录是 ESP32-P4 Function EV Board v1.0 的 SC2336 摄像头移植可复现交付物。固件在 OpenVela/NuttX 上提供 `/dev/video0`，图像格式为 1024x600、30 fps、8-bit BGGR Bayer RAW。

移植链路包括 SC2336 SCCB 驱动、ESP32-P4 双通道 MIPI D-PHY/CSI Host、ISP RAW8 bypass、DW-GDMA 到 PSRAM、NuttX `imgsensor/imgdata` 和 V4L2 设备注册。

烧录示例：

```powershell
py -3.13 -m esptool --chip esp32p4 --port COM7 --baud 921600 write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB 0x2000 nuttx.bin
```

代码补丁为 `0003-esp32p4-sc2336-camera.patch`，应在 `0001-esp32p4-nuttx-overlay.patch` 之后应用。完整检查证据见 `TEST_REPORT.md` 和 `camera-final-boot.log`。

注意：本固件针对 revision v1.0/ECO2 样板，已启用低于 rev3 的实验性兼容选项，不应直接用于量产。RGB 彩色转换与 LCD 实时预览属于上层应用/ISP 功能，不在本 RAW8 驱动交付范围内。
