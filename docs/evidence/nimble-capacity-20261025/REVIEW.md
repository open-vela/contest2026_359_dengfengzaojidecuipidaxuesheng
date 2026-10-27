# 第 1 项：NimBLE 配置映射修复审查

当前基线是实板验收过的 XIP 固件 77845a88158ea9e737cbb1230aeed3b0550f9cc48ff0c77d925c8e6ca6ab0030。新候选 68013af36b8d2678d321452efcbc6840146d8fd40297284702cb89bb6865f57d，仅改变四个生产输入：NimBLE Makefile、glass_ble.c/.h、c6ble/ble_main.c。C6 固件不变。

示例 syscfg 的默认连接数为 1；修复将 NuttX CONFIG_NIMBLE_BLE_MAX_CONN 通过公共 CFLAGS 传入所有 NimBLE 编译单元，避免只改应用副本。glass_ble.c 编译期拒绝映射不一致；运行时 c6ble status 同时显示配置值与实际编译值。builder 提供显式 --ble-connections 1/2/3，并在 olddefconfig 后复核。

新容量审计器读取真实 Makefile 的完整 CFLAGS，在相同目标编译参数及真实 ble_hs_priv.h 下编译容量/池大小探针，比较最终 ELF 的 ble_hs_conn_elem_mem。新候选配置值=2，编译值=2，目标池尺寸=224 字节，ELF 池尺寸=224 字节；原基线相同审计为 1/1/112/112。真实 Makefile/固定上游 syscfg 的 1/2/3 宿主测试通过，删掉映射的反例被拒绝。central 和 HID 邮箱 ASan/UBSan 回归通过。

不修改角色锁、会话、GAP/GATT 请求、HID 所有权及配对存储；这一步证明容量映射修复，不代表双业务并发已经实现。新增容量函数只返回编译常量，没有资源、线程或副作用。status 分别读取两个已有快照，不记录密钥或设备地址。

XIP 准备源码哈希与基线完全相同，最终 ELF 重新审计：复制前正常直接调用及校准回调无 PSRAM 代码依赖，四处间接调用仍为相同内部地址的 s_do_tuning；PSRAM 数据引用仍仅为段边界数值。指令结束 0x48376300，常量结束 0x489b2994，PMP 对齐与镜像偏移通过；BSS 结束 0x4ffabf9c 小于 0x4ffaefc0。保留 SDK SPI1 mutex、复制/CPU0 保护准入、CPU1 ready 前保护及 CSR 回读。未启用被拒绝的 cache-off IRQ、自动挂起或 DSI reload。

checkpatch 已执行，但现有压缩格式、SPDX-only 文件布局、Windows 换行及挂载权限被大量报告，默认 riscv64-unknown-elf-gcc 缺失；不能标为全量 checkpatch 通过。本次新增函数和状态查询按现有 API 边界检查；固件使用明确的 ESP RISC-V GCC14 工具链成功构建，未以关闭编译/链接检查掩盖问题。

烧录前保留完整 XIP 回退制品；仅应用区写入。最终烧录、设备初始化和 Flash/显示保留验证另记，不能使用上一版物理观察替代本候选验收。
