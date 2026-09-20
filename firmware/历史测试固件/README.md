# 历史测试固件

这里保存 v1.0/ECO2 板卡和早期驱动实验的可追溯材料。它们不属于当前默认版本，GitHub 浏览时请先返回 [`firmware/v3/`](../v3/)。

## 内容

| 目录 | 内容 | 说明 |
| --- | --- | --- |
| `esp32p4-desktop-v1.0-release/` | v1.0 桌面发布包 | 仅适配 v1.0/ECO2；保留原始验收报告和哈希 |
| `esp32p4-desktop-v1/` | v1 桌面验收镜像 | 历史回归记录 |
| `esp32p4-camera-preview-v1.0/` | RGB565 相机预览 | 早期预览交付 |
| `esp32p4-sc2336-camera-v1.0/` | SC2336 RAW8 移植 | 实验性相机驱动 |
| `esp32p4-nsh-v1.x/` | v1.x NSH 变体 | 与 v3.2 NSH 分离保存 |
| `esp32p4-nsh-root/` | 早期根级 NSH 镜像和启动日志 | 仅作历史证据 |

## 使用边界

这些目录中的 `nuttx.bin`、配置、报告和启动日志是历史材料。它们不能证明当前 v3 应用功能，也不能跨 revision 烧录。使用前必须阅读同目录 README、确认板卡芯片 revision，并在同目录执行哈希核对。

仓库根目录的 `python3 tools/check_package.py` 会继续校验这些历史文件，因为它们是比赛复现和审计材料；校验通过不等于当前硬件验收通过。
