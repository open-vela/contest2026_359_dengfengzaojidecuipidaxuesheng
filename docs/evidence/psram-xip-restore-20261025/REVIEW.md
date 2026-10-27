# 当前回退基线恢复 PSRAM XIP：烧录前审查

基线应用 SHA256：731755198bb3190eb314a7dbec5c94a156de9d5c255eff851811737ed3f61aa5。
候选应用 SHA256：77845a88158ea9e737cbb1230aeed3b0550f9cc48ff0c77d925c8e6ca6ab0030。

648 个生产源码输入与基线完全一致。配置差异仅为 ESP32P4_PSRAM_XIP 及其四个 SPIRAM 选择项；存储布局、相机、蓝牙、语音及缓存配置保留。17 个外部/生成库中 16 个二进制哈希相同，libspeech_nuttx.a 在独立路径重编；逐个语音源文件、实际编译源哈希与额外优化选项均相同，不能将该库表述为二进制完全相同。

最终 ELF 的指令段为 0x48000000..0x48376200，常量段为 0x48380020..0x489b2924。指令保护边界按 128 字节对齐；镜像真实 Flash 偏移与 PSRAM 虚拟地址的低 16 位一致。启动 BSS 结束 0x4ffabee4，小于保留边界 0x4ffaefc0；镜像结束小于 0xc00000 数据分区。

本次重新遍历最终 ELF：复制前正常直接调用与八个校准回调无 PSRAM 代码依赖。四个间接调用均在 s_do_tuning.constprop.0；已读取当前 SDK 的驱动赋值，P4 DQS phase/delayline 分别注册 init、tune、read、select-best、set-best，共八个不同目标，均由链接审计覆盖。PSRAM 地址引用只涉及指令/常量段边界数值，不是复制前提前访问应用常量。SDK fatal/assert 终止路径不作为可继续运行的启动路径。

保留原早期 APM、复制后 CPU0/CPU1 PMP 设置与 CSR 回读验证；CPU1 在 ready 发布前保护。SPI1 实际链接保留 mutex 与 XIP 准入检查，未链接其 cache-off 分支。内部栈 dispatcher 保留，已拒绝的 IRQ、自动挂起、DSI reload 继续关闭。动态 Flash mmap 不在复制段并发保证内。

宿主生产校验器通过正常、损坏、MMU 目标错误、堆重叠及未完成保护准入反例；真实 SDK SPI1 100 次互斥锁配对、零 cache-off 通过。固件构建及本次 ELF 审计通过。烧录、启动和实际写擦及实体屏幕观察需要另行记录；此文件本身不证明物理验收成功。
