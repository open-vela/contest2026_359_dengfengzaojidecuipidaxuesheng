# 米家 HA 原生页：Home Assistant 卡片复刻说明

本页把 Home Assistant 前端的设备卡片语言搬到 LVGL 原生实现里。所有数值都取自
HA 前端源码，而不是凭观感猜测；抓取到的原始文件保存在
`diagnostics/ha-design-reference/`。

## 依据文件

| HA 前端文件 | 用途 |
| --- | --- |
| `resources/theme/color/color.globals.ts` | 明暗主题底色、文字色、分隔线、状态色 |
| `resources/theme/core.globals.ts` | 圆角 token（lg = 12 px，pill 全圆） |
| `resources/theme/typography.globals.ts` | 字号 token（s 12 / m 14 / l 16 / xl 20 / 3xl 28） |
| `components/ha-card.ts` | 卡片默认：1 px 分隔线描边、无阴影 |
| `components/tile/ha-tile-icon.ts` | 图标徽章：36 px 全圆、状态色 20% 底 |
| `common/entity/state_color.ts` | 状态色四级覆盖顺序 |
| `common/entity/state_active.ts` | 各域“激活”判定 |

## 主题色值

| 语义 | 浅色 | 深色 |
| --- | --- | --- |
| 页面底色 | `#fafafa` | `#111111` |
| 卡片底色 | `#ffffff` | `#1c1c1c` |
| 次级容器 | `#eeeeee` | `#282828` |
| 分隔线 | `#e0e0e0` | `#3a3a3a` |
| 主文字 | `#212121` | `#e1e1e1` |
| 次文字 | `#727272` | `#9b9b9b` |
| 未激活/不可用 | `#9e9e9e` / `#bdbdbd` | `#9e9e9e` / `#6f6f6f` |

主题主色（按钮、开关轨道、选中态）沿用米家绿 `#15956b`／`#43c996`，等价于
HA 中用户自定义的 `--primary-color`。

## 设备状态色

按 `state_color.ts` 的顺序 `--state-<domain>-<device_class>-<state>-color` →
`--state-<domain>-<state>-color` → `--state-<domain>-<active|inactive>-color`
→ `--state-<active|inactive>-color` 解析：

| 域 | 激活色 | 备注 |
| --- | --- | --- |
| `light` | `#ffc107` amber | 图标按亮度做 CSS `brightness()` 缩放 |
| `switch` / `input_boolean` | `#ffc107` | 关闭时图标 `#9e9e9e` |
| `fan` | `#00bcd4` cyan | |
| `cover` | `#926bc7` purple | 非 `closed` 即激活 |
| `media_player` | `#03a9f4` light blue | 非 `off`/`standby` 即激活 |
| `vacuum` | `#009688` teal | |
| `lock` | `locked` 绿、`unlocked`/`jammed` 红、动作中橙 | |
| `climate` | heat `#ff6f22`、cool `#2196f3`、heat_cool `#ffc107`、auto 绿、dry 橙、fan_only cyan | |
| `binary_sensor` | `#ffc107`，危险类（门磁/烟感/漏水/燃气等）红 | |
| `sensor` | `#44739e`（电池：>50% 绿、≤50% 橙、≤10% 红） | 数值本身用主文字色 |
| `alarm_control_panel` | armed_* 绿、arming/disarming/pending 橙、triggered 红 | disarmed 为未激活 |
| `humidifier` / `valve` | `#2196f3` 蓝 | valve 非 closed 即激活 |
| `water_heater` | eco 绿、electric/gas/heat_pump 橙、high_demand/performance 深橙 | |
| `lawn_mower` | `#009688` teal，error 红 | docked/paused/idle 为未激活 |
| `siren` | `#f44336` 红 | |
| `camera` | `#ffc107`（未单独定义，回落到 `--state-active-color`） | streaming/recording 才激活 |
| `person` / `device_tracker` | home 绿，其它激活态蓝 | not_home 为未激活 |
| `sun` | above_horizon 琥珀、below_horizon 靛蓝 | |
| `weather` | sunny 琥珀、clear_night 深紫、cloudy 浅灰、partlycloudy 蓝灰、rainy 蓝、pouring 靛蓝、snowy `#c0e0ff`、snowy_rainy 浅蓝、fog 灰、hail 青、lightning 黄、lightning_rainy 黄绿、windy 绿、exceptional 红 | |
| `update` | 橙 | |
| `plant` | 红（仅 problem 激活） | |
| `timer` | 琥珀（仅 active 激活） | |
| `scene` / `script` / `button` | 琥珀 | 无状态色，属 `--state-active-color` |
| `input_number` / `number` / `select` / `input_select` / `group` / `remote` / `automation` | 琥珀；数值域用主文字色 | |

## 支持的实体域

卡片按域渲染，分三类版式：

- **数值卡**（28 px 读数 + 次行）：`sensor`、`climate`、`weather`、
  `water_heater`、`input_number`、`number`
- **状态卡**（20 px 状态词 + 次行）：`binary_sensor`、`lock`、
  `media_player`、`person`、`device_tracker`、`alarm_control_panel`、
  `update`、`timer`、`scene`、`script`、`button`、`sun`、`plant`、
  `camera`、`remote`、`automation`、`siren`、`lawn_mower`、`group`、
  `humidifier`、`valve`、`select`、`input_select`
- **控制卡**（16 px 状态 + 胶囊开关 + 进度条）：`light`、`switch`、
  `input_boolean`、`fan`；`cover`、`vacuum` 用同一版式但只读

状态词中文化由 `g_hass_ui_state_texts[]` 表驱动（布防、加湿、热水模式、
天气、割草、计时等 60 余条），未收录的域回落显示原始 state。

## 分页

一屏 6 张卡片（2 × 3）。筛选结果超过 6 台时，标题右侧会出现翻页按钮
（位于状态 chip 下方），副标题显示「x/y 页」；切换「全屋／常用／环境」
会回到第一页。原生页最多保存 128 个可展示实体；超过时继续统计总数，并在右侧
状态区明确提示只显示前 128 个。状态色与中文化都集中在两张 const 表里，新增域
只需追加表项并在 `hass_ui_symbol()` 里补一个图标映射。

## 卡片版式（232 × 118，网格 244 × 128）

| 元素 | 位置与规格 |
| --- | --- |
| 卡片 | 圆角 12，1 px 分隔线描边；选中为 2 px 主色描边 + 浅主色底 |
| 图标徽章 | `(14,14) 36×36` 全圆（ha-tile-icon 原尺寸），24 px 状态色图标 |
| 名称 | `(60,10)` 20 px 主文字 |
| 数值（传感器/空调/天气等） | `(60,38)` 28 px 主文字 |
| 状态行 | `(60,44)` 16 px 主文字（ha-tile-info 两行同为 `--primary-text-color`） |
| 次行 | `(60,74)` 16 px 次文字色（目标温度、位置、设备类别） |
| 开关 | `(170,44) 48×24` 胶囊，开=主色，关=次级容器色 |
| 亮度特性 | 灯卡片 `(14,99) 204×8` 可拖动滑杆（复刻 brightness feature） |
| 只读进度条 | `(14,100) 204×6`（风扇档位、窗帘位置、电量） |

## 交互（对齐 HA tile 默认动作）

| 操作 | HA 行为 | 本实现 |
| --- | --- | --- |
| 点击卡片 | `toggle`：`DOMAINS_TOGGLE` 直接开关，其它域开 more-info | 灯/开关/input_boolean/风扇直接开关，其余选中并在底栏显示详情 |
| 长按卡片 | `hold_action` 默认 more-info | 选中并在底栏显示详情 |
| 点击图标 | `icon_tap_action`（配置后才有，同时出现底色） | 只读卡片：点图标=查看详情并显示底色；可控卡片无图标动作，故无底色 |
| 点击开关 | tile 的 toggle 特性 | 直接开关，不触发选中 |
| 拖动亮度条 | brightness 特性，松手写回 | 拖动实时更新状态行文案，松手下发 `brightness_pct` |

`HASS_UI_ICON_TINT_ALWAYS` 置 1 可让所有激活实体都显示图标底色
（更接近常见的 Mushroom 风格，但不是 HA 原生 tile 的默认行为）。

控制请求的 HTTP 2xx 只表示 Home Assistant 已受理，不代表设备已经执行。原生页会在
8 秒窗口内最多读取 4 次原实体（间隔至少 1 秒），开关状态一致且亮度误差不超过
1 个百分点才更新为成功；超时、离线或读回不一致会显示“控制结果未确认”，不会重发
控制命令。正常状态列表每 10 秒自动同步一次，手动刷新和控制确认共用同一串行请求槽。
未上报 `brightness` 属性的灯只显示开关，不显示会被服务端拒绝的亮度滑杆。
底栏只保留设备控制、刷新和连接设置，不再提供旧 QPK 高级管理入口。完整
`/api/states` 超过 64 KiB 时，原生页会提示在 Home Assistant 中减少公开实体。

## 与 HA 的已知差异

- LVGL 内置符号字体没有灯泡/窗帘/门锁图标，按域就近映射（灯泡→闪电、
  空调→水滴、窗帘→列表、门锁→存储），语义主要由颜色承担。
- 本项目字体只有 16/20/28 三档，因此 HA 的 14/12 px 主次层级放大为
  20/16 px；28 px 用于数值。
- 本项目 CJK 字体只有一个字重，无法复刻 HA 名称行的 `font-weight: 500`。
- 未使用 `lv_arc`，空调卡片用「当前温度 + 目标温度」而不是圆盘温控。
- 状态文字保持主文字色（与 HA 一致）；不可用实体的状态文字用 `disabled`
  而不是主文字色，便于一眼分辨。
- 控制白名单不变，仍然只有 `light`、`switch`、`input_boolean`、`fan`
  可下发；其余域只读展示（HA 的 `DOMAINS_TOGGLE` 还包含 group/automation/
  humidifier/valve，这些在当前服务端会被拒绝，因此按只读处理）。
- 控制确认只证明 HA 的实体状态已经达到目标，不证明设备物理执行；断网或慢设备在
  8 秒窗口内没有收敛时需要用户刷新确认，应用不会自动重复开关设备。
- HA tile 默认无内建进度条；本实现把 brightness 特性作为卡片底部滑杆，
  其余只读域用细进度条代替，属于小屏适配。

## 预览与校验

```sh
python diagnostics/hass_ui_design_preview.py     # 生成浅色/深色设计预览 PNG
python diagnostics/check_hass_ui_syntax.py \
  "04-v3-20260913/espdl-quickapp/overlay/apps/system/desktop/glass_hass_ui.inc"
python diagnostics/audit_hass_ui_layout.py \
  "04-v3-20260913/espdl-quickapp/overlay/apps/system/desktop/glass_hass_ui.inc"
```

预览脚本中的几何数值与 C 代码一一对应，可直接用于比对，生成
`hass-ui-light-preview.png`、`hass-ui-dark-preview.png`、
`hass-ui-light-preview-alt.png`、`hass-ui-light-preview-extended.png` 四张图。
当前状态为源码级改造 + 固件编译通过，尚未烧录、未实板验收。

## LVGL 9.2.1 适配要点

- **基础对象默认可点击**：`lv_obj_create()` 出来的对象在 v9 里自带
  `LV_OBJ_FLAG_CLICKABLE`，卡片里的图标徽章、进度条、分隔线必须显式
  `lv_obj_remove_flag(..., LV_OBJ_FLAG_CLICKABLE)`，否则点上去会被这些装饰
  子对象吃掉、卡片收不到 `LV_EVENT_CLICKED`。最容易漏的是徽章里的图标
  文字（`glass_symbol()` 返回的是 label），已在 `hass_ui_badge()` 里清除。
- **字体只有三档**：`zh_font()` 把尺寸映射到 16 / 20 / 28，所以界面里所有
  文字尺寸都取自这三档，审计脚本会强制检查。
- 控件与开关沿用 v9 API：`lv_button_create`、`lv_obj_remove_style_all`、
  `lv_obj_remove_flag/add_flag`、`lv_slider_*`（`CONFIG_LV_USE_SLIDER=y`）、
  24 px 图标字体（`CONFIG_LV_FONT_MONTSERRAT_24=y`）；没有使用 v8 已移除的
  `lv_btn_create` / `lv_obj_clear_flag` / `lv_style_set_*`。
- 卡片不设阴影（HA 默认 `--ha-card-box-shadow: none`），只用 1 px 描边区分层级。
