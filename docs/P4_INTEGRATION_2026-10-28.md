# P4 下午整合与修复（2026-10-28）

已烧录并独立读回校验的固件 SHA-256：
`c5d012c0a3b69d102174b765dd26a7fec76128cefdce42e8aef9b4dc6467b9aa`。
整合本地下午功能及云端 screen_pwm 提交
`5a7089e7038ebeb64df2753051748d1e7e977825`，沿用旧 UI 图标。

- 全局字体：当前全局字体扩大字形缓存，小字体包在后台预载；每轮 GUI 只解析一个字号，切走时收缩非活动缓存。
- 返回手势：左右边缘 28 px、向内 48 px 门槛，接受松手最终坐标；取消逐采样整页平移。
- 音频：统一主音量，ES8311 编解码器增益上限 75%（0 dB）；高采样率媒体每块 960 帧，语音保留 240 帧。用户实听《晴天》确认无电音。
- 网络：C6 TX 轮询每批最多 8 包，保留继续通知并释放网络锁；米家只读同步 4 台设备、200 个实体通过。
- 媒体：照片/视频递归索引、音乐 SD 缓存、补缓冲、拖动与 50 ms 歌词跟随；H.264/MJPEG 原尺寸和全屏、触摸退出及 CSI 预览/停止通过。
- 浏览器：统一固件包含有界 3 MiB 原生执行池和匹配加载器。实际安装模块的冻结源码位于
  `app/netsurf-browser-dev-20261028-integrated/`；阅读/原页往返、正常入口及用户偏好保留通过。
- 中英双语、标题字重、页面布局、导航、BOLL/Grok 残影及空闲心情、录音机修正同时保留。

构建使用 `tools/build_bilibili_v3.py`、GCC 14.2.0 和已锁定的准备输入。
准备流程见 [构建输入资料](P4_BUILD_REPRO_2026-10-27.md)。本整合候选的关键开关为：

```text
--offline-inputs --usb-camera --factory-header --psram-xip
--hardware-jpeg --video-blit --lcd-backlight --ethernet-share
--share-forward-slots 16 --share-net-stats --ble-eight-test
```

应用偏移 0x2000，数据分区 0xc00000/0x400000；烧录前保存私有回退镜像，保留用户分区。
新 BLE 候选身份为 `OpenVela Mouse-DCEE2040` / `F4:0A:DC:EE:20:40`，出厂 MAC/eFuse 保留。

[分项验收摘要](evidence/integrated-20261028/verification.json) 区分宿主和设备验证。
软件重启后的字体选择、双核 XIP、Wi-Fi/米家恢复通过。USB 摄像头未连接，物理断电冷启动、
新 BLE 首次配对/八链路并发、任意 Flash 写擦期间实体闪屏没有完成本轮物理验收。
较早两次串口断连/重启的手动操作与根因未确认，不将其写成已证明的硬件故障。
已拒绝的 cache-off IRQ、Flash 自动挂起和 DSI reload 试验保持关闭。

本次提交不含新图标生成器、几何/动效文件、旧 MCPWM 背光试验、历史浏览器导出、
编译产物或 Flash 私有备份；这些本地文件保留原样。
