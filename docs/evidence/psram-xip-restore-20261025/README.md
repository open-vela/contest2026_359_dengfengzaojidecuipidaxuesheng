# 当前固件 PSRAM XIP 恢复记录

本次基于最新回退固件 731755198bb3190eb314a7dbec5c94a156de9d5c255eff851811737ed3f61aa5 重新构建、烧录和实测，未使用历史 XIP 镜像的验收结果。

新应用 SHA256：`77845a88158ea9e737cbb1230aeed3b0550f9cc48ff0c77d925c8e6ca6ab0030`。ESP32-P4 rev3.2 / 32 MiB PSRAM / 16 MiB Flash，COM4，MAC e8:f6:0a:e7:6f:f1。

| 项目 | 本次结果 |
|---|---|
| 生产源码 | 与回退版本的 648 个输入哈希完全相同 |
| 配置 | 仅启用 ESP32P4_PSRAM_XIP 与四个 SPIRAM 依赖项 |
| 指令 / 常量 VMA | 0x48000000..0x48376200 / 0x48380020..0x489b2924 |
| 启动 BSS / 上限 | 0x4ffabee4 / 0x4ffaefc0 |
| 镜像 / 分区 | 10,371,584 字节，0x2000 烧录，原 12 MiB 应用 / 4 MiB 数据布局 |
| 宿主测试 | 生产验证器正常及损坏/MMU/堆重叠反例；SDK SPI1 100 次互斥锁配对、零 cache-off |
| ELF 审查 | 复制前调用与校准回调闭包、真实镜像偏移、两核 PMP 调用及边界通过 |
| 烧录 | 应用校验及完整独立读回 SHA256 一致；烧录后数据分区哈希与烧录前相同 |
| 首次启动 | ready=1，protected_cpus=3，cache_policy=on，spi1_mutex=1 |
| 2048 并行写擦 | 4 轮，写 3469 次，擦 375 次；IRQ 最大间隔 16624 us |
| CSI 预览并行写擦 | 4 轮，写 1336 次，擦 133 次；IRQ 最大间隔 16646 us |
| CSI 其他实测 | 采样帧率 10.0, 10.0, 9.9, 10.0, 9.8, 9.7, 9.7, 9.5 fps；翻页调用最长 33115 us；本轮 underrun/pan error 均为 0 |
| 实体屏幕观察 | passed：用户重测后反馈没问题，并对 CSI Flash 写擦阶段明确确认这一段也没有问题 |
| 断电冷启动 | pending |

首次启动原始状态：

```text
[psram-xip] verified text=48000000..48376200 hash=97b3255a rodata=48380020..489b2924 hash=31621921 heap=489c0000..4a000000
[psram-xip] ready=1 protected_cpus=3 flash_entries=4174 text_hash=97b3255a rodata_hash=31621921 cache_policy=on spi1_mutex=1
23330816    9296696   14034120    9333408   13994624   3652      8 Umem
77000      61552      15448      61968      14120    242     13 Kmem
```

恢复方式为独立 build.sh 显式传入 `--psram-xip`，通用 builder 默认仍按仓库约定关闭；后续基于本版本构建应保留该选择并重新验收。完整本地制品位于 `C:/Users/11937/Documents/ChatGPT/vela/output/psram-xip-restore-20261025/`，其中 source/ 为当前源码快照、build.sh 为构建参数、firmware/ 为新镜像、rollback/ 为回退与整片备份；配置差异见 CONFIG-DIFF.json。未覆盖工作区用户改动。C6 固件与蓝牙逻辑未修改。

PSRAM 要同时容纳复制后的指令、常量及原有数据，剩余堆以以上本次 free 输出为准。17 个库中 16 个二进制哈希与基线相同，语音适配库在独立目录重新编译，逐源文件及额外优化选项已核对一致。

实际内部 Flash 测试使用固件的独占 .storage-check 目录进行写入、覆盖、读取、删除，不删除用户数据，不以 SD 写入代替 Flash 写擦。原回退镜像和完整 Flash 备份已保存。独立读回、首次启动、后续写擦分别保留日志。

表中采用用户要求重新测试后的 2048 及 CSI 轮次，合计写 4805 次、擦 508 次；不包含之前首次 2048 的 1886 次写与 178 次擦及场景切换期间后台文件系统操作。首次启动 CSI 时，5 秒等待未达到活跃帧条件，脚本在写擦前停止；随后确认 busy=1、alive=1 且帧数增长，再完成本表测试，原始 camera-setup.log 保留。此处没有将未启动预览的准备阶段计为验收成功。

本次未验收 USB 摄像头（当前未连接）、真实云模型、长时间老化或动态 Flash mmap。显示计数正常不能代替实体屏幕观察；pending 项不标为通过。已拒绝的 IRQ/自动挂起/DSI reload 继续禁用。
