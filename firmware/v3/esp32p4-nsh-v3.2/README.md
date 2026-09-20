# ESP32-P4 v3.2 UART NSH

这是面向 ESP32-P4 v3.x revision 的 UART NSH 构建产物，不是 v1.0/ECO2 兼容镜像。

- 镜像：`esp32p4-nsh-v3.2.bin`
- 配置：`nuttx-v3.2.config`
- 应用偏移：`0x2000`
- 控制台：UART0，按板卡说明使用对应串口和 115200 波特率

从仓库根目录可执行：

```powershell
powershell -File tools\flash_p4_nsh.ps1 -Variant v3.2 -Port COM7 -Transport uart-bridge -DryRun
```

先确认芯片 ID 和物理下载接口，再去掉 `-DryRun`。该镜像只代表 v3.2 构建候选，当前仓库没有对应 v3.2 实板验收记录。
