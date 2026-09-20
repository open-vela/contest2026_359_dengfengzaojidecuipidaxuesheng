# SC2336 摄像头移植实机验收报告

测试时间：2026-08-30（Asia/Shanghai）。目标板为 ESP32-P4 Function EV Board，芯片 revision v1.0/ECO2，摄像头为 SC2336，串口为 COM7/115200。

最终固件大小为 2,983,768 bytes，SHA-256 为 `263cd1e9a015b94d28dfe01853755fbaca01bf2fa0b641450046dbfbadd5259f`。esptool 5.3.1 已将镜像写入 `0x2000`，并通过写后 Hash 校验。

已完成四次独立冷启动取帧验证。每次均完整收到 614,400 bytes，且像素范围不恒定、FNV-1a 随画面变化，排除了零填充缓冲区或固定测试数据的伪成功。其中三次校验值为 `0x36bdb632`、`0x917c8acc` 和 `0x11034bf3`。

最终串口证据：

```text
SC2336: stream register=0x01
Camera: first frame OK, 614400 bytes, range=0..35, fnv1a=0x11034bf3
Camera: SC2336 1024x600 BGGR8 registered at /dev/video0
nsh> ls /dev/video0
 /dev/video0
```

软件验证包括：完整 `make -j8` 构建成功，NuttX `tools/checkpatch.sh` 对五个新驱动/板级文件全部通过，`git diff --check` 无空白错误，摄像头增量补丁在基础 overlay 之后通过 `git apply --check`。

实机通过项包括：SCCB 芯片 ID `0xcb3a`、SC2336 stream-on 回读、MIPI D-PHY 高速信号、CSI Host、ISP RAW8 bypass、DW-GDMA 中断、PSRAM 帧缓冲、首帧内容检查以及 `/dev/video0` 注册。

已知限制：ROM 会对 simple-boot RAM-only header 打印 `SHA-256 comparison failed` 后继续启动，写入镜像本身已经通过 esptool Hash 验证。revision v1.0 仍会打印 NuttX 的非量产警告。
