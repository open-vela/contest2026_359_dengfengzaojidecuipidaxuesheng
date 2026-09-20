# ESP32-P4 NSH 历史镜像

本目录只保留早期根级 NSH 镜像与 [bootlog.txt](bootlog.txt)。根级 `nuttx.bin`
不是跨 revision 的通用镜像；v1.x 变体见同级的 `../esp32p4-nsh-v1.x/`，当前 v3
变体见 [`../../v3/`](../../v3/)。返回[历史固件索引](../README.md)。

## 使用步骤

核对芯片 ID、板卡 revision、物理下载接口以及目标子目录中的文件。与此目录配套的
[flash_p4_nsh.ps1](../../tools/flash_p4_nsh.ps1)接受 `-Variant`、`-Port` 和必需的
`-Transport`；先从仓库根目录预览命令：

```powershell
powershell -File tools\flash_p4_nsh.ps1 -Variant v1.x -Port COM7 -Transport uart-bridge -DryRun
```

该工具把应用写到 `0x2000`，并检查与 `0x400000` 数据区的边界；不是任意 v3 应用/模型
分区布局的通用烧录器。`v3.2-usb` 控制台配置也不能替代对实际下载接口的识别。

`wsl_copy_firmware.py` 仍有本机路径假设，复制新镜像前先审查源、目标；当前脚本默认
写入 `firmware/v3/`，v1.x 只写入历史归档。
不要把私有配置、设备数据分区或未经核验的完整 Flash 转储归档到这里。
