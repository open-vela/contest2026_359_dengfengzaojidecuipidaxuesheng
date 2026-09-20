# ESP32-P4 v3.2 USB NSH

这是面向 ESP32-P4 v3.x revision 的 USB Serial/JTAG 控制台变体，不是 v1.0/ECO2 兼容镜像。

- 镜像：`esp32p4-nsh-v3.2-usb.bin`
- 配置：`nuttx-v3.2-usb.config`
- 应用偏移：`0x2000`
- 控制台和下载：P4 原生 USB Serial/JTAG，端口号以系统枚举结果为准

从仓库根目录可执行：

```powershell
powershell -File tools\flash_p4_nsh.ps1 -Variant v3.2-usb -Port COM23 -Transport usb-jtag -DryRun
```

先确认芯片 ID 和 USB 下载接口，再去掉 `-DryRun`。该镜像只代表 v3.2 构建候选，当前仓库没有对应 v3.2 实板验收记录。
