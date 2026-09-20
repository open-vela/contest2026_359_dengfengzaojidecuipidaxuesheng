# ESP32-P4 v1.0 实机验收报告

测试时间：2026-08-27（Asia/Shanghai）；目标板：ESP32-P4 Function EV Board；芯片：ESP32-P4 revision v1.0 / ECO2；MAC：`60:55:f9:fa:f4:8b`；串口：Silicon Labs CP210x，COM7，UART0 115200 8N1。

固定源码从干净工作区生成 `nuttx.bin`，大小 2908784 bytes，SHA-256 为 `74420c66be6a0298dbbeb21326600d47010612c27a097030df4e47cc90e2c058`。镜像写入偏移 `0x2000`，esptool 5.3.1 完成压缩写入、写后校验和硬复位。

连续两次硬复位均进入 NuttShell，未出现 `CHIP_LP_WDT_RESET`、panic 或异常。两次均得到同一干净版本标识：

```text
NuttX 0.0.0 cd61ccdd Aug 27 2026 10:45:27 risc-v esp32p4-function-ev-board
```

运行态检查结果：

```text
desktop_main  Waiting/Signal  stack 32704
/dev/fb0
/dev/input0
Kmem total 489332, used 3724, free 485608
Umem total 33554428, used 2540036, free 31014392
```

结论：v1.0 专用启动兼容路径、桌面任务、LCD framebuffer、GT911 输入节点、PSRAM 和后台 NSH 已通过自动化实机启动验收。实体 LCD 的实际画面、颜色/方向和触摸坐标仍属于人工目视验收项，不能由串口节点存在替代。

完整 UART 证据位于 `logs/v1-repro2-smoke-20260827.log` 和 `logs/v1-repro2-reset-cycle2-20260827.log`；对应构建日志为 `logs/build-repro-v1.0-20260827.log`。
