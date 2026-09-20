# 当前 v3 固件

这里是仓库默认展示和复现入口。目录中的镜像面向 ESP32-P4 v3.x revision；它们与
[`历史测试固件/`](../历史测试固件/) 中的 v1.0/ECO2 镜像严格分开。

## 镜像

- [`esp32p4-desktop-v3.2-candidate/`](esp32p4-desktop-v3.2-candidate/)：桌面/LVGL v3.2 构建候选，含 `nuttx.bin`、解析配置、构建元数据和 SHA-256 清单。
- [`esp32p4-nsh-v3.2/`](esp32p4-nsh-v3.2/)：v3.2 UART NSH 构建产物。
- [`esp32p4-nsh-v3.2-usb/`](esp32p4-nsh-v3.2-usb/)：v3.2 USB Serial/JTAG 控制台构建产物。
- [`esp32p4-application-v3-20260919.md`](esp32p4-application-v3-20260919.md)：实际烧录到 v3.2 实板并完成 Home Assistant、BLE 触摸板和 UI 验证的应用镜像记录。

## 状态边界

`esp32p4-desktop-v3.2-candidate` 是 2026-08-27 的独立旧构建候选，仍按其目录说明保留“未完成实板验收”的边界。在此之后，2026-09-19 的本地 v3 应用镜像已经在 v3.2 实板上完成烧录、启动、Home Assistant UI、连接服务和 BLE 触摸板验证；两者不是同一个二进制文件。所有镜像仍禁止烧录到 v1.0/ECO2 板。

当前本地已验证的 v3 应用版本是源码优先的交付基线：Home Assistant 卡片 UI、QuickJS 原生服务桥接、BLE HID/触摸板和 C6 入口均在
[`app/espdl-quickapp/`](../../app/espdl-quickapp/README.md)，实板烧录和 UI 证据在
[`logs/2026-09-19/`](../../logs/2026-09-19/)。

## 复现和校验

从仓库根目录运行：

```bash
python3 tools/check_package.py
```

它会逐个读取各目录的 `SHA256SUMS`，并拒绝跨目录引用、缺失文件和不应提交的构建产物。构建固定点和配置见各镜像目录的 `BUILD-METADATA.txt`、`defconfig` 与 `resolved.config`。
