# 家居（Home Assistant 面板）

OpenVela QPK 桌面应用：Lovelace 风格家页面，通过 REST 连接局域网里的 Home Assistant（树莓派 / Green）。

## 布局

- `quickapp/homeassistant/`：设备端 `app.js`、网页预览 `preview.html`、设计对照 `ref-db.json`
- `app/homeassistant/`：QPK C 客户端 `qpk_homeassistant.*`、资源生成脚本、测试

## 约束

- QPK 最多 16 个事件、64 个控件
- HTTP 仅 `config` / `states` / `services` 与 `GET /api/states/{entity_id}`
- 实体身份对齐 HA core：`(domain, platform, unique_id)`
- 实时状态用 5 秒静默轮询，不是 WebSocket

## 预览

仓库根目录起静态服务后打开：

`camera-app` 开发时为 `http://127.0.0.1:8787/camera-app/homeassistant/preview.html`

本目录下直接打开 `preview.html` 也可看 Lovelace 家/能源分区。

## 连接真实 HA

1. 在 HA 个人资料创建长期访问令牌
2. 设备端保存地址 `http://homeassistant.local:8123` 与令牌
3. 同步后按 entity_id 注册槽位

不要把令牌写进仓库。
