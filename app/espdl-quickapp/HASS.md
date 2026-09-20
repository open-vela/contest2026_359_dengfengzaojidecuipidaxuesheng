# Home Assistant 原生智能家居

本文说明 2026-09-17 合入的 Home Assistant（HASS）原生集成。它不是小米官方米家
客户端，也不登录小米账号：需要先把米家等设备接入用户自己的 Home Assistant，再由
本固件通过用户配置的 HTTP 地址直连。

## 入口与界面

- 桌面“智能家居”卡片、Dock 家居按钮、全部应用列表和 `desktop ui ha` 都启动内置
  QuickJS `homeassistant/app.js`（与 ZIP 提供的 `app.js` 同源），通过共享的
  `system.homeAssistantService` 访问现有 Home Assistant 连接；旧的
  `glass_hass_ui.inc` 保留为原生回归/参考实现。
- `desktop ui ha-settings` / `desktop ui ha-address` 直接打开 Portal 的智能家居配置页。
- 未启用 `CONFIG_SYSTEM_HASS` 时，QuickJS 应用仍可启动，但不会获得原生服务授权；
  正式连接和控制验证仍要求启用该配置。

## 源码结构

- `overlay/apps/system/hass/`：共享异步 HASS 服务与 HTTP 传输层
  （`hass_main.c`、`hass_service.c`、`hass_transport.c`），由 `CONFIG_SYSTEM_HASS=y` 启用。
- `overlay/apps/system/desktop/hass_portal.c`：Portal 配置读写与授权校验。
- `overlay/apps/system/desktop/hass_qjs.c`：QuickJS 兼容桥，供 QPK 侧调用同一服务。
- `overlay/apps/system/desktop/hass_ui_auth.h`：由 `homeassistant/app.js` 生成的
  SHA-256 授权绑定，必须与同份 JS 一起重新生成。
- `overlay/apps/system/desktop/glass_portal_config.c`：智能家居配置的
  `portal_ha_value()` / `portal_ha_save()` 持久化，不向 HTTP 回传令牌。
- `overlay/apps/system/desktop/glass_portal_ui.inc`：Portal 返回路径按来源区分
  “返回家居 / 返回设置 / 返回聊天”。
- `app/homeassistant/`：HASS 原生服务与页面的宿主回归测试。

## 能力边界

- QuickJS 页面通过共享 `hass_open(HASS_READ | HASS_CONTROL)` 服务读取 `states`
  缓存，状态有效期 10 秒，并每 10 秒自动同步；控制只走 `hass_control()`，且仅
  接受 `light`、`switch`、`input_boolean`、`fan`，UI 层没有放宽服务白名单。
- 控制返回 HTTP 2xx 后不会乐观改写卡片。原生页在 8 秒内最多 4 次读取原实体，
  开关状态一致且亮度误差不超过 1 个百分点才显示确认成功；否则明确提示结果未确认，
  不自动重发控制命令。没有 `brightness` 属性的灯不显示亮度滑杆。
- 原生页按 2×3 卡片分页展示，支持全屋、常用、环境分类和扩展的只读 HA 实体域；
  一页 6 张不再等于只显示前 6 个实体。最多保存 128 个可展示实体，超出时页面
  明确提示截断，不把总数误报为已全部载入。
- 底栏只保留设备控制、刷新和连接设置；旧 QPK 高级管理页不进入原生固件构建。
  完整 `states` 响应超过 64 KiB 时，原生页会提示在 HA 中减少公开实体。
- 配置接受设备 Portal 中显式授权的 `http://主机[:端口]`，包括公网 IPv4 地址；不支持
  任意子路径和 HTTPS。HTTP 会以明文传输令牌，应自行确保链路可信。
- 401/403 会清除已保存令牌，需要重新在 Portal 配置。
- 仓库不保存 Home Assistant 地址、长期访问令牌或设备 `/data`。

## 验证

- 宿主回归：`node app/homeassistant/tests/homeassistant.test.cjs`。
- 构建：重新生成 `homeassistant_resource.c` 和 `hass_ui_auth.h`，启用
  `CONFIG_SYSTEM_HASS=y`，执行完整 `make olddefconfig/context` 后再做一次干净的
  ILP32F 构建，不混用旧对象。
- 实板证据：`evidence/firmware-validation.json`、
  `evidence/mijia-ha-0.7.0-runtime.json`、`evidence/mijia-ha-0.7.0-ui-audit.json`、
  `evidence/ha-native-refresh-icon-boot-20260917.json` 与对应的
  `evidence/flash-ha-native-*-20260917.log`。
- 2026-09-17 的已烧录镜像为 7028888 bytes（SHA-256
  `6d3139f33c3ac95458620d3b9937eb2e380c0371541fb68263a9fb38654c59cf`），已通过
  8 MiB 程序分区检查；同日 esptool 写入与 “Hash of data verified” 记录见
  `evidence/mijia-ha-0.7.0-flash.log`。整机镜像本身不随源码提交，因为它包含无关
  系统模块与私有配置。

2026-09-18 的分页、扩展实体卡片与控制确认属于后续源码版，须以新的构建报告为准；
在重新烧录和实板控制前，不沿用 2026-09-17 的硬件通过结论。
当前源码版完整构建为 7038028 bytes（SHA-256
`5dea759c4a63753935bb1aaf4a29ea646a0631d1b14296e5914c31726e9f0ac7`），
SRAM 链接占用 469524 / 978880 bytes，程序分区、ILP32F ABI、关键符号和启动静态区
边界检查通过；`hardware_tested` 仍为 `false`。
