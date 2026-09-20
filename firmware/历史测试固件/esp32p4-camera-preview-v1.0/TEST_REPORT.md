# SC2336 相机 RGB565 实时预览实机验收报告

测试时间：2026-09-03（Asia/Shanghai）。目标板为 ESP32-P4 Function EV Board，芯片 revision v1.0/ECO2，摄像头 SC2336，屏幕 1024x600 MIPI-DSI，串口 COM7/115200，CPU 180 MHz。

最终固件 `nuttx.bin` 大小 3,191,440 bytes，SHA-256 为 `0122d65d194367e5a7c7cb28fea927fd4924ce2918f8f8e8b4861eae6e7de672`。esptool 5.3.1 写入 `0x2000` 并通过写后 Hash 校验。以下全部数据来自烧录这个二进制之后的实测，原始串口记录见同目录 `acceptance-boot-preview.log` 与 `acceptance-open-close-cycles.log`（已去除终端控制序列，其余未改动）。

## 启动

```text
I (383) cpu_start: cpu freq: 180000000 Hz
Camera: SC2336 1024x600 RGB565 registered at /dev/video0
eth0	Link encap:Ethernet HWaddr 60:55:f9:fa:f4:8b at UP mtu 1500
```

上一版固件在此处打印 `emac_esp_new_dma(513): no mem for RX DMA buffers` 和 `board_emac_init failed: -5`；本版内核堆从 32 KB 提高到 128 KB 后 EMAC 正常，`free` 显示内核堆启动占用 84,432 / 131,064 bytes。

## 连续预览（`acceptance-boot-preview.log`）

`desktop camera` 启动后连续运行约 2 分钟：

```text
CSI: HAL ready, 1024x600 RAW8 to RGB565, direct USERPTR capture
[qpk] camera first frame displayed: 1228800 bytes
[qpk] camera preview fps: 29.7 captured=31 dropped=0
...
[qpk] camera health: captured=3600 displayed=3600 dropped=0 heap_free=29329480 heap_largest=29253488 dqerr=0 qbuferr=0 panerr=0
```

- 3600 帧全部采集并显示，`dropped`、`dqerr`、`qbuferr`、`panerr` 全程为 0；
- 相机帧率约 30 fps（传感器满帧率），用户态 `heap_free` 全程恒定 29,329,480 bytes；
- 全程无复位、无错误日志。

## MIPI-DSI FIFO underrun（蓝屏闪烁的客观指标）

下表是归档固件当时的原始验收结果：该二进制通过固定符号地址采样 32 位帧数和 underrun。固定地址仅用于解释历史证据，不再作为新构建的验收接口。0008/0009 之后的源码使用 `/dev/dsi-diag0` 和 `dsi_diag --reset` / `dsi_diag --json` 提供 64 位原子快照、复位及 worker 排队错误计数；新接口仍需在重建固件后补做实板回归。

| 时刻 | 显示帧数 | underrun |
|---|---|---|
| 开机桌面空闲 | 595 | 0 |
| 相机运行 3 s | 826 | 0 |
| 相机运行 +60 s | 4290 | 0 |
| 相机运行 +120 s | 7756 | 0 |
| 关闭相机后 | 8041 | 0 |
| 10 次开关测试结束 | 25,632 | 0 |

对照：上一版三页轮转 + DW-GDMA 整帧拷贝的固件在同一测量方法下为约 33 次/秒（每显示帧一次）。仅缩小 DMA burst（512→64/128）或去掉每帧 cache invalidate 只降到约 33 次/秒，无实质效果；删除二次拷贝、由 CSI 直接写显示页后归零。

## 相机开关 10 次（`acceptance-open-close-cycles.log`）

`desktop camera` / `desktop camera-stop` 循环 10 次，每次都出现 `camera first frame displayed`，无失败。`free` 对比：

| | Kmem used | Umem used |
|---|---|---|
| 循环前 | 85,064 | 3,792,256 |
| 循环后 | 86,024 | 3,791,816 |

用户堆回到基线；内核堆增加 960 bytes（21 个块），与上一版观察到的量级一致，判断为首次开关后的懒初始化残留而非线性泄漏，但尚未做 100 次量级的验证。

## 软件验证

- `make -j8 CROSSDEV=riscv32-esp-elf-` 完整构建通过，无编译警告涉及本次改动文件；
- `boards/.../desktop-v1/defconfig` 由 `make savedefconfig` 重新生成，用 `make olddefconfig` 回放后与构建所用 `.config` 除 `CONFIG_BASE_DEFCONFIG` 外逐行一致；
- 2026-09-04 的 P1 修复链固定为 nuttx `2f1387d5`、apps `88827afd` 和 HAL `78c09290`；补丁文件由 `SHA256SUMS` 校验，应用脚本拒绝错误基线和脏工作树，并以应用后文件摘要验证安全的二次执行；
- `desktop-v1-test` 静态构建已链接 SC2336、DSI 换页、ostest、SMP 和 timerjitter，产物 hash 及环境见 `P1_BUILD_VALIDATION.txt`。该结果来自 WSL2，不替代官方原生 Ubuntu 22.04 或实板验收。

## 已知限制

- 画面颜色和撕裂需要肉眼确认，串口只能证明帧计数、帧率、错误计数和 DSI underrun；本报告不替代目测。
- ROM 对 simple-boot RAM-only header 打印 `SHA-256 comparison failed` 后继续启动，属已知现象，镜像本身已通过 esptool Hash 校验。
- revision v1.0 会打印 NuttX 非量产警告。
- 上一版曾在 1-2 分钟后出现 `rst:0x1 (POWERON)` 复位且无软件错误，判断为供电压降；本版 2 分钟连续测试和 10 次开关中未再出现，但未做长时间复测，建议使用独立 5 V 供电。
- 历史固件开机时偶见未经操作的应用自动启动（`2048`、相机）。P1 修复已加入 GT911 RESET/INT 生命周期、可取消轮询和启动触摸丢弃窗口，但新代码尚未完成 100 次冷启动实板回归，不能将历史问题标记为已关闭。
