# 原生 JPEG candidate-5 证据

当前镜像 `1165ffbe30d98dc713b01cbbdbdaa75adff9cd6c59f6577df9a56b028a9d8902`。
实现和能力边界见 `../../NATIVE_JPEG_2026-10-26.md`。

`RUNTIME.json` 记录两轮各 22 个真实硬件解码组合、错误恢复、PPA 交替、尾部
守护区与有界堆用量检查。`DEPLOYED.json` 区分应用独立读回、启动前数据区一致、
暖启动、双核 XIP、手机跳过和其余待验收项。`HOST.json` 为九项宿主回归，
`STYLE.json` 为六个新增芯片驱动文件的 nxstyle 结果。

`ELF-AUDIT.json`、`XIP-INDEPENDENT-EVIDENCE.json` 和 `XIP-REVIEW.md` 对应当前
最终 ELF；JSON 中为兼容旧收集器而保留的 candidate3 比较标签，实际指网络
candidate-4，具体镜像 SHA 已逐项列出。完整原始制品及日志保留在工作区
`output/jpeg-adaptation-20261026/candidate-5/`，失败候选 2/4 也保留原记录。
此目录不包含原始 Flash 备份、配置、令牌、配对密钥或用户照片。

当前未完成实体显示/Flash/CSI、新身份配对和断电冷启动。AGENTS.md 要求
“闪屏验收需要实体屏幕观察，不能用静态截图代替”；自动像素检查不代替这一项。
手机热点网页验收由用户明确跳过，未记为通过。硬件 JPEG 编码仍为下一项。
