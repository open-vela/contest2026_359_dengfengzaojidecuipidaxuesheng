# 有线热点共享验收证据

当前设备为候选 11 `11aaf366`，见 [部署记录](candidate-11/DEPLOYED.json) 和
[待测清单](candidate-11/RUNTIME-DEFERRED.json)。仅增加静态转发池 4→16 和网络统计；
最终 ELF 检查、烧录独立回读、4 MiB 数据保留与 XIP 启动检查通过。
用户要求后续只设计和修 bug、不再配合测试，因此手机、吞吐、Flash/CSI 实体观察、
冷启动和新 BLE 配对全部待测。5 Mbps 目标尚未达成，C10 checksum 回归未解除。
后续不会自动开启热点、运行压力测试或再烧录。

后续回归优先看 [候选 10 大流量失败记录](candidate-10/RUNTIME-REGRESSION.json)：
正式下载只有一个完整窗口约 0.96755 Mbps，第二轮 checksum 中止，尚无完整上下行基线。
后续短测上下行校验通过，但未解除长测完整性失败；提速/性能发布保持未验收。

此前候选 10 `1b963fd4` 的历史记录见 [部署记录](candidate-10/DEPLOYED.json) 和
[PHY/生命周期检查](candidate-10/RUNTIME-PARTIAL.json)。BGN/HT20 与 BGNAX/HT20 的
回读、关闭及 HE 重开通过，AP/DHCP/NAT 状态正常。
[手机验收记录](candidate-10/RUNTIME-PHONE.json) 记录 2026-10-27 用户确认：关闭移动数据后
上网和屏幕正常，手机显示 Wi-Fi 6；设备读到 1 个 HE 能力客户端及双向转发，NAT 峰值 424/2048、
限额拒绝与分配失败 0。尚无独立空口逐帧 PHY 测量。
[本轮完整验收](candidate-10/RUNTIME-ACCEPTANCE.json) 补充了 2048/CSI 各两轮 Flash 写擦：
写 1489 次、擦 153 次，相机采集/显示 300 帧、丢帧 0，用户确认两个场景全程正常。
有真实流量后关闭使 AP/DHCP/NAT 退出，重开 HE/20 MHz READY；未独立确认重开后第二轮手机流量。
冷启动、吞吐、新 BLE 身份配对与长期负载仍待验收。
STA 到路由器的 PHY=6 不作为手机到热点协商的证据。候选 9 的
[UID 失败记录](candidate-9/RUNTIME-REJECTED.json) 保留，不能把它理解为 C6 不支持 HE。

稳定回退候选 8 `232409de` 将容量从 512 扩为 2048。见
[部署记录](candidate-8/DEPLOYED.json) 和 [手机/生命周期验收](candidate-8/RUNTIME-PHONE.json)：
用户确认网页稳定、屏幕正常，NAT 峰值 239/2048、限额及分配失败 0，有流量的关闭/重开通过。
实体确认只覆盖普通浏览；Flash/CSI、冷启动和长期运行仍待验收，未独立测得重开后的第二轮手机流量。

候选 7 历史结果见 [部署记录](candidate-7/DEPLOYED.json) 和
[有限运行检查](candidate-7/RUNTIME-PARTIAL.json)：有线上游、热点开关、STA 重新获址通过；
后续 [手机浏览与有流量关闭检查](candidate-7/RUNTIME-PHONE.json) 记录用户确认网页稳定、屏幕正常，
且有流量关闭/重开通过。但容量达到 512、限额拒绝 28 次，候选 8 已部署以扩充容量。
本次实体确认不包括 Flash/CSI 压力。候选 6 已因 DHCP 发送路径锁循环拒绝，
详见 [候选 6 失败记录](candidate-6/RUNTIME-FAILED.json)。候选 7 改为常驻线程处理发送通知，
构建及宿主对照通过不等于手机/实体显示通过。以下候选 5 记录仅为历史有限场景结果。

## 候选 5 历史实板记录

镜像 `574bcd6a`。完整 SHA、验证项目、用户确认和原始日志哈希见 RUNTIME.json。
用户确认关闭移动数据后可上网；第一轮真实流量后点击关闭，AP/DHCP/NAT 均停止，重开成功。
第二轮上网为用户确认，转发计数未继续增长，未记为独立测得第二轮或 Flash 期间网络流量。
2048 和活跃 CSI 各两轮 Flash 自测，合计写 1597 次、擦 167 次；用户分别确认屏幕正常。
JPEG 解码 22 组、编码 19 组及 PPA 像素检查在当前镜像重新通过。

候选 5 的生产源码、SDK 和配置与失败候选 4 相同，只重新生成 BLE 测试身份。
因此当前通过不等于已定位并消除先前的偶发阻塞。冷启动、长期稳定性、新身份首配、
此前跳过的手机配置保存仍未验收。HOST/STYLE 的继承范围以各项字段为准。
XIP-REVIEW.md 是烧录前审查记录，其中 pending 状态由 RUNTIME.json 的本轮证据补充。

这里只导出白名单验收元数据；Flash 备份、配置、凭据和原始图像不入仓。

## 后续回归（覆盖先前稳定性结论）

用户随后报告网页时好时坏及短暂蓝屏。现场 NAT 表达到 64/64，IOB 空闲 96；稍后 NAT 降至 44 条。见 RUNTIME-REGRESSION.json。前述记录只保留为当时有限场景的通过，候选 5 整体稳定性未接受。
