# PPA RGB565 候选 13

新代码已构建，未部署。本轮没有访问设备或启动个人验收。

- [状态](STATUS.json)：镜像/ELF SHA、大小、BSS、身份、部署状态及验证范围。
- [9 组自动回归](HOST.json)：PPA 应用、PPA 驱动序列、DMA2D、旧 JPEG 适配器、相册、
  相机缓存/scanout、MJPEG 预览、GUI 交接、XIP。DMA2D 回归包含 PPA/JPEG 的 P4 目标单元编译。
- [最终 ELF](ELF-AUDIT.json)、[源码/配置对照](XIP-INDEPENDENT-EVIDENCE.json)、
  [闭包人工阅读](XIP-REVIEW.md)、[预期 XIP 哈希](EXPECTED-XIP-HASHES.json)。
- [格式检查](STYLE.json) 保留整文件报告，未标为全通过。

实际测试执行生产函数体和官方 SDK 块计算函数，但 PPA 寄存器/设备传输由宿主替身提供。
因此这些记录不证明物理 RGB565 插值、颜色或性能结果。旧 UYVY ioctl/job 布局保持不变。
此前 C12 的 BLE 并发源码保留，未把 C12 宿主结果重记为 C13 的实板结果。

原始构建：`output/network-share-20261026/candidate-13/`。
原始回归及保留的夹具失败记录：`output/ppa-rgb565-20261027/`。
冻结源码与当前仓库源码哈希一致；没有 Git 提交或推送。
稳定下载 5 Mbps 仍是设计目标，验收保持 `skipped_by_user`，没有新增吞吐数据。
