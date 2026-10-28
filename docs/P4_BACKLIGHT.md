# P4 LCD 背光调光

接线遵循乐鑫 Function-EV-Board 用户指南：LCD 适配板 **J6 PWM → 开发板 J1 GPIO26**；LCD 复位继续使用 GPIO27。GPIO26/27 的 USB FS 占用保护保持启用，OTG HS 与 Serial/JTAG 保持原配置。

`tools/build_bilibili_v3.py --lcd-backlight` 将 LEDC timer0/channel0 配置为屏幕专用 GPIO26；`tools/build_p4_repro.py` 的完整配置显式带此参数。PWM0 不再作为应用或 Agent 的通用扩展通道，实际可用通道以 `hardware_capabilities` 为准。通用构建未选择该参数时保留原 PWM0/GPIO2。

控制面板的亮度滑块使用 5 kHz 硬件 PWM，范围为 10%–100%。拖动即时改变背光，丢失触摸时恢复本次拖动之前确认的亮度；驱动失败显示“调节失败”。100% 使用 65535，占空比不使用会在驱动 uint16_t 转换时归零的 65536。启动默认为 100%，当前只保存本次开机的亮度，不在拖动时写 Flash。

PWM 文件描述符由常驻桌面任务组持有。设置仅在桌面启动/LVGL 回调执行；NSH 的 `desktop backlight-status` 只读取线程安全的状态快照，不跨任务组访问 PWM fd。

主机验证：`python3 tools/test_backlight.py --output <host-output>`；`tools/test_board_interfaces.py` 包含背光通道不可被应用发现的配置用例。实板验证还需核对构建配置、PWM0 节点、状态回执，并观察实体屏幕的低/中/高亮度及恢复；软件回执不能替代屏幕观察。

官方接线说明：https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4-function-ev-board/user_guide.html
