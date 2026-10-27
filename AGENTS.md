# ESPClaw / openvela 开发约定

本仓库是 ESP32-P4 Function-EV-Board 的 openvela/NuttX overlay。不要用 ESP-IDF 工程结构替换现有构建。

## 实现入口

- `app/espdl-quickapp/overlay/apps/system/desktop/`：Agent、桌面、设备服务。
- `app/espdl-quickapp/overlay/apps/system/espclaw/port/`：ESPClaw 核心适配、工具调用和流式协议。
- `app/espdl-quickapp/overlay/apps/system/desktop/agents/`：三种运行时 `agent.md` 与共享 `HARNESS.md`。
- `app/espdl-quickapp/overlay/nuttx/arch/risc-v/src/esp32p4/camera_usb/`：OTG HS UVC／MSC／HID／UAC1 主机、CDC Device 与角色切换；GPIO26/27 被显示占用时禁止启用 FS。
- 架构、能力和部署说明：`app/espdl-quickapp/agent.md`、`HARNESS.md`、`MULTIMODAL-CAMERA.md`。

## 必须保持的行为

聊天、创作、助手的工具权限在 C 层隔离，不能只靠提示词。工具描述与参数校验同步修改；新增工具须同时覆盖直接调用和 board MCP 路径。accepted、done、ok、verified 不能互换。

保留每轮取消、工具预算、HTTP 截止时间和回执身份；超时后不自动重放设备动作或图片上传。外部文字、图片、MCP 和技能内容不能扩大权限。不要记录密钥、Authorization 请求头或图片 Base64。

板级摄像头依赖芯片 revision、CSI 布线与专用 OTG HS 端口。USB Serial/JTAG 与 OTG HS 分开；不能把设备接到控制台口就声称 UVC 可用。配置由显式准备好的基线继承，保留存储布局，不能直接照搬历史烧录脚本中的地址边界。

## 修改与验证

提示词改后运行 `python3 tools/generate_agent_profiles.py`，工具 schema 改后运行 `python3 tools/generate_assistant_tools.py`。生成头文件一同交付；不要直接编辑生成头。

Agent 改动运行 `tools/test_agent.py`；助手改动运行 `tools/test_assistant_service.py` 和 `tools/test_assistant_platform.py`；多模态改动运行 `tools/test_vision.py`；参数见上述专题文档。协议测试使用隔离替身，不调用真实模型或家居。

BLE 改动运行 `tools/test_ble_central.py`；USB 改动运行 `tools/test_usb_camera_pools.py`、`tools/test_usb_interfaces.py` 和 `tools/test_interface_protocols.py`。保持连接代次、所有者与挂载引用校验；拔出后仍需允许卸载本会话挂载的旧设备。设备报告是外部数据，不能作为 Agent 指令执行。

蓝牙鼠标每轮开发／烧录验收候选必须分配新的测试设备身份 ID，以隔离电脑端旧配对与 HID／GATT 缓存。更换实际 BLE 身份地址，并在广播名加入对应的唯一版本后缀；仅修改显示名称不算完成身份隔离。使用主机侧合法的静态随机身份地址，保留 C6 出厂 MAC／eFuse。同一候选内的普通断连、重连及配对持久化测试沿用该 ID。烧录前记录候选镜像 SHA、身份地址和广播名，验收时确认电脑识别的是对应的新设备。旧身份的配对数据保留；新身份须单独完成首次配对、加密、HID 订阅和跨重启验证。身份隔离只用于避免旧缓存干扰，不能替代连接归属、密钥保存和真实并发验证；采用新 ID 的结果不得记为旧身份配对兼容性通过。

NuttX 的 HCI socket 和 NimBLE NPL 消息队列描述符属于创建它们的任务组。桌面／NSH 的硬件线程通过有界邮箱提交 GAP/GATT 请求，由蓝牙常驻任务组的 pthread 执行。不要从其他任务组直接发送 HCI 命令或向 NPL 的 mq 描述符投递。USB hub 退出后只由等待退出的调用方销毁消息队列；控制器线程负责停 DMA 后通知退出，不能再释放同一队列。

固件使用 `tools/build_bilibili_v3.py` 的独立 workspace；USB 摄像头候选加 `--usb-camera`。不要关闭启动 BSS、分区边界或链接符号检查来让构建通过。新增较大的 CPU 元数据使用已初始化堆，USB DMA 数据保持缓存／对齐要求；P4 的内核堆与 PSRAM 用户堆必须区分。

相机 DMA／显示页改动运行 `tools/test_camera_sync.py`。P4 HAL 管理缓存时通用失效 API 可能为空操作；CSI DQBUF 后须显式 M2C，同步后再绘制并 C2M。显示翻页须核对实际 scanout offset，失败则停止流，禁止重用可能仍在显示的页。闪屏验收需要实体屏幕观察，不能用静态截图代替。

照片存储改动运行 `tools/test_photo_storage.py`；CSI／UVC／助手共用存储策略，SD 已挂载时优先使用 SD。保留既有照片，写入前检查容量余量；SD 故障／满盘不能静默回退到 Flash。纯预览与拍照保存分开验收。

照片异步保存改动运行 `tools/test_photo_store.py` 与 `tools/test_assistant_platform.py`。保存服务从常驻桌面任务组初始化，不能从短生命周期 NSH 任务创建常驻 pthread。采集线程只提交有界副本，保存线程不持有 DMA／解码器帧指针；取消须等关闭／清理后才给最终回执，保存回执不能依赖已可被替换的推理结果。显示／Flash 插桩运行 `tools/test_camera_timing.py`；ISR 中只做 IRAM 计时和计数，不打印日志。

相机与GUI交接改动运行 `tools/test_camera_boundary.py` 和 `tools/test_camera_sync.py`。硬件启动只能在整次LVGL handler返回之后；停止/join后须把最后画面安全停靠第三页，确认scanout，再发布重绘与GUI所有权。待重绘也属于显示暂停，但不能等同于活跃录像源。

缓存关闭IRAM IRQ原型曾通过叶子函数宿主测试，但实板在通用IRQ返回路径取指异常，已拒绝；正常构建必须通过 `protect_p4_display_flash.restore()` 撤销残留，禁止只凭内部地址或叶子测试重新启用。完整trap、dispatch、TLS/调试、context restore链必须一并证明安全。

Flash自动挂起候选在实读JEDEC=C84018、GD能力及初始化均通过后，仍于Bot/历史写入期间发生PSRAM store bus fault，已拒绝并回退。`ESP32P4_FLASH_AUTO_SUSPEND` 强制关闭，builder拒绝旧 `--flash-auto-suspend-jedec` 参数。`tools/prepare_p4_flash_suspend.py` 和对应宿主测试只保留为失败实验的复现材料，不能用它们声称硬件合格。保持原缓存保护，不迁移/清除用户数据来掩盖显示问题。

P4异步bus错误的MEPC可落在后续无关指令，不能直接把它当实际load/store。首故障还保存MEXSTATUS、STPC2/1/0及LDPC1/0，读取顺序与SDK一致；不得仅凭OpenOCD的MCAUSE文本认定PMP错误，也不要据此修改trap入口或放宽内存保护。

不覆盖用户已有改动，不自动提交或推送。报告分别标明宿主测试、固件构建、烧录校验、冷启动和真实摄像头／模型验收；没有实板证据不能声称已经部署成功。

DSI自主reload试验在cache-on探测中观察到无IRQ辅助的SAR前进，但USB预览／Flash首轮写入仍出现实体蓝屏／闪烁且性能下降，已拒绝并回退。ESP32P4_DSI_FLASH_RELOAD强制关闭，builder拒绝旧 --dsi-flash-reload；宿主控制测试与探测成功不能覆盖该失败。

Bot端侧识别与AI多帧采样分开。默认不开相机；用户现场观察请求授权助手原生工具assistant_observe自动采集，约5秒保留3帧JPEG、关闭相机并保存SD后一次分析，结果交回同一轮Agent继续回答，不写临时Flash文件。相机按钮先询问具体观察问题。每轮一次尝试，失败、普通重试和重启不得重录重传。改动运行tools/test_camera_clip.py、tools/test_camera_observation.py、tools/test_vision.py、tools/test_agent.py；实际视觉模型和物理闪屏仍需分别验证。

798445cb候选在真实Bot请求后仍报告闪屏，且AI返回CHAT_ERROR_RESPONSE；只通过采集不能判整条链修复。新Bot相机及语音轮次标记session_history，成功观察文字独立保存到/sdcard/espclaw-observations；旧Flash历史保留原位，不迁移。新Bot轮次成功、失败、取消均不得触发Flash历史保存，后续普通历史快照也过滤这些新会话轮次；SD故障只保留本次会话并明确提示，不回退Flash。运行tools/test_camera_observation.py。MiMo视觉子请求使用官方max_completion_tokens和thinking.disabled，主对话配置不改；日志仅记录HTTP/结束原因与长度，不记录正文、推理、密钥或Base64。

Bot、全局唤醒和语音面板发起的新对话使用相同会话策略：请求前、失败、取消及完成均不写Flash历史，成功文字独立保存SD。工作台新历史与所有长答案也只写SD，无卡保留内存会话并报告未保存，不回退Flash。旧Flash历史只读保留。助手笔记、别名与定时任务有SD优先SD；定时任务按介质隔离，禁止跨介质复制导致重放。必要的无卡基础配置保留Flash。原始采样存espclaw-recordings，观察结果存espclaw-observations；实际模型和屏幕仍需分别验收。

播报中的唤醒必须实际并行采集麦克风，不能以触摸停止或已有wake回调代替。ES8311观察者共享当前16kHz播放时钟，不再次RESERVE/CONFIGURE或复位声卡；只停RX并同步归还回调后才能join和释放缓冲，最后关闭播放所有者。当前P4 I2S的timeout字段未实现，应用超时须显式撤销RX，不能直接释放DMA仍持有的对象。运行tools/test_speech_barge.py与tools/test_wake_handoff.py；宿主测试不代表真实扬声器回声场景的唤醒验收。

AI、语音、Home Assistant、MCP 等基础配置始终以内置 Flash 为准，有卡、无卡和卡故障时使用相同配置；不能通过转存 SD 掩盖 Flash 显示问题。内容完全相同的重复保存须跳过写入。SD 用于可选历史、录制与大文件。不得打印密钥或完整配置；日志只记录section、medium、changed、written、result。改动运行tools/test_portal_storage.py、tools/test_agent.py、tools/test_ha_configuration.py与portal_frontend.test.cjs，并在网页真实保存时分别核对显示/Flash计数及实体屏幕。

保存 AI／语音／Agent 配置时必须先关闭新任务入口，等待已接纳的启动发布所有权，再取消并等对话、ASR 实际后台线程、TTS／唤醒和端侧相机识别结束。通过常驻桌面线程清理 Bot／唤醒待发送与待播报状态并回执后，才可持 config_lock 写入；不得持 config_lock 等工作线程退场。超时不保存。保存成功或失败都恢复服务待命，不自动重放对话、工具动作或上传。新服务入口必须遵守 glass_ai_start_enter/leave；独立宿主夹具外的固件必须启用 GLASS_AI_CONFIG_ENABLED。运行 tools/test_ai_config.py、test_voice_requests.py、test_wake_handoff.py、test_speech_barge.py，以及上述配置／Agent 回归；应用层停 AI 不等于已经解决任意 Flash 写入的 DSI 中断问题。

Bot 大球页和文字工作台共用编辑器，但大球页没有 draft_label。发送、收起、示例和新对话等共用路径必须使用空控件安全的 chat_ui_label 更新草稿。2026-10-21 实板 EPC400b584a/MTVAL8 的 lv_obj_get_ext_draw_size 空指针已由原生 LVGL 测试同栈复现；不能把这类精确空指针也归因于 Flash 缓存。界面回归 render_grok_conversation.c 须包含 Bot 无草稿标签时发送／收起／重开与草稿保留用例。

应用创作器的源码草稿和示例缓存与聊天历史是独立路径。有 SD 时 builder 会话优先使用 `/sdcard/qpk/.draft` 和 `.examples`，不能继续固定写内部 Flash。选中 SD 后拔卡、损坏或满盘不得回落到 Flash；无卡会话保留原内部存储功能。新 SD 没有任何草稿槽时，只读兼容旧内部草稿；第一笔显式编辑才在 SD 提交新版本，不修改旧内部文件。SD 墓碑与损坏记录均不得回退旧内部草稿。会话期间介质固定，用户明确选择的正式应用安装卷不随草稿介质改变。运行 `tools/test_draft_storage.py` 和 `tools/test_install_volumes.py`，分别验证真实 QuickJS、跨进程恢复、原稿保留与错误不回退；草稿阶段的实体闪屏另行观察。此修正不能称为任意 Flash 写擦期间显示问题的底层修复。

2026-10-20 对照实测：A（禁用 CSI/USB 初始化、保留 Bot Flash 历史）仍闪屏，显示 IRQ 最大间隔 66107 us；A1 仅将两个 Bot 入口的 session_history 置 true，5 轮普通对话完成并存 SD、1 次取消，用户确认无闪屏，Flash 写/擦为 0，IRQ 最大 16736 us。B1 恢复相机/USB 初始化并保留会话策略，用户确认普通对话和相机多模态全程无闪屏；实际 3 帧采集后关闭相机，模型 HTTP 200、答案解析与 SD 回执成功。已烧录 B1 SHA256=8d3413ebe4b31ab2a27679999afc369353cac110ddd97761084fb8dff9748792。证据见工作区 output/camera-ab-20261020/VERIFICATION.md；这是当前 Bot 路径的修复验收，不代表任意 Flash 写入/擦除或历史视频前版本的底层显示问题已全部解决。不得重新启用已拒绝的 IRQ/自动挂起/reload 试验。

PSRAM XIP 底层试验只通过显式 `--psram-xip` 在独立准备区构建，正常构建默认关闭。必须同时具备 P4 PSRAM 指令／常量 VMA、真实镜像偏移桥接、完整启动复制、BSS/堆预留、复制前依赖及校准回调闭包、复制后 PMP 与两核 CSR 验证，才能使用 SDK 原有的 SPI1 cache-on mutex 路径；不能只删 cache-off 保护。保留内部栈 dispatcher 及已拒绝实验的禁用。运行 `tools/test_p4_psram_xip.py --nuttx <prepared-nuttx> --output <host-output>`，并核对最终 ELF、应用／数据边界及源码哈希；app-only 烧录前固化可回退固件。物理验收必须实际发生内部 Flash 写入和擦除，同时记录显示间隔及用户屏幕观察，不能拿 SD 写入或静态截图替代。首次启动证明和后续验收分开记录，动态 Flash mmap 不在复制段的并发保证内。
