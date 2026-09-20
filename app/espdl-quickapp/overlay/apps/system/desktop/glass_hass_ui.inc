/* SPDX-License-Identifier: Apache-2.0 */

#define HASS_UI_MAX_DEVICES 128
#define HASS_UI_VISIBLE 6
#define HASS_UI_REFRESH_INTERVAL_MS 10000
#define HASS_UI_VERIFY_INTERVAL_MS 1000
#define HASS_UI_VERIFY_WINDOW_MS 8000
#define HASS_UI_VERIFY_ATTEMPTS 4

/* Home Assistant design tokens. The palette mirrors
 * frontend/src/resources/theme/color/color.globals.ts of the HA frontend:
 * state colors are resolved as
 *   --state-<domain>-<device_class>-<state>-color
 *   --state-<domain>-<state>-color
 *   --state-<domain>-<active|inactive>-color
 *   --state-<active|inactive>-color
 * and the tile chrome follows ha-card (radius lg = 12 px, 1 px divider
 * border) plus ha-tile-icon (pill badge, 20% state color tint).
 */
#define HASS_HA_RED          0xf44336
#define HASS_HA_PINK         0xe91e63
#define HASS_HA_PURPLE       0x926bc7
#define HASS_HA_DEEP_PURPLE  0x6e41ab
#define HASS_HA_INDIGO       0x3f51b5
#define HASS_HA_BLUE         0x2196f3
#define HASS_HA_LIGHT_BLUE   0x03a9f4
#define HASS_HA_CYAN         0x00bcd4
#define HASS_HA_TEAL         0x009688
#define HASS_HA_GREEN        0x4caf50
#define HASS_HA_LIME         0xcddc39
#define HASS_HA_YELLOW       0xffeb3b
#define HASS_HA_AMBER        0xffc107
#define HASS_HA_ORANGE       0xff9800
#define HASS_HA_DEEP_ORANGE  0xff6f22
#define HASS_HA_BROWN        0x795548
#define HASS_HA_GREY         0x9e9e9e
#define HASS_HA_LIGHT_GREY   0xbdbdbd
#define HASS_HA_BLUE_GREY    0x607d8b
#define HASS_HA_STATE_ICON   0x44739e
#define HASS_HA_TINT_OPA     51   /* ha-tile-icon: state color at 0.2 */

/* ha-tile-icon paints its background only while the icon itself owns an
 * action ("interactive" in the web component). 1 keeps the tinted circle on
 * every active entity instead. */
#define HASS_UI_ICON_TINT_ALWAYS 0

enum hass_ui_mode_e
{
  HASS_UI_ALL = 0,
  HASS_UI_COMMON,
  HASS_UI_ENVIRONMENT
};

enum hass_ui_request_e
{
  HASS_UI_REQUEST_NONE = 0,
  HASS_UI_REQUEST_STATES,
  HASS_UI_REQUEST_CONTROL,
  HASS_UI_REQUEST_VERIFY
};

struct hass_ui_device_s
{
  char entity[96];
  char name[72];
  char state[40];
  char domain[20];
  char unit[20];
  char device_class[24];
  char detail[64];
  bool controllable;
  bool environment;
  bool available;
  bool on;
  bool active;
  bool has_current;
  bool has_target;
  int brightness;
  int position;
  int percentage;
  int current_tenths;
  int target_tenths;
};

static struct
{
  lv_obj_t *root;
  lv_obj_t *list;
  lv_obj_t *heading;
  lv_obj_t *subtitle;
  lv_obj_t *top_status;
  lv_obj_t *count;
  lv_obj_t *nav[3];
  lv_obj_t *hero_state;
  lv_obj_t *hero_detail;
  lv_obj_t *hero_badge;
  lv_obj_t *total_value;
  lv_obj_t *online_value;
  lv_obj_t *running_value;
  lv_obj_t *sync_label;
  lv_obj_t *bottom_name;
  lv_obj_t *bottom_state;
  lv_obj_t *bottom_action;
  lv_obj_t *bottom_action_label;
  lv_obj_t *brightness_group;
  lv_obj_t *brightness_label;
  lv_obj_t *brightness;
  lv_obj_t *bottom_badge;
  lv_obj_t *bottom_icon;
  lv_obj_t *page_prev;
  lv_obj_t *page_next;
  lv_obj_t *card_state[HASS_UI_VISIBLE];
  lv_obj_t *summary_row[5];
  lv_obj_t *summary_count[5];
  lv_timer_t *timer;
  struct hass_ui_device_s devices[HASS_UI_MAX_DEVICES];
  int visible[HASS_UI_VISIBLE];
  uint32_t client;
  uint32_t next_refresh;
  uint32_t verify_at;
  uint32_t verify_deadline;
  unsigned count_all;
  unsigned count_online;
  unsigned count_running;
  unsigned count_light;
  unsigned count_switch;
  unsigned count_fan;
  unsigned count_sensor;
  unsigned count_other;
  unsigned stored;
  unsigned page;
  unsigned pages;
  unsigned filtered;
  unsigned verify_attempts;
  enum hass_ui_mode_e mode;
  enum hass_ui_request_e request;
  int selected;
  int pending_device;
  int pending_brightness;
  bool pending_on;
  char pending_entity[96];
  bool configured;
  bool busy;
  bool loaded;
  bool cached;
  bool truncated;
  char url[128];
  char error[80];
  uint32_t primary;
  uint32_t secondary;
  uint32_t background;
  uint32_t surface;
  uint32_t sidebar;
  uint32_t divider;
  uint32_t variant;
  uint32_t inactive;
  uint32_t disabled;
  uint32_t accent;
  uint32_t warm;
} g_hass_ui;

static void hass_ui_render_devices(void);
static void hass_ui_render_summary(void);
static void hass_ui_refresh_request(bool notify);
static void hass_ui_refresh_clicked(lv_event_t *event);
static void hass_ui_settings(lv_event_t *event);

static void hass_ui_copy(char *out, size_t capacity, const char *text)
{
  if (!capacity) return;
  if (!text) text = "";
  size_t length = strnlen(text, capacity - 1);
  if (text[length] != '\0')
    while (length && (((unsigned char)text[length] & 0xc0) == 0x80)) length--;
  memcpy(out, text, length);
  out[length] = '\0';
}

static lv_obj_t *hass_ui_text(lv_obj_t *parent, const char *text, int x,
                              int y, int width, int size, uint32_t color)
{
  return glass_text(parent, text, x, y, width, size, color);
}

static lv_obj_t *hass_ui_button(lv_obj_t *parent, const char *text, int x,
                                int y, int width, int height, uint32_t color,
                                lv_event_cb_t callback, void *data)
{
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_remove_style_all(button);
  lv_obj_set_pos(button, x, y);
  lv_obj_set_size(button, width, height);
  lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(button, height > 40 ? 12 : height / 2, 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_80, LV_STATE_PRESSED);
  if (callback) glass_clickable(button, callback, data);
  lv_obj_t *label = hass_ui_text(button, text, 0, 0, width - 16, 16,
                                 g_hass_ui.primary);
  if (!strcmp(text, LV_SYMBOL_REFRESH))
    lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(label);
  return button;
}

static bool hass_ui_domain(const char *entity, char *domain, size_t capacity)
{
  const char *dot = entity ? strchr(entity, '.') : NULL;
  size_t length = dot ? (size_t)(dot - entity) : 0;
  if (!length || length >= capacity) return false;
  memcpy(domain, entity, length);
  domain[length] = '\0';
  return true;
}

static bool hass_ui_control_domain(const char *domain)
{
  return !strcmp(domain, "light") || !strcmp(domain, "switch") ||
         !strcmp(domain, "input_boolean") || !strcmp(domain, "fan");
}

static bool hass_ui_environment_domain(const char *domain)
{
  return !strcmp(domain, "sensor") || !strcmp(domain, "binary_sensor") ||
         !strcmp(domain, "climate") || !strcmp(domain, "weather") ||
         !strcmp(domain, "sun") || !strcmp(domain, "humidifier");
}

static bool hass_ui_display_domain(const char *domain)
{
  /* STATE_COLORED_DOMAIN plus the read-only domains the tiles can render. */
  static const char *const extra[] =
    {
      "cover", "lock", "media_player", "vacuum", "humidifier",
      "water_heater", "valve", "lawn_mower", "siren",
      "alarm_control_panel", "camera", "person", "device_tracker", "sun",
      "weather", "update", "plant", "timer", "remote", "scene", "script",
      "button", "automation", "input_number", "number", "select",
      "input_select", "group",
    };
  if (hass_ui_control_domain(domain) || hass_ui_environment_domain(domain))
    return true;
  for (unsigned i = 0; i < sizeof(extra) / sizeof(extra[0]); i++)
    if (!strcmp(domain, extra[i])) return true;
  return false;
}

/* LVGL ships a fixed symbol font, so each Home Assistant domain maps onto the
 * closest built-in glyph; the domain color carries the real meaning. */
static const char *hass_ui_symbol(const struct hass_ui_device_s *device)
{
  if (!strcmp(device->domain, "light")) return LV_SYMBOL_CHARGE;
  if (!strcmp(device->domain, "fan")) return LV_SYMBOL_LOOP;
  if (!strcmp(device->domain, "switch") ||
      !strcmp(device->domain, "input_boolean")) return LV_SYMBOL_POWER;
  if (!strcmp(device->domain, "media_player")) return LV_SYMBOL_AUDIO;
  if (!strcmp(device->domain, "vacuum")) return LV_SYMBOL_REFRESH;
  if (!strcmp(device->domain, "lock")) return LV_SYMBOL_SAVE;
  if (!strcmp(device->domain, "cover")) return LV_SYMBOL_LIST;
  if (!strcmp(device->domain, "climate")) return LV_SYMBOL_TINT;
  if (!strcmp(device->domain, "binary_sensor")) return LV_SYMBOL_BELL;
  if (!strcmp(device->domain, "sensor")) return LV_SYMBOL_EYE_OPEN;
  if (!strcmp(device->domain, "alarm_control_panel")) return LV_SYMBOL_WARNING;
  if (!strcmp(device->domain, "siren")) return LV_SYMBOL_VOLUME_MAX;
  if (!strcmp(device->domain, "camera")) return LV_SYMBOL_VIDEO;
  if (!strcmp(device->domain, "person") ||
      !strcmp(device->domain, "device_tracker")) return LV_SYMBOL_GPS;
  if (!strcmp(device->domain, "sun")) return LV_SYMBOL_EYE_OPEN;
  if (!strcmp(device->domain, "weather")) return LV_SYMBOL_TINT;
  if (!strcmp(device->domain, "humidifier")) return LV_SYMBOL_TINT;
  if (!strcmp(device->domain, "water_heater")) return LV_SYMBOL_CHARGE;
  if (!strcmp(device->domain, "valve")) return LV_SYMBOL_DOWNLOAD;
  if (!strcmp(device->domain, "lawn_mower")) return LV_SYMBOL_REFRESH;
  if (!strcmp(device->domain, "update")) return LV_SYMBOL_UPLOAD;
  if (!strcmp(device->domain, "plant")) return LV_SYMBOL_EYE_OPEN;
  if (!strcmp(device->domain, "timer")) return LV_SYMBOL_BELL;
  if (!strcmp(device->domain, "remote")) return LV_SYMBOL_KEYBOARD;
  if (!strcmp(device->domain, "scene")) return LV_SYMBOL_IMAGE;
  if (!strcmp(device->domain, "script")) return LV_SYMBOL_PLAY;
  if (!strcmp(device->domain, "button")) return LV_SYMBOL_OK;
  if (!strcmp(device->domain, "automation")) return LV_SYMBOL_SETTINGS;
  if (!strcmp(device->domain, "input_number") ||
      !strcmp(device->domain, "number")) return LV_SYMBOL_EDIT;
  if (!strcmp(device->domain, "select") ||
      !strcmp(device->domain, "input_select")) return LV_SYMBOL_LIST;
  if (!strcmp(device->domain, "group")) return LV_SYMBOL_DIRECTORY;
  return LV_SYMBOL_HOME;
}

static bool hass_ui_state_in(const char *state, const char *const *list,
                             unsigned count)
{
  for (unsigned i = 0; i < count; i++)
    if (!strcmp(state, list[i])) return true;
  return false;
}

/* state_active.ts: which states count as "active" for coloring. */
static bool hass_ui_active_state(const struct hass_ui_device_s *device)
{
  static const char *const idle_states[] = {"idle", "docked", "paused"};
  static const char *const live_states[] = {"streaming", "recording"};
  static const char *const group_on[] =
    {"on", "home", "open", "locked", "problem"};

  if (!device->available) return false;
  if (!strcmp(device->domain, "cover")) return strcmp(device->state, "closed");
  if (!strcmp(device->domain, "lock")) return strcmp(device->state, "locked");
  if (!strcmp(device->domain, "media_player"))
    return strcmp(device->state, "off") && strcmp(device->state, "standby");
  if (!strcmp(device->domain, "vacuum"))
    return !hass_ui_state_in(device->state, idle_states, 3);
  if (!strcmp(device->domain, "lawn_mower"))
    return !hass_ui_state_in(device->state, idle_states, 3);
  if (!strcmp(device->domain, "alarm_control_panel"))
    return strcmp(device->state, "disarmed") != 0;
  if (!strcmp(device->domain, "valve")) return strcmp(device->state, "closed");
  if (!strcmp(device->domain, "person") ||
      !strcmp(device->domain, "device_tracker"))
    return strcmp(device->state, "not_home") != 0;
  if (!strcmp(device->domain, "plant"))
    return !strcmp(device->state, "problem");
  if (!strcmp(device->domain, "camera"))
    return hass_ui_state_in(device->state, live_states, 2);
  if (!strcmp(device->domain, "timer"))
    return !strcmp(device->state, "active");
  if (!strcmp(device->domain, "group"))
    return hass_ui_state_in(device->state, group_on, 5);
  if (!strcmp(device->domain, "alert"))
    return strcmp(device->state, "idle") != 0;
  /* Anything but "off" counts as active, so sensors with a numeric state and
   * climate modes keep their state color. */
  return strcmp(device->state, "off") != 0;
}

static bool hass_ui_risk_class(const struct hass_ui_device_s *device)
{
  static const char *const classes[] =
    {"battery", "carbon_monoxide", "gas", "glass_break", "heat", "lock",
     "moisture", "problem", "safety", "smoke", "sound", "tamper"};
  for (unsigned i = 0; i < sizeof(classes) / sizeof(classes[0]); i++)
    if (!strcmp(device->device_class, classes[i])) return true;
  return false;
}

/* --state-<domain>-<state>-color, straight from color.globals.ts. */
struct hass_ui_state_color_s
{
  const char *domain;
  const char *state;
  uint32_t color;
};

static const struct hass_ui_state_color_s g_hass_ui_state_colors[] =
{
  {"climate", "heat", HASS_HA_DEEP_ORANGE},
  {"climate", "cool", HASS_HA_BLUE},
  {"climate", "heat_cool", HASS_HA_AMBER},
  {"climate", "auto", HASS_HA_GREEN},
  {"climate", "dry", HASS_HA_ORANGE},
  {"climate", "fan_only", HASS_HA_CYAN},
  {"lock", "locked", HASS_HA_GREEN},
  {"lock", "unlocked", HASS_HA_RED},
  {"lock", "jammed", HASS_HA_RED},
  {"lock", "open", HASS_HA_RED},
  {"lock", "locking", HASS_HA_ORANGE},
  {"lock", "unlocking", HASS_HA_ORANGE},
  {"lock", "opening", HASS_HA_ORANGE},
  {"alarm_control_panel", "armed_home", HASS_HA_GREEN},
  {"alarm_control_panel", "armed_away", HASS_HA_GREEN},
  {"alarm_control_panel", "armed_night", HASS_HA_GREEN},
  {"alarm_control_panel", "armed_vacation", HASS_HA_GREEN},
  {"alarm_control_panel", "armed_custom_bypass", HASS_HA_GREEN},
  {"alarm_control_panel", "arming", HASS_HA_ORANGE},
  {"alarm_control_panel", "disarming", HASS_HA_ORANGE},
  {"alarm_control_panel", "pending", HASS_HA_ORANGE},
  {"alarm_control_panel", "triggered", HASS_HA_RED},
  {"person", "home", HASS_HA_GREEN},
  {"device_tracker", "home", HASS_HA_GREEN},
  {"sun", "above_horizon", HASS_HA_AMBER},
  {"sun", "below_horizon", HASS_HA_INDIGO},
  {"weather", "sunny", HASS_HA_AMBER},
  {"weather", "clear_night", HASS_HA_DEEP_PURPLE},
  {"weather", "cloudy", HASS_HA_LIGHT_GREY},
  {"weather", "partlycloudy", HASS_HA_BLUE_GREY},
  {"weather", "rainy", HASS_HA_BLUE},
  {"weather", "pouring", HASS_HA_INDIGO},
  {"weather", "snowy", 0xc0e0ff},
  {"weather", "snowy_rainy", HASS_HA_LIGHT_BLUE},
  {"weather", "fog", HASS_HA_GREY},
  {"weather", "hail", HASS_HA_CYAN},
  {"weather", "lightning", HASS_HA_YELLOW},
  {"weather", "lightning_rainy", HASS_HA_LIME},
  {"weather", "windy", HASS_HA_GREEN},
  {"weather", "windy_variant", HASS_HA_GREEN},
  {"weather", "exceptional", HASS_HA_RED},
  {"water_heater", "eco", HASS_HA_GREEN},
  {"water_heater", "electric", HASS_HA_ORANGE},
  {"water_heater", "gas", HASS_HA_ORANGE},
  {"water_heater", "heat_pump", HASS_HA_ORANGE},
  {"water_heater", "high_demand", HASS_HA_DEEP_ORANGE},
  {"water_heater", "performance", HASS_HA_DEEP_ORANGE},
  {"lawn_mower", "error", HASS_HA_RED},
  {"vacuum", "error", HASS_HA_RED},
  {"alert", "off", HASS_HA_ORANGE},
  {"alert", "on", HASS_HA_RED},
};

/* color.globals.ts resolves state colors through four levels of overrides. */
static uint32_t hass_ui_state_color(const struct hass_ui_device_s *device)
{
  const uint32_t inactive = g_hass_ui.inactive;
  const bool active = device->active;

  if (!device->available) return g_hass_ui.disabled;
  for (unsigned i = 0;
       i < sizeof(g_hass_ui_state_colors) / sizeof(g_hass_ui_state_colors[0]);
       i++)
    {
      const struct hass_ui_state_color_s *rule = &g_hass_ui_state_colors[i];
      if (!strcmp(device->domain, rule->domain) &&
          !strcmp(device->state, rule->state)) return rule->color;
    }
  if (!strcmp(device->domain, "sensor") &&
      !strcmp(device->device_class, "battery"))
    {
      char *end = NULL;
      long level = strtol(device->state, &end, 10);
      if (end == device->state) return HASS_HA_STATE_ICON;
      if (level <= 10) return HASS_HA_RED;
      if (level <= 50) return HASS_HA_ORANGE;
      return HASS_HA_GREEN;
    }
  if (!active) return inactive;
  /* --state-<domain>-active-color */
  if (!strcmp(device->domain, "light")) return HASS_HA_AMBER;
  if (!strcmp(device->domain, "switch") ||
      !strcmp(device->domain, "input_boolean")) return HASS_HA_AMBER;
  if (!strcmp(device->domain, "fan")) return HASS_HA_CYAN;
  if (!strcmp(device->domain, "cover")) return HASS_HA_PURPLE;
  if (!strcmp(device->domain, "media_player")) return HASS_HA_LIGHT_BLUE;
  if (!strcmp(device->domain, "vacuum")) return HASS_HA_TEAL;
  if (!strcmp(device->domain, "humidifier")) return HASS_HA_BLUE;
  if (!strcmp(device->domain, "valve")) return HASS_HA_BLUE;
  if (!strcmp(device->domain, "lawn_mower")) return HASS_HA_TEAL;
  if (!strcmp(device->domain, "siren")) return HASS_HA_RED;
  if (!strcmp(device->domain, "plant")) return HASS_HA_RED;
  if (!strcmp(device->domain, "update")) return HASS_HA_ORANGE;
  if (!strcmp(device->domain, "person") ||
      !strcmp(device->domain, "device_tracker")) return HASS_HA_BLUE;
  if (!strcmp(device->domain, "binary_sensor"))
    return hass_ui_risk_class(device) ? HASS_HA_RED : HASS_HA_AMBER;
  if (!strcmp(device->domain, "sensor")) return HASS_HA_STATE_ICON;
  /* --state-active-color */
  return HASS_HA_AMBER;
}

/* stateColorBrightness(): CSS brightness() on the icon color follows the
 * reported light brightness, so a dimmed lamp gets a dimmed icon. */
static uint32_t hass_ui_scale_color(uint32_t color, int percent)
{
  if (percent >= 100) return color;
  if (percent < 20) percent = 20;
  uint32_t red = ((color >> 16) & 0xff) * percent / 100;
  uint32_t green = ((color >> 8) & 0xff) * percent / 100;
  uint32_t blue = (color & 0xff) * percent / 100;
  return (red << 16) | (green << 8) | blue;
}

static uint32_t hass_ui_icon_color(const struct hass_ui_device_s *device)
{
  uint32_t color = hass_ui_state_color(device);
  if (!strcmp(device->domain, "light") && device->on && device->brightness > 0)
    {
      int raw = device->brightness * 255 / 100;
      color = hass_ui_scale_color(color, (raw + 245) / 5);
    }
  return color;
}

static void hass_ui_tenths(char *buffer, size_t capacity, int tenths)
{
  int whole = tenths / 10;
  int fraction = tenths % 10;
  if (fraction < 0) fraction = -fraction;
  if (!whole && tenths < 0) snprintf(buffer, capacity, "-0.%d", fraction);
  else if (fraction) snprintf(buffer, capacity, "%d.%d", whole, fraction);
  else snprintf(buffer, capacity, "%d", whole);
}

/* --state-<domain>-<state> labels, mirroring the HA state translations. */
struct hass_ui_state_text_s
{
  const char *domain;
  const char *state;
  const char *text;
};

static const struct hass_ui_state_text_s g_hass_ui_state_texts[] =
{
  {"alarm_control_panel", "disarmed", "已撤防"},
  {"alarm_control_panel", "armed_home", "在家布防"},
  {"alarm_control_panel", "armed_away", "离家布防"},
  {"alarm_control_panel", "armed_night", "夜间布防"},
  {"alarm_control_panel", "armed_vacation", "度假布防"},
  {"alarm_control_panel", "armed_custom_bypass", "自定义布防"},
  {"alarm_control_panel", "arming", "布防中"},
  {"alarm_control_panel", "disarming", "撤防中"},
  {"alarm_control_panel", "pending", "等待确认"},
  {"alarm_control_panel", "triggered", "已触发"},
  {"humidifier", "on", "加湿中"},
  {"humidifier", "off", "已关闭"},
  {"water_heater", "off", "已关闭"},
  {"water_heater", "eco", "节能"},
  {"water_heater", "electric", "电加热"},
  {"water_heater", "gas", "燃气"},
  {"water_heater", "heat_pump", "热泵"},
  {"water_heater", "high_demand", "高需求"},
  {"water_heater", "performance", "高性能"},
  {"valve", "open", "已打开"},
  {"valve", "closed", "已关闭"},
  {"valve", "opening", "正在打开"},
  {"valve", "closing", "正在关闭"},
  {"lawn_mower", "mowing", "割草中"},
  {"lawn_mower", "docked", "已回充"},
  {"lawn_mower", "paused", "已暂停"},
  {"lawn_mower", "returning", "返回中"},
  {"lawn_mower", "error", "故障"},
  {"siren", "on", "鸣响中"},
  {"siren", "off", "已关闭"},
  {"camera", "streaming", "直播中"},
  {"camera", "recording", "录制中"},
  {"camera", "idle", "待机"},
  {"camera", "off", "已关闭"},
  {"person", "home", "在家"},
  {"person", "not_home", "离家"},
  {"device_tracker", "home", "在家"},
  {"device_tracker", "not_home", "离家"},
  {"sun", "above_horizon", "已日出"},
  {"sun", "below_horizon", "已日落"},
  {"update", "on", "有可用更新"},
  {"update", "off", "已是最新"},
  {"timer", "active", "计时中"},
  {"timer", "idle", "空闲"},
  {"timer", "paused", "已暂停"},
  {"plant", "problem", "需要照顾"},
  {"plant", "ok", "状态良好"},
  {"remote", "on", "已开启"},
  {"remote", "off", "已关闭"},
  {"automation", "on", "已启用"},
  {"automation", "off", "已停用"},
  {"group", "on", "已开启"},
  {"group", "off", "已关闭"},
  {"weather", "sunny", "晴"},
  {"weather", "clear_night", "晴夜"},
  {"weather", "cloudy", "多云"},
  {"weather", "partlycloudy", "局部多云"},
  {"weather", "rainy", "有雨"},
  {"weather", "pouring", "大雨"},
  {"weather", "snowy", "有雪"},
  {"weather", "snowy_rainy", "雨夹雪"},
  {"weather", "fog", "有雾"},
  {"weather", "hail", "冰雹"},
  {"weather", "lightning", "雷电"},
  {"weather", "lightning_rainy", "雷雨"},
  {"weather", "windy", "大风"},
  {"weather", "windy_variant", "阵风"},
  {"weather", "exceptional", "极端天气"},
};

static const char *hass_ui_state_label(const char *domain, const char *state)
{
  for (unsigned i = 0;
       i < sizeof(g_hass_ui_state_texts) / sizeof(g_hass_ui_state_texts[0]);
       i++)
    if (!strcmp(domain, g_hass_ui_state_texts[i].domain) &&
        !strcmp(state, g_hass_ui_state_texts[i].state))
      return g_hass_ui_state_texts[i].text;
  return NULL;
}

static const char *hass_ui_state_text(const struct hass_ui_device_s *device,
                                      char *buffer, size_t capacity)
{
  if (!device->available) return "不可用";
  const char *label = hass_ui_state_label(device->domain, device->state);
  if (label != NULL) return label;
  if (!strcmp(device->domain, "scene") || !strcmp(device->domain, "script") ||
      !strcmp(device->domain, "button"))
    return "可执行";
  if (!strcmp(device->domain, "person") ||
      !strcmp(device->domain, "device_tracker"))
    {
      snprintf(buffer, capacity, "在 %s", device->state);
      return buffer;
    }
  if (!strcmp(device->domain, "cover"))
    {
      if (!strcmp(device->state, "opening")) return "正在打开";
      if (!strcmp(device->state, "closing")) return "正在关闭";
      if (!strcmp(device->state, "closed")) return "已关闭";
      return "已打开";
    }
  if (!strcmp(device->domain, "lock"))
    {
      if (!strcmp(device->state, "locked")) return "已锁定";
      if (!strcmp(device->state, "unlocked")) return "已解锁";
      if (!strcmp(device->state, "jammed")) return "卡住";
      return "正在动作";
    }
  if (!strcmp(device->domain, "media_player"))
    {
      if (!strcmp(device->state, "playing")) return "正在播放";
      if (!strcmp(device->state, "paused")) return "已暂停";
      if (!strcmp(device->state, "idle")) return "空闲";
      if (!strcmp(device->state, "standby")) return "待机";
      return "已关闭";
    }
  if (!strcmp(device->domain, "vacuum"))
    {
      if (!strcmp(device->state, "cleaning")) return "清扫中";
      if (!strcmp(device->state, "returning")) return "返回中";
      if (!strcmp(device->state, "docked")) return "已回充";
      if (!strcmp(device->state, "paused")) return "已暂停";
      return "待机";
    }
  if (!strcmp(device->domain, "binary_sensor"))
    return device->on ? "检测到" : "正常";
  if (device->controllable)
    {
      if (!device->on) return "已关闭";
      if (!strcmp(device->domain, "light") && device->brightness > 0)
        {
          snprintf(buffer, capacity, "已开启 · %d%%", device->brightness);
          return buffer;
        }
      if (!strcmp(device->domain, "fan") && device->percentage > 0)
        {
          snprintf(buffer, capacity, "运行中 · %d%%", device->percentage);
          return buffer;
        }
      return !strcmp(device->domain, "fan") ? "运行中" : "已开启";
    }
  if (device->unit[0])
    {
      snprintf(buffer, capacity, "%s %s", device->state, device->unit);
      return buffer;
    }
  snprintf(buffer, capacity, "%s", device->state);
  return buffer;
}

/* Second line for value tiles (climate, cover, media player). */
static const char *hass_ui_class_text(const char *device_class)
{
  static const struct
  {
    const char *key;
    const char *text;
  } names[] =
    {
      {"temperature", "温度"}, {"humidity", "湿度"}, {"battery", "电池"},
      {"power", "功率"}, {"energy", "电量"}, {"illuminance", "光照"},
      {"pressure", "气压"}, {"carbon_dioxide", "二氧化碳"},
      {"door", "门磁"}, {"window", "窗磁"}, {"opening", "门窗"},
      {"motion", "人体感应"}, {"occupancy", "有人"}, {"presence", "在家"},
      {"moisture", "漏水"}, {"smoke", "烟雾"}, {"gas", "燃气"},
      {"carbon_monoxide", "一氧化碳"}, {"heat", "过热"}, {"tamper", "防拆"},
      {"vibration", "震动"}, {"problem", "异常"}, {"connectivity", "连接"},
      {"plug", "插拔"}, {"lock", "门锁"}, {"safety", "安全"},
      {"sound", "声音"}, {"glass_break", "玻璃破碎"},
    };
  for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    if (!strcmp(device_class, names[i].key)) return names[i].text;
  return device_class;
}

static const char *hass_ui_secondary_text(const struct hass_ui_device_s *device,
                                          char *buffer, size_t capacity)
{
  if (!strcmp(device->domain, "climate"))
    {
      if (device->has_target)
        {
          char target[16];
          hass_ui_tenths(target, sizeof(target), device->target_tenths);
          snprintf(buffer, capacity, "目标 %s°", target);
          return buffer;
        }
      return hass_ui_state_text(device, buffer, capacity);
    }
  if (!strcmp(device->domain, "cover") && device->position >= 0)
    {
      snprintf(buffer, capacity, "位置 %d%%", device->position);
      return buffer;
    }
  if (!strcmp(device->domain, "media_player") && device->detail[0])
    return device->detail;
  if (!strcmp(device->domain, "sensor") && device->device_class[0])
    {
      if (!strcmp(device->device_class, "battery")) return "电池";
      snprintf(buffer, capacity, "%s传感器",
               hass_ui_class_text(device->device_class));
      return buffer;
    }
  if (!strcmp(device->domain, "binary_sensor") && device->device_class[0])
    {
      snprintf(buffer, capacity, "%s",
               hass_ui_class_text(device->device_class));
      return buffer;
    }
  if (!strcmp(device->domain, "weather"))
    return hass_ui_state_text(device, buffer, capacity);
  if (!strcmp(device->domain, "humidifier") && device->percentage >= 0)
    {
      snprintf(buffer, capacity, "目标湿度 %d%%", device->percentage);
      return buffer;
    }
  if (!strcmp(device->domain, "valve") && device->position >= 0)
    {
      snprintf(buffer, capacity, "位置 %d%%", device->position);
      return buffer;
    }
  if ((!strcmp(device->domain, "update") || !strcmp(device->domain, "timer")) &&
      device->detail[0])
    return device->detail;
  return NULL;
}

static bool hass_ui_accept(const struct hass_ui_device_s *device)
{
  if (g_hass_ui.mode == HASS_UI_COMMON) return device->controllable;
  if (g_hass_ui.mode == HASS_UI_ENVIRONMENT) return device->environment;
  return true;
}

static int hass_ui_first_visible(void)
{
  for (unsigned i = 0; i < HASS_UI_VISIBLE; i++)
    if (g_hass_ui.visible[i] >= 0) return g_hass_ui.visible[i];
  return -1;
}

static unsigned hass_ui_filter(int *out, unsigned capacity)
{
  unsigned count = 0;
  for (unsigned pass = 0; pass < (g_hass_ui.mode == HASS_UI_ALL ? 2u : 1u);
       pass++)
    {
      for (unsigned i = 0; i < g_hass_ui.stored && count < capacity; i++)
        {
          const struct hass_ui_device_s *device = &g_hass_ui.devices[i];
          if (!hass_ui_accept(device)) continue;
          if (g_hass_ui.mode == HASS_UI_ALL &&
              device->controllable != (pass == 0)) continue;
          out[count++] = (int)i;
        }
    }
  return count;
}

static void hass_ui_collect_visible(void)
{
  int filtered[HASS_UI_MAX_DEVICES];
  unsigned total = hass_ui_filter(filtered, HASS_UI_MAX_DEVICES);
  unsigned pages = (total + HASS_UI_VISIBLE - 1) / HASS_UI_VISIBLE;
  if (!pages) pages = 1;
  if (g_hass_ui.page >= pages) g_hass_ui.page = pages - 1;
  g_hass_ui.pages = pages;
  g_hass_ui.filtered = total;
  for (unsigned i = 0; i < HASS_UI_VISIBLE; i++) g_hass_ui.visible[i] = -1;
  for (unsigned slot = 0; slot < HASS_UI_VISIBLE; slot++)
    {
      unsigned position = g_hass_ui.page * HASS_UI_VISIBLE + slot;
      if (position < total) g_hass_ui.visible[slot] = filtered[position];
    }
  bool selected_visible = false;
  if (g_hass_ui.selected >= 0)
    for (unsigned i = 0; i < total; i++)
      if (filtered[i] == g_hass_ui.selected) selected_visible = true;
  if (!selected_visible) g_hass_ui.selected = hass_ui_first_visible();
}

static void hass_ui_update_selected(void)
{
  struct hass_ui_device_s *device = NULL;
  if (g_hass_ui.selected >= 0 &&
      (unsigned)g_hass_ui.selected < g_hass_ui.stored)
    device = &g_hass_ui.devices[g_hass_ui.selected];
  char state[80];
  lv_label_set_text(g_hass_ui.bottom_name, device ? device->name : "选择一个设备");
  if (device)
    {
      char line[160];
      snprintf(line, sizeof(line), "%s · %s", device->entity,
               hass_ui_state_text(device, state, sizeof(state)));
      lv_label_set_text(g_hass_ui.bottom_state, line);
    }
  else
    {
      lv_label_set_text(g_hass_ui.bottom_state, "点按设备卡片查看并控制");
    }
  lv_label_set_text(g_hass_ui.bottom_icon,
                    device ? hass_ui_symbol(device) : LV_SYMBOL_HOME);
  lv_obj_set_style_text_color(g_hass_ui.bottom_icon,
    lv_color_hex(device ? hass_ui_icon_color(device) : g_hass_ui.secondary), 0);
  lv_obj_set_style_bg_color(g_hass_ui.bottom_badge,
    lv_color_hex(device ? hass_ui_state_color(device) : g_hass_ui.inactive), 0);
  lv_obj_set_style_bg_opa(g_hass_ui.bottom_badge,
    device && device->available ? HASS_HA_TINT_OPA : LV_OPA_TRANSP, 0);
  bool controllable = device && device->controllable && device->available;
  lv_label_set_text(g_hass_ui.bottom_action_label,
                    controllable ? (device->on ? "关闭" : "开启") : "不可控制");
  lv_obj_set_style_bg_color(g_hass_ui.bottom_action,
                            lv_color_hex(controllable ? g_hass_ui.accent :
                                         g_hass_ui.variant), 0);
  lv_obj_set_style_text_color(g_hass_ui.bottom_action_label,
                              lv_color_hex(controllable ? 0xffffff :
                                           g_hass_ui.secondary), 0);
  if (controllable) lv_obj_remove_state(g_hass_ui.bottom_action, LV_STATE_DISABLED);
  else lv_obj_add_state(g_hass_ui.bottom_action, LV_STATE_DISABLED);
  bool dimmable = device && device->available && device->brightness >= 0 &&
                  !strcmp(device->domain, "light");
  if (dimmable)
    {
      lv_obj_remove_flag(g_hass_ui.brightness_group, LV_OBJ_FLAG_HIDDEN);
      int value = device->brightness > 0 ? device->brightness : 100;
      lv_slider_set_value(g_hass_ui.brightness, value, LV_ANIM_OFF);
      lv_label_set_text_fmt(g_hass_ui.brightness_label, "亮度 %d%%", value);
    }
  else
    {
      lv_obj_add_flag(g_hass_ui.brightness_group, LV_OBJ_FLAG_HIDDEN);
    }
}

static void hass_ui_device_select(lv_event_t *event)
{
  int index = (int)(uintptr_t)lv_event_get_user_data(event) - 1;
  if (index < 0 || (unsigned)index >= g_hass_ui.stored) return;
  g_hass_ui.selected = index;
  hass_ui_render_devices();
}

static void hass_ui_start_control(int index, bool on, int brightness)
{
  if (!g_hass_ui.client || index < 0 ||
      (unsigned)index >= g_hass_ui.stored) return;
  struct hass_ui_device_s *device = &g_hass_ui.devices[index];
  if (!device->controllable || !device->available) return;
  if (g_hass_ui.request != HASS_UI_REQUEST_NONE || g_hass_ui.busy ||
      g_hass_ui.pending_entity[0])
    {
      show_toast("家庭服务正在同步，请稍候");
      return;
    }
  int ret = hass_control(g_hass_ui.client, device->entity, on, brightness);
  if (ret < 0)
    {
      show_toast(ret == -EBUSY ? "家庭服务正忙，请稍候" :
                 ret == -ENOTCONN ? "请先连接 Home Assistant" :
                 "设备控制未发送，请重试");
      return;
    }
  g_hass_ui.request = HASS_UI_REQUEST_CONTROL;
  g_hass_ui.pending_device = index;
  g_hass_ui.pending_on = on;
  g_hass_ui.pending_brightness = brightness;
  hass_ui_copy(g_hass_ui.pending_entity, sizeof(g_hass_ui.pending_entity),
               device->entity);
  g_hass_ui.verify_attempts = 0;
  g_hass_ui.verify_at = 0;
  g_hass_ui.verify_deadline = 0;
  g_hass_ui.next_refresh = 0;
  g_hass_ui.busy = true;
  lv_label_set_text(g_hass_ui.top_status, "正在执行");
  hass_ui_render_summary();
}

static void hass_ui_device_toggle(lv_event_t *event)
{
  lv_event_stop_bubbling(event);
  int index = (int)(uintptr_t)lv_event_get_user_data(event) - 1;
  if (index < 0 || (unsigned)index >= g_hass_ui.stored) return;
  g_hass_ui.selected = index;
  hass_ui_start_control(index, !g_hass_ui.devices[index].on, -1);
}

static void hass_ui_bottom_toggle(lv_event_t *event)
{
  LV_UNUSED(event);
  int index = g_hass_ui.selected;
  if (index < 0 || (unsigned)index >= g_hass_ui.stored) return;
  hass_ui_start_control(index, !g_hass_ui.devices[index].on, -1);
}

static void hass_ui_brightness_changed(lv_event_t *event)
{
  int value = lv_slider_get_value(lv_event_get_target(event));
  lv_label_set_text_fmt(g_hass_ui.brightness_label, "亮度 %d%%", value);
}

static void hass_ui_brightness_commit(lv_event_t *event)
{
  int value = lv_slider_get_value(lv_event_get_target(event));
  hass_ui_start_control(g_hass_ui.selected, true, value);
}

/* HA's default tile tap action is `toggle`: DOMAINS_TOGGLE entities switch,
 * everything else opens more-info. The native service whitelist covers
 * light/switch/input_boolean/fan, so those toggle and the rest select here. */
static void hass_ui_card_tap(lv_event_t *event)
{
  int index = (int)(uintptr_t)lv_event_get_user_data(event) - 1;
  if (index < 0 || (unsigned)index >= g_hass_ui.stored) return;
  struct hass_ui_device_s *device = &g_hass_ui.devices[index];
  if (!hass_ui_control_domain(device->domain) || !device->available)
    {
      hass_ui_device_select(event);
      return;
    }
  g_hass_ui.selected = index;
  hass_ui_start_control(index, !device->on, -1);
}

/* hold_action defaults to more-info in HA; the detail bar is our more-info. */
static void hass_ui_card_hold(lv_event_t *event)
{
  hass_ui_device_select(event);
}

/* Brightness card feature: drag to set, service call on release, exactly like
 * the light brightness feature row in a HA tile. */
static void hass_ui_feature_changed(lv_event_t *event)
{
  int slot = (int)(uintptr_t)lv_event_get_user_data(event) - 1;
  if (slot < 0 || slot >= HASS_UI_VISIBLE) return;
  int index = g_hass_ui.visible[slot];
  if (index < 0 || (unsigned)index >= g_hass_ui.stored) return;
  int value = lv_slider_get_value(lv_event_get_target(event));
  if (g_hass_ui.card_state[slot])
    lv_label_set_text_fmt(g_hass_ui.card_state[slot], "已开启 · %d%%", value);
}

static void hass_ui_feature_commit(lv_event_t *event)
{
  int slot = (int)(uintptr_t)lv_event_get_user_data(event) - 1;
  if (slot < 0 || slot >= HASS_UI_VISIBLE) return;
  int index = g_hass_ui.visible[slot];
  if (index < 0 || (unsigned)index >= g_hass_ui.stored) return;
  int value = lv_slider_get_value(lv_event_get_target(event));
  if (value < 1) value = 1;
  g_hass_ui.selected = index;
  hass_ui_start_control(index, true, value);
}

/* ha-switch look: pill track, on-state uses the theme primary color. */
static void hass_ui_toggle(lv_obj_t *parent, const struct hass_ui_device_s *device,
                           int index, int x, int y)
{
  uint32_t track_color = device->on ? g_hass_ui.accent : g_hass_ui.variant;
  lv_obj_t *track = glass_box(parent, x, y, 48, 24, track_color, 12);
  lv_obj_set_style_bg_opa(track, device->available ? LV_OPA_COVER : LV_OPA_40, 0);
  lv_obj_t *knob = glass_box(track, device->on ? 26 : 4, 3, 18, 18,
                             0xffffff, 9);
  lv_obj_remove_flag(knob, LV_OBJ_FLAG_CLICKABLE);
  if (device->available)
    {
      glass_clickable(track, hass_ui_device_toggle,
                      (void *)(uintptr_t)(index + 1));
      lv_obj_remove_flag(track, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
}

/* ha-tile-icon: 36 px pill that carries the state color on the glyph. The
 * background tint is part of the component only while the icon is
 * interactive, so it follows the same rule here. */
static void hass_ui_badge(lv_obj_t *card,
                          const struct hass_ui_device_s *device, int index,
                          bool interactive)
{
  uint32_t state_color = hass_ui_state_color(device);
  bool tinted = device->available && state_color != g_hass_ui.inactive &&
                (HASS_UI_ICON_TINT_ALWAYS || interactive);
  lv_obj_t *badge = glass_box(card, 14, 14, 36, 36, state_color, 18);
  lv_obj_set_style_bg_opa(badge, tinted ? HASS_HA_TINT_OPA : LV_OPA_TRANSP, 0);
  lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *icon = glass_symbol(badge, hass_ui_symbol(device), 0, 0,
                                hass_ui_icon_color(device));
  lv_obj_center(icon);
  if (interactive && device->available)
    {
      /* Icon tap action: mirror HA's icon_tap_action -> more-info, which the
       * detail bar at the bottom plays here. */
      glass_clickable(icon, hass_ui_device_select,
                      (void *)(uintptr_t)(index + 1));
      lv_obj_remove_flag(icon, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
  else
    {
      /* LVGL 9 marks every created object clickable; a dead glyph would
       * otherwise swallow the tile tap. */
      lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    }
}

/* ha-tile-info: both the primary (name) and secondary (state) lines use
 * --primary-text-color; the state color lives on the icon and the feature. */
static uint32_t hass_ui_line_color(const struct hass_ui_device_s *device)
{
  if (!device->available) return g_hass_ui.disabled;
  return g_hass_ui.primary;
}

static void hass_ui_bar(lv_obj_t *card, uint32_t color, int level)
{
  lv_obj_t *track = glass_box(card, 14, 100, 204, 6, g_hass_ui.variant, 3);
  lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);
  if (level <= 0) return;
  int width = 204 * level / 100;
  if (width < 6) width = 6;
  lv_obj_t *fill = glass_box(track, 0, 0, width, 6, color, 3);
  lv_obj_remove_flag(fill, LV_OBJ_FLAG_CLICKABLE);
}

/* Light brightness feature: a draggable pill track with a round knob, matching
 * the brightness feature of a HA tile card. */
static void hass_ui_feature(lv_obj_t *card,
                            const struct hass_ui_device_s *device, int slot)
{
  uint32_t state_color = hass_ui_state_color(device);
  int level = device->on ? (device->brightness > 0 ? device->brightness : 100)
                         : 0;
  lv_obj_t *slider = lv_slider_create(card);
  lv_obj_set_pos(slider, 14, 99);
  lv_obj_set_size(slider, 204, 8);
  lv_slider_set_range(slider, 0, 100);
  lv_slider_set_value(slider, level, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(slider, lv_color_hex(g_hass_ui.variant),
                            LV_PART_MAIN);
  lv_obj_set_style_radius(slider, 4, LV_PART_MAIN);
  lv_obj_set_style_bg_color(slider, lv_color_hex(state_color),
                            LV_PART_INDICATOR);
  lv_obj_set_style_radius(slider, 4, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(slider, lv_color_hex(0xffffff), LV_PART_KNOB);
  lv_obj_set_style_border_width(slider, 1, LV_PART_KNOB);
  lv_obj_set_style_border_color(slider, lv_color_hex(state_color),
                                LV_PART_KNOB);
  lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
  lv_obj_set_style_pad_all(slider, 6, LV_PART_KNOB);
  lv_obj_add_event_cb(slider, hass_ui_feature_changed, LV_EVENT_VALUE_CHANGED,
                      (void *)(uintptr_t)(slot + 1));
  lv_obj_add_event_cb(slider, hass_ui_feature_commit, LV_EVENT_RELEASED,
                      (void *)(uintptr_t)(slot + 1));
}

/* Tiles whose main line is a reading rather than a status word. */
static bool hass_ui_value_tile(const char *domain)
{
  return !strcmp(domain, "sensor") || !strcmp(domain, "climate") ||
         !strcmp(domain, "weather") || !strcmp(domain, "water_heater") ||
         !strcmp(domain, "input_number") || !strcmp(domain, "number");
}

/* Tiles whose main line is a status word shown one size larger. */
static bool hass_ui_status_tile(const char *domain)
{
  static const char *const list[] =
    {
      "binary_sensor", "lock", "media_player", "person", "device_tracker",
      "alarm_control_panel", "update", "timer", "scene", "script", "button",
      "sun", "plant", "camera", "remote", "automation", "siren",
      "lawn_mower", "group", "humidifier", "valve", "select", "input_select",
    };
  for (unsigned i = 0; i < sizeof(list) / sizeof(list[0]); i++)
    if (!strcmp(domain, list[i])) return true;
  return false;
}

static void hass_ui_card_body(lv_obj_t *card,
                              const struct hass_ui_device_s *device, int index,
                              int slot)
{
  const char *domain = device->domain;
  char state[80];
  char value[40];
  char secondary[80];
  const char *line = hass_ui_state_text(device, state, sizeof(state));
  const char *sub = hass_ui_secondary_text(device, secondary, sizeof(secondary));
  uint32_t line_color = hass_ui_line_color(device);
  uint32_t state_color = hass_ui_state_color(device);

  g_hass_ui.card_state[slot] = NULL;
  /* The icon owns an action whenever the tile itself is read-only, which is
   * what makes ha-tile-icon show its background in HA. */
  bool icon_interactive = !hass_ui_control_domain(domain);
  hass_ui_badge(card, device, index, icon_interactive);
  hass_ui_text(card, device->name, 60, 10, 158, 20, g_hass_ui.primary);

  if (hass_ui_value_tile(domain))
    {
      if (device->has_current)
        {
          char current[16];
          hass_ui_tenths(current, sizeof(current), device->current_tenths);
          snprintf(value, sizeof(value), "%s°", current);
        }
      else
        {
          snprintf(value, sizeof(value), "%s", line);
        }
      /* 28 px reading first, secondary line below it; both end above the
       * 100 px progress bar used by the control tiles. */
      g_hass_ui.card_state[slot] = hass_ui_text(card, value, 60, 38, 158, 28,
                                                line_color);
      if (sub) hass_ui_text(card, sub, 60, 82, 158, 16, g_hass_ui.secondary);
      return;
    }

  if (hass_ui_status_tile(domain))
    {
      g_hass_ui.card_state[slot] =
        hass_ui_text(card, (!strcmp(domain, "media_player") &&
                            device->detail[0]) ? device->detail : line,
                     60, 42, 158, 20, line_color);
      if (sub) hass_ui_text(card, sub, 60, 74, 158, 16, g_hass_ui.secondary);
      return;
    }

  if (device->controllable)
    {
      g_hass_ui.card_state[slot] = hass_ui_text(card, line, 60, 44, 98, 16,
                                                line_color);
      hass_ui_toggle(card, device, index, 170, 44);
    }
  else
    {
      g_hass_ui.card_state[slot] = hass_ui_text(card, line, 60, 44, 158, 16,
                                                line_color);
    }

  if (sub) hass_ui_text(card, sub, 60, 74, 158, 16, g_hass_ui.secondary);

  if (!strcmp(domain, "light") && device->available &&
      device->brightness >= 0)
    hass_ui_feature(card, device, slot);
  else if (!strcmp(domain, "fan") && device->on && device->percentage > 0)
    hass_ui_bar(card, state_color, device->percentage);
  else if (!strcmp(domain, "cover") && device->position >= 0)
    hass_ui_bar(card, state_color, device->position);
  else if (!strcmp(domain, "vacuum") && device->percentage >= 0)
    hass_ui_bar(card, state_color, device->percentage);
}

static void hass_ui_render_empty(void)
{
  const bool unconfigured = !g_hass_ui.configured;
  const bool waiting = g_hass_ui.configured && !g_hass_ui.loaded &&
                       !g_hass_ui.error[0];
  lv_obj_t *panel = glass_box(g_hass_ui.list, 0, 36, 488, 292,
                              g_hass_ui.surface, 12);
  lv_obj_set_style_border_width(panel, 1, 0);
  lv_obj_set_style_border_color(panel, lv_color_hex(g_hass_ui.divider), 0);
  lv_obj_set_style_border_opa(panel, LV_OPA_COVER, 0);
  lv_obj_t *mark = glass_box(panel, 24, 24, 64, 64,
                             unconfigured ? g_hass_ui.warm : g_hass_ui.accent,
                             18);
  lv_obj_remove_flag(mark, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *icon = glass_symbol(mark, unconfigured ? LV_SYMBOL_WIFI :
                               LV_SYMBOL_REFRESH, 0, 0, 0xffffff);
  lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_center(icon);
  hass_ui_text(panel, unconfigured ? "连接你的家" : waiting ?
               "正在同步家庭设备" :
               g_hass_ui.error[0] ? "暂时没有同步成功" : "还没有可显示的设备",
               108, 25, 350, 28, g_hass_ui.primary);
  hass_ui_text(panel, unconfigured ?
               "用手机扫码填写 HTTP 地址与长期访问令牌，设备会自动出现在这里。" :
               waiting ? "正在从家庭中枢读取状态，设备卡片稍后会出现在这里。" :
               g_hass_ui.error[0] ? g_hass_ui.error :
               "支持灯、开关、风扇、传感器、空调等 Home Assistant 实体。",
               108, 72, 344, 16, g_hass_ui.secondary);
  if (unconfigured)
    {
      hass_ui_text(panel, "01  同一 Wi-Fi", 28, 134, 132, 16,
                   g_hass_ui.secondary);
      hass_ui_text(panel, "02  扫码配置", 174, 134, 132, 16,
                   g_hass_ui.secondary);
      hass_ui_text(panel, "03  自动同步", 320, 134, 132, 16,
                   g_hass_ui.secondary);
      lv_obj_t *button = hass_ui_button(panel, "打开连接设置", 28, 202, 194, 48,
                                        g_hass_ui.accent, hass_ui_settings, NULL);
      lv_obj_set_style_text_color(lv_obj_get_child(button, 0),
                                  lv_color_hex(0xffffff), 0);
    }
  else if (!waiting)
    {
      lv_obj_t *button = hass_ui_button(panel, "重新同步", 28, 202, 154, 48,
                                        g_hass_ui.accent,
                                        hass_ui_refresh_clicked, NULL);
      lv_obj_set_style_text_color(lv_obj_get_child(button, 0),
                                  lv_color_hex(0xffffff), 0);
    }
}

static void hass_ui_render_devices(void)
{
  if (!g_hass_ui.list) return;
  lv_obj_clean(g_hass_ui.list);
  hass_ui_collect_visible();
  for (unsigned i = 0; i < 3; i++)
    {
      bool selected = (unsigned)g_hass_ui.mode == i;
      lv_obj_set_style_bg_color(g_hass_ui.nav[i],
                                lv_color_hex(g_hass_ui.accent), 0);
      lv_obj_set_style_bg_opa(g_hass_ui.nav[i],
                              selected ? 38 : LV_OPA_TRANSP, 0);
      lv_obj_set_style_text_color(lv_obj_get_child(g_hass_ui.nav[i], 0),
                                  lv_color_hex(selected ? g_hass_ui.accent :
                                               g_hass_ui.secondary), 0);
    }
  const char *headings[] = {"全屋设备", "常用控制", "环境感知"};
  const char *subtitles[] = {"设备状态，一眼看清", "灯光、开关与风扇", "温度、湿度与传感器"};
  lv_label_set_text(g_hass_ui.heading, headings[g_hass_ui.mode]);
  if (g_hass_ui.filtered > HASS_UI_VISIBLE)
    {
      char page_text[96];
      snprintf(page_text, sizeof(page_text), "%s · %u/%u 页",
               subtitles[g_hass_ui.mode], g_hass_ui.page + 1, g_hass_ui.pages);
      lv_label_set_text(g_hass_ui.subtitle, page_text);
    }
  else
    {
      lv_label_set_text(g_hass_ui.subtitle, subtitles[g_hass_ui.mode]);
    }
  glass_pager_enabled(g_hass_ui.page_prev, g_hass_ui.page > 0,
                      g_hass_ui.primary);
  glass_pager_enabled(g_hass_ui.page_next,
                      g_hass_ui.page + 1 < g_hass_ui.pages, g_hass_ui.primary);
  if (!g_hass_ui.loaded || hass_ui_first_visible() < 0)
    {
      hass_ui_render_empty();
      hass_ui_update_selected();
      return;
    }
  for (unsigned slot = 0; slot < HASS_UI_VISIBLE; slot++)
    {
      int index = g_hass_ui.visible[slot];
      if (index < 0) continue;
      struct hass_ui_device_s *device = &g_hass_ui.devices[index];
      int x = (slot % 2) * 244;
      int y = (slot / 2) * 128;
      bool selected = index == g_hass_ui.selected;
      /* ha-card: --ha-card-border-radius (lg = 12) with a 1 px divider. */
      lv_obj_t *card = glass_box(g_hass_ui.list, x, y, 232, 118,
                                 selected ? (g_desktop.light_theme ?
                                             0xeaf6f1 : 0x17302a) :
                                            g_hass_ui.surface, 12);
      lv_obj_set_style_border_width(card, selected ? 2 : 1, 0);
      lv_obj_set_style_border_color(card,
        lv_color_hex(selected ? g_hass_ui.accent : g_hass_ui.divider), 0);
      lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
      /* HA tile: tap = toggle (DOMAINS_TOGGLE) or more-info, hold = more-info,
       * icon tap = more-info. */
      glass_clickable(card, hass_ui_card_tap, (void *)(uintptr_t)(index + 1));
      lv_obj_add_event_cb(card, hass_ui_card_hold, LV_EVENT_LONG_PRESSED,
                          (void *)(uintptr_t)(index + 1));
      hass_ui_card_body(card, device, index, (int)slot);
    }
  hass_ui_update_selected();
}

static void hass_ui_render_summary(void)
{
  if (!g_hass_ui.root) return;
  const char *status = !g_hass_ui.configured ? "等待连接" :
                       g_hass_ui.busy ? "正在同步" :
                       g_hass_ui.error[0] ? "连接异常" :
                       g_hass_ui.loaded ? "家庭在线" : "准备连接";
  const char *chip = !g_hass_ui.configured ? "未连接" :
                     g_hass_ui.error[0] ? "需检查" :
                     g_hass_ui.busy ? "同步中" :
                     g_hass_ui.loaded ? "家庭在线" : "准备连接";
  uint32_t chip_color = g_hass_ui.error[0] ? g_hass_ui.warm :
                        (g_hass_ui.configured && g_hass_ui.loaded) ?
                          g_hass_ui.accent : g_hass_ui.secondary;
  lv_label_set_text(g_hass_ui.hero_badge, chip);
  lv_obj_set_style_text_color(g_hass_ui.hero_badge,
                              lv_color_hex(chip_color), 0);
  lv_obj_t *chip_box = lv_obj_get_parent(g_hass_ui.hero_badge);
  if (chip_box)
    {
      lv_obj_set_style_bg_color(chip_box, lv_color_hex(chip_color), 0);
      lv_obj_set_style_bg_opa(chip_box, 40, 0);
    }
  lv_label_set_text(g_hass_ui.hero_state, status);
  lv_label_set_text(g_hass_ui.hero_detail,
                    !g_hass_ui.configured ? "尚未配置 Home Assistant" :
                    g_hass_ui.url[0] ? g_hass_ui.url : "等待配置地址");
  lv_label_set_text_fmt(g_hass_ui.total_value, "%u", g_hass_ui.count_all);
  lv_label_set_text_fmt(g_hass_ui.online_value, "%u", g_hass_ui.count_online);
  lv_label_set_text_fmt(g_hass_ui.running_value, "%u", g_hass_ui.count_running);
  if (g_hass_ui.truncated && !g_hass_ui.busy && !g_hass_ui.error[0])
    lv_label_set_text_fmt(g_hass_ui.sync_label, "实体较多，仅显示前 %u 个",
                          HASS_UI_MAX_DEVICES);
  else
    lv_label_set_text(g_hass_ui.sync_label,
                      !g_hass_ui.configured ? "扫码连接后自动同步" :
                      g_hass_ui.busy ? "正在读取最新状态" :
                      g_hass_ui.error[0] ? g_hass_ui.error :
                      g_hass_ui.cached ? "已读取本地状态缓存" :
                      g_hass_ui.loaded ? "状态刚刚更新" : "等待首次同步");
  lv_label_set_text_fmt(g_hass_ui.count, "%u 台设备 · %u 在线",
                        g_hass_ui.count_all, g_hass_ui.count_online);
  /* Only domains that actually exist are listed, like the HA overview. */
  const unsigned per_domain[5] =
    {g_hass_ui.count_light, g_hass_ui.count_switch, g_hass_ui.count_fan,
     g_hass_ui.count_sensor, g_hass_ui.count_other};
  int row = 0;
  for (unsigned i = 0; i < 5; i++)
    {
      lv_label_set_text_fmt(g_hass_ui.summary_count[i], "%u", per_domain[i]);
      if (per_domain[i])
        {
          lv_obj_set_pos(g_hass_ui.summary_row[i], 22, 262 + row * 32);
          lv_obj_remove_flag(g_hass_ui.summary_row[i], LV_OBJ_FLAG_HIDDEN);
          row++;
        }
      else
        {
          lv_obj_add_flag(g_hass_ui.summary_row[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
  if (!g_hass_ui.busy)
    lv_label_set_text(g_hass_ui.top_status,
                      g_hass_ui.error[0] ? "需要检查" :
                      g_hass_ui.loaded ? "已同步" :
                      g_hass_ui.configured ? "待同步" : "未连接");
}

static void hass_ui_set_error(int status, int error)
{
  if (status == 401 || status == 403)
    hass_ui_copy(g_hass_ui.error, sizeof(g_hass_ui.error),
                 "访问令牌已失效，请重新配置");
  else if (error == -EOVERFLOW || error == -E2BIG)
    hass_ui_copy(g_hass_ui.error, sizeof(g_hass_ui.error),
                 "状态超过 64 KiB，请在 HA 中减少公开实体");
  else if (status && (status < 200 || status >= 300))
    snprintf(g_hass_ui.error, sizeof(g_hass_ui.error),
             "Home Assistant 返回状态 %d", status);
  else if (error)
    hass_ui_copy(g_hass_ui.error, sizeof(g_hass_ui.error),
                 "无法连接家庭中枢，请检查网络");
  else
    g_hass_ui.error[0] = '\0';
}

static bool hass_ui_parse(const char *body)
{
  cJSON *root = cJSON_ParseWithOpts(body, NULL, true);
  if (!cJSON_IsArray(root))
    {
      cJSON_Delete(root);
      hass_ui_copy(g_hass_ui.error, sizeof(g_hass_ui.error),
                   "设备数据格式异常，请重新同步");
      return false;
    }
  char selected_entity[96] = "";
  if (g_hass_ui.selected >= 0 &&
      (unsigned)g_hass_ui.selected < g_hass_ui.stored)
    hass_ui_copy(selected_entity, sizeof(selected_entity),
                 g_hass_ui.devices[g_hass_ui.selected].entity);
  g_hass_ui.stored = 0;
  g_hass_ui.count_all = 0;
  g_hass_ui.count_online = 0;
  g_hass_ui.count_running = 0;
  g_hass_ui.count_light = 0;
  g_hass_ui.count_switch = 0;
  g_hass_ui.count_fan = 0;
  g_hass_ui.count_sensor = 0;
  g_hass_ui.count_other = 0;
  g_hass_ui.truncated = false;
  g_hass_ui.selected = -1;
  cJSON *item;
  cJSON_ArrayForEach(item, root)
    {
      cJSON *entity = cJSON_GetObjectItemCaseSensitive(item, "entity_id");
      cJSON *state = cJSON_GetObjectItemCaseSensitive(item, "state");
      cJSON *attributes = cJSON_GetObjectItemCaseSensitive(item, "attributes");
      if (!cJSON_IsString(entity) || !cJSON_IsString(state) ||
          !cJSON_IsObject(attributes)) continue;
      char domain[20];
      if (!hass_ui_domain(entity->valuestring, domain, sizeof(domain)) ||
          !hass_ui_display_domain(domain)) continue;
      g_hass_ui.count_all++;
      bool available = strcmp(state->valuestring, "unavailable") &&
                       strcmp(state->valuestring, "unknown");
      bool controllable = hass_ui_control_domain(domain);
      bool on = !strcmp(state->valuestring, "on") ||
                !strcmp(state->valuestring, "open") ||
                !strcmp(state->valuestring, "playing") ||
                !strcmp(state->valuestring, "heat") ||
                !strcmp(state->valuestring, "cool");
      if (available) g_hass_ui.count_online++;
      if (available && controllable && on) g_hass_ui.count_running++;
      if (g_hass_ui.stored >= HASS_UI_MAX_DEVICES)
        {
          g_hass_ui.truncated = true;
          continue;
        }
      struct hass_ui_device_s *device =
        &g_hass_ui.devices[g_hass_ui.stored];
      memset(device, 0, sizeof(*device));
      hass_ui_copy(device->entity, sizeof(device->entity), entity->valuestring);
      hass_ui_copy(device->domain, sizeof(device->domain), domain);
      hass_ui_copy(device->state, sizeof(device->state), state->valuestring);
      cJSON *friendly = cJSON_GetObjectItemCaseSensitive(attributes,
                                                          "friendly_name");
      if (cJSON_IsString(friendly) && friendly->valuestring[0])
        hass_ui_copy(device->name, sizeof(device->name), friendly->valuestring);
      else
        {
          const char *fallback = strchr(entity->valuestring, '.');
          hass_ui_copy(device->name, sizeof(device->name),
                       fallback ? fallback + 1 : entity->valuestring);
          for (char *p = device->name; *p; p++) if (*p == '_') *p = ' ';
        }
      cJSON *unit = cJSON_GetObjectItemCaseSensitive(attributes,
                                                      "unit_of_measurement");
      if (cJSON_IsString(unit))
        hass_ui_copy(device->unit, sizeof(device->unit), unit->valuestring);
      cJSON *brightness = cJSON_GetObjectItemCaseSensitive(attributes,
                                                            "brightness");
      device->brightness = cJSON_IsNumber(brightness) ?
        (brightness->valueint * 100 + 127) / 255 : -1;
      device->position = -1;
      device->percentage = -1;
      cJSON *device_class = cJSON_GetObjectItemCaseSensitive(attributes,
                                                              "device_class");
      if (cJSON_IsString(device_class))
        hass_ui_copy(device->device_class, sizeof(device->device_class),
                     device_class->valuestring);
      cJSON *position = cJSON_GetObjectItemCaseSensitive(attributes,
                                                          "current_position");
      if (cJSON_IsNumber(position))
        {
          int level = (int)(position->valuedouble + 0.5);
          device->position = level < 0 ? 0 : level > 100 ? 100 : level;
        }
      cJSON *percentage = cJSON_GetObjectItemCaseSensitive(attributes,
                                                            "percentage");
      if (cJSON_IsNumber(percentage))
        {
          int level = (int)(percentage->valuedouble + 0.5);
          device->percentage = level < 0 ? 0 : level > 100 ? 100 : level;
        }
      cJSON *current = cJSON_GetObjectItemCaseSensitive(attributes,
                                                         "current_temperature");
      cJSON *target = cJSON_GetObjectItemCaseSensitive(attributes,
                                                        "temperature");
      if (cJSON_IsNumber(current))
        {
          device->has_current = true;
          device->current_tenths = (int)(current->valuedouble * 10 +
                                         (current->valuedouble < 0 ? -0.5 :
                                                                      0.5));
        }
      if (cJSON_IsNumber(target))
        {
          device->has_target = true;
          device->target_tenths = (int)(target->valuedouble * 10 +
                                        (target->valuedouble < 0 ? -0.5 : 0.5));
        }
      cJSON *title = cJSON_GetObjectItemCaseSensitive(attributes,
                                                       "media_title");
      cJSON *artist = cJSON_GetObjectItemCaseSensitive(attributes,
                                                        "media_artist");
      if (cJSON_IsString(title) && title->valuestring[0])
        hass_ui_copy(device->detail, sizeof(device->detail),
                     title->valuestring);
      else if (cJSON_IsString(artist) && artist->valuestring[0])
        hass_ui_copy(device->detail, sizeof(device->detail),
                     artist->valuestring);
      if (!strcmp(domain, "weather") && cJSON_IsNumber(target))
        {
          /* The weather tile shows the current temperature as its reading. */
          device->has_current = true;
          device->current_tenths = (int)(target->valuedouble * 10 +
                                         (target->valuedouble < 0 ? -0.5 :
                                                                      0.5));
          device->has_target = false;
        }
      cJSON *humidity_target = cJSON_GetObjectItemCaseSensitive(attributes,
                                                                "target_humidity");
      if (cJSON_IsNumber(humidity_target))
        {
          int level = (int)(humidity_target->valuedouble + 0.5);
          device->percentage = level < 0 ? 0 : level > 100 ? 100 : level;
        }
      cJSON *installed = cJSON_GetObjectItemCaseSensitive(attributes,
                                                          "installed_version");
      if (cJSON_IsString(installed) && installed->valuestring[0] &&
          !device->detail[0])
        hass_ui_copy(device->detail, sizeof(device->detail),
                     installed->valuestring);
      cJSON *remaining = cJSON_GetObjectItemCaseSensitive(attributes,
                                                          "remaining");
      if (cJSON_IsString(remaining) && remaining->valuestring[0] &&
          !device->detail[0])
        hass_ui_copy(device->detail, sizeof(device->detail),
                     remaining->valuestring);
      device->controllable = controllable;
      device->environment = hass_ui_environment_domain(domain);
      device->available = available;
      device->on = on;
      device->active = hass_ui_active_state(device);
      if (!strcmp(domain, "light")) g_hass_ui.count_light++;
      else if (!strcmp(domain, "switch") || !strcmp(domain, "input_boolean"))
        g_hass_ui.count_switch++;
      else if (!strcmp(domain, "fan")) g_hass_ui.count_fan++;
      else if (!strcmp(domain, "sensor") ||
               !strcmp(domain, "binary_sensor")) g_hass_ui.count_sensor++;
      else g_hass_ui.count_other++;
      if (selected_entity[0] && !strcmp(selected_entity, device->entity))
        g_hass_ui.selected = (int)g_hass_ui.stored;
      g_hass_ui.stored++;
    }
  cJSON_Delete(root);
  g_glass.available = (int)g_hass_ui.count_online;
  g_glass.total = (int)g_hass_ui.count_all;
  g_hass_ui.loaded = true;
  g_hass_ui.error[0] = '\0';
  return true;
}

static int hass_ui_pending_index(void)
{
  int index = g_hass_ui.pending_device;
  if (index >= 0 && (unsigned)index < g_hass_ui.stored &&
      !strcmp(g_hass_ui.devices[index].entity, g_hass_ui.pending_entity))
    return index;
  for (unsigned i = 0; i < g_hass_ui.stored; i++)
    if (!strcmp(g_hass_ui.devices[i].entity, g_hass_ui.pending_entity))
      return (int)i;
  return -1;
}

static void hass_ui_clear_pending(void)
{
  g_hass_ui.pending_device = -1;
  g_hass_ui.pending_entity[0] = '\0';
  g_hass_ui.pending_brightness = -1;
  g_hass_ui.pending_on = false;
  g_hass_ui.verify_attempts = 0;
  g_hass_ui.verify_at = 0;
  g_hass_ui.verify_deadline = 0;
}

static void hass_ui_verify_uncertain(const char *message)
{
  if (!message || !message[0])
    message = "控制结果未确认，请刷新；不会重试";
  hass_ui_copy(g_hass_ui.error, sizeof(g_hass_ui.error), message);
  show_toast(message);
  hass_ui_clear_pending();
  g_hass_ui.busy = false;
  g_hass_ui.next_refresh = lv_tick_get() + HASS_UI_REFRESH_INTERVAL_MS;
}

/* A successful service POST only means HA accepted the request. Read the
 * original entity back and update the tile from observed state, never from
 * the requested value. */
static bool hass_ui_parse_verification(const char *body)
{
  cJSON *root = cJSON_ParseWithOpts(body, NULL, true);
  cJSON *entity = cJSON_GetObjectItemCaseSensitive(root, "entity_id");
  cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
  cJSON *attributes = cJSON_GetObjectItemCaseSensitive(root, "attributes");
  if (!cJSON_IsObject(root) || !cJSON_IsString(entity) ||
      !cJSON_IsString(state) || !cJSON_IsObject(attributes) ||
      strcmp(entity->valuestring, g_hass_ui.pending_entity))
    {
      cJSON_Delete(root);
      return false;
    }

  int index = hass_ui_pending_index();
  if (index < 0)
    {
      cJSON_Delete(root);
      return false;
    }

  struct hass_ui_device_s *device = &g_hass_ui.devices[index];
  bool old_online = device->available;
  bool old_running = device->available && device->controllable && device->on;
  bool available = strcmp(state->valuestring, "unavailable") &&
                   strcmp(state->valuestring, "unknown");
  bool on = !strcmp(state->valuestring, "on");
  cJSON *brightness = cJSON_GetObjectItemCaseSensitive(attributes,
                                                        "brightness");
  int level = -1;
  if (cJSON_IsNumber(brightness))
    {
      level = (brightness->valueint * 100 + 127) / 255;
      if (level < 0) level = 0;
      if (level > 100) level = 100;
    }

  hass_ui_copy(device->state, sizeof(device->state), state->valuestring);
  device->available = available;
  device->on = on;
  if (level >= 0) device->brightness = level;
  device->active = hass_ui_active_state(device);
  bool running = device->available && device->controllable && device->on;
  if (old_online != device->available)
    {
      if (device->available) g_hass_ui.count_online++;
      else if (g_hass_ui.count_online) g_hass_ui.count_online--;
    }
  if (old_running != running)
    {
      if (running) g_hass_ui.count_running++;
      else if (g_hass_ui.count_running) g_hass_ui.count_running--;
    }

  bool matched = available && on == g_hass_ui.pending_on;
  if (matched && g_hass_ui.pending_brightness > 0)
    matched = level >= 0 &&
              abs(level - g_hass_ui.pending_brightness) <= 1;
  cJSON_Delete(root);
  return matched;
}

static bool hass_ui_submit_verification(void)
{
  uint32_t now = lv_tick_get();
  if (!g_hass_ui.pending_entity[0]) return false;
  if ((int32_t)(now - g_hass_ui.verify_deadline) >= 0)
    {
      hass_ui_verify_uncertain(NULL);
      return false;
    }
  int ret = hass_get_state(g_hass_ui.client, g_hass_ui.pending_entity);
  if (ret < 0)
    {
      if (ret == -EBUSY)
        {
          g_hass_ui.verify_at = now + 250;
          g_hass_ui.busy = true;
        }
      else
        {
          hass_ui_verify_uncertain(NULL);
        }
      return false;
    }
  g_hass_ui.request = HASS_UI_REQUEST_VERIFY;
  g_hass_ui.verify_attempts++;
  g_hass_ui.verify_at = 0;
  g_hass_ui.busy = true;
  lv_label_set_text(g_hass_ui.top_status, "正在核对");
  return true;
}

static void hass_ui_refresh_request(bool notify)
{
  if (!g_hass_ui.client || !g_hass_ui.configured) return;
  if (g_hass_ui.request != HASS_UI_REQUEST_NONE || g_hass_ui.busy ||
      g_hass_ui.pending_entity[0])
    {
      if (notify) show_toast("家庭服务正在同步，请稍候");
      return;
    }
  int ret = hass_get(g_hass_ui.client, "states");
  if (ret < 0)
    {
      if (ret == -EBUSY)
        {
          g_hass_ui.next_refresh = lv_tick_get() + 750;
          if (notify) show_toast("家庭服务正忙，稍后自动重试");
        }
      else
        {
          hass_ui_set_error(0, ret);
          hass_ui_render_summary();
          if (notify) show_toast("同步请求未发送，请检查连接");
        }
      return;
    }
  g_hass_ui.request = HASS_UI_REQUEST_STATES;
  g_hass_ui.busy = true;
  g_hass_ui.cached = false;
  g_hass_ui.error[0] = '\0';
  lv_label_set_text(g_hass_ui.top_status, "正在同步");
  hass_ui_render_summary();
}

static void hass_ui_refresh_clicked(lv_event_t *event)
{
  LV_UNUSED(event);
  hass_ui_refresh_request(true);
}

static void hass_ui_nav(lv_event_t *event)
{
  uintptr_t mode = (uintptr_t)lv_event_get_user_data(event);
  if (mode > HASS_UI_ENVIRONMENT) return;
  g_hass_ui.mode = (enum hass_ui_mode_e)mode;
  g_hass_ui.page = 0;
  hass_ui_render_devices();
}

static void hass_ui_page_step(int delta)
{
  if (g_hass_ui.pages <= 1) return;
  int page = (int)g_hass_ui.page + delta;
  if (page < 0 || page >= (int)g_hass_ui.pages)
    {
      show_toast(delta < 0 ? "已经是第一页" : "已经是最后一页");
      return;
    }
  g_hass_ui.page = (unsigned)page;
  hass_ui_render_devices();
}

static void hass_ui_page_prev(lv_event_t *event)
{
  LV_UNUSED(event);
  hass_ui_page_step(-1);
}

static void hass_ui_page_next(lv_event_t *event)
{
  LV_UNUSED(event);
  hass_ui_page_step(1);
}

static void hass_ui_settings(lv_event_t *event)
{
  LV_UNUSED(event);
  glass_portal_show(glass_homeassistant);
}

static void hass_ui_poll(lv_timer_t *timer)
{
  LV_UNUSED(timer);
  if (!g_hass_ui.client || !g_hass_ui.root) return;
  uint32_t now = lv_tick_get();
  bool configured = false;
  bool service_busy = false;
  hass_status(&configured, &service_busy, NULL);
  g_hass_ui.configured = configured;
  struct hass_result_s result;
  int ret = hass_poll(g_hass_ui.client, &result);
  if (ret < 0)
    {
      if (g_hass_ui.pending_entity[0]) hass_ui_verify_uncertain(NULL);
      else
        {
          hass_ui_set_error(0, ret);
          g_hass_ui.busy = false;
          g_hass_ui.next_refresh = now + HASS_UI_REFRESH_INTERVAL_MS;
        }
      hass_ui_render_summary();
      return;
    }
  g_hass_ui.busy = result.busy ||
    (service_busy && g_hass_ui.request != HASS_UI_REQUEST_NONE) ||
    (g_hass_ui.pending_entity[0] &&
     g_hass_ui.request == HASS_UI_REQUEST_NONE);
  if (result.done)
    {
      enum hass_ui_request_e completed = g_hass_ui.request;
      g_hass_ui.request = HASS_UI_REQUEST_NONE;
      g_hass_ui.busy = false;
      bool success = result.error == 0 && result.status >= 200 &&
                     result.status < 300;
      bool verify_expired = completed == HASS_UI_REQUEST_VERIFY &&
        (int32_t)(now - g_hass_ui.verify_deadline) >= 0;
      if (verify_expired)
        {
          hass_ui_verify_uncertain(NULL);
        }
      else if (!success)
        {
          hass_ui_set_error(result.status, result.error);
          if (completed == HASS_UI_REQUEST_VERIFY)
            {
              if (result.status == 401 || result.status == 403)
                {
                  show_toast(g_hass_ui.error);
                  hass_ui_clear_pending();
                }
              else
                {
                  hass_ui_verify_uncertain(NULL);
                }
            }
          else if (completed == HASS_UI_REQUEST_CONTROL)
            {
              show_toast(g_hass_ui.error[0] ? g_hass_ui.error :
                                                    "设备控制失败");
              hass_ui_clear_pending();
            }
          if (result.status != 401 && result.status != 403)
            g_hass_ui.next_refresh = now + HASS_UI_REFRESH_INTERVAL_MS;
        }
      else if (completed == HASS_UI_REQUEST_STATES)
        {
          g_hass_ui.cached = result.cached;
          if (result.body) hass_ui_parse(result.body);
          g_hass_ui.next_refresh = now + HASS_UI_REFRESH_INTERVAL_MS;
        }
      else if (completed == HASS_UI_REQUEST_CONTROL)
        {
          g_hass_ui.error[0] = '\0';
          g_hass_ui.verify_deadline = now + HASS_UI_VERIFY_WINDOW_MS;
          g_hass_ui.verify_at = now + 1;
          g_hass_ui.busy = true;
          lv_label_set_text(g_hass_ui.top_status, "正在核对");
        }
      else if (completed == HASS_UI_REQUEST_VERIFY)
        {
          bool matched = result.body &&
                         hass_ui_parse_verification(result.body);
          if (matched)
            {
              hass_ui_clear_pending();
              g_hass_ui.error[0] = '\0';
              g_hass_ui.next_refresh = now + HASS_UI_REFRESH_INTERVAL_MS;
              show_toast("设备状态已确认");
            }
          else if (g_hass_ui.verify_attempts >= HASS_UI_VERIFY_ATTEMPTS ||
                   (int32_t)(now - g_hass_ui.verify_deadline) >= 0)
            {
              hass_ui_verify_uncertain(NULL);
            }
          else
            {
              g_hass_ui.verify_at = now + HASS_UI_VERIFY_INTERVAL_MS;
              g_hass_ui.busy = true;
              lv_label_set_text(g_hass_ui.top_status, "等待确认");
            }
        }
      hass_result_free(&result);
      hass_ui_render_devices();
      hass_ui_render_summary();
    }
  now = lv_tick_get();
  if (g_hass_ui.pending_entity[0] &&
      g_hass_ui.request == HASS_UI_REQUEST_NONE &&
      (int32_t)(now - g_hass_ui.verify_deadline) >= 0)
    {
      hass_ui_verify_uncertain(NULL);
      hass_ui_render_devices();
      hass_ui_render_summary();
      return;
    }
  if (g_hass_ui.configured && g_hass_ui.request == HASS_UI_REQUEST_NONE &&
      !service_busy && g_hass_ui.pending_entity[0] &&
      g_hass_ui.verify_at &&
      (int32_t)(now - g_hass_ui.verify_at) >= 0)
    {
      hass_ui_submit_verification();
      hass_ui_render_summary();
      return;
    }
  if (g_hass_ui.configured && g_hass_ui.request == HASS_UI_REQUEST_NONE &&
      !service_busy && !g_hass_ui.pending_entity[0] &&
      g_hass_ui.next_refresh &&
      (int32_t)(now - g_hass_ui.next_refresh) >= 0)
    {
      g_hass_ui.next_refresh = 0;
      hass_ui_refresh_request(false);
    }
}

static void hass_ui_deleted(lv_event_t *event)
{
  LV_UNUSED(event);
  if (g_hass_ui.timer) lv_timer_delete(g_hass_ui.timer);
  if (g_hass_ui.client) hass_close(g_hass_ui.client);
  memset(&g_hass_ui, 0, sizeof(g_hass_ui));
}

static void glass_hass_app(lv_obj_t *card)
{
  lv_obj_clean(card);
  memset(&g_hass_ui, 0, sizeof(g_hass_ui));
  g_hass_ui.root = card;
  g_hass_ui.selected = -1;
  g_hass_ui.pending_device = -1;
  g_hass_ui.pending_brightness = -1;
  bool light = g_desktop.light_theme;
  /* Home Assistant default light/dark values from color.globals.ts. */
  g_hass_ui.primary = light ? 0x212121 : 0xe1e1e1;
  g_hass_ui.secondary = light ? 0x727272 : 0x9b9b9b;
  g_hass_ui.background = light ? 0xfafafa : 0x111111;
  g_hass_ui.surface = light ? 0xffffff : 0x1c1c1c;
  g_hass_ui.sidebar = light ? 0xffffff : 0x1c1c1c;
  /* divider = rgba(0,0,0,.12) over #fff / rgba(225,225,225,.12) over #1c1c1c;
   * variant = --secondary-background-color (#e5e5e5 / #282828). */
  g_hass_ui.divider = light ? 0xe0e0e0 : 0x343434;
  g_hass_ui.variant = light ? 0xe5e5e5 : 0x282828;
  g_hass_ui.inactive = 0x9e9e9e;
  g_hass_ui.disabled = light ? 0xbdbdbd : 0x6f6f6f;
  /* Theme primary color for interactive surfaces (the Mijia green). */
  g_hass_ui.accent = light ? 0x15956b : 0x43c996;
  g_hass_ui.warm = light ? 0xf06a3b : 0xff8a61;
  lv_obj_set_pos(card, 0, 0);
  lv_obj_set_size(card, 1024, 600);
  lv_obj_set_style_radius(card, 0, 0);
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_set_style_bg_color(card, lv_color_hex(g_hass_ui.background), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_add_event_cb(card, hass_ui_deleted, LV_EVENT_DELETE, NULL);

  lv_obj_t *sidebar = glass_box(card, 0, 0, 178, 500,
                                g_hass_ui.sidebar, 0);
  lv_obj_t *logo = glass_box(sidebar, 22, 24, 48, 48, g_hass_ui.warm, 14);
  lv_obj_t *home = glass_symbol(logo, LV_SYMBOL_HOME, 0, 0, 0xffffff);
  lv_obj_center(home);
  hass_ui_text(sidebar, "米家", 82, 24, 78, 28, g_hass_ui.primary);
  hass_ui_text(sidebar, "HOME", 84, 59, 72, 16, g_hass_ui.secondary);
  const char *nav_names[] = {"全屋", "常用", "环境"};
  for (unsigned i = 0; i < 3; i++)
    g_hass_ui.nav[i] = hass_ui_button(sidebar, nav_names[i], 12,
                                      116 + i * 60, 154, 48,
                                      g_hass_ui.sidebar,
                                      hass_ui_nav, (void *)(uintptr_t)i);
  g_hass_ui.count = hass_ui_text(sidebar, "0 台设备 · 0 在线", 24, 310,
                                 136, 16, g_hass_ui.secondary);
  hass_ui_button(sidebar, "返回桌面", 16, 434, 146, 44,
                 g_hass_ui.variant, panel_hide, NULL);

  g_hass_ui.heading = hass_ui_text(card, "全屋设备", 202, 24, 250, 28,
                                   g_hass_ui.primary);
  g_hass_ui.subtitle = hass_ui_text(card, "设备状态，一眼看清", 202, 64,
                                    300, 16, g_hass_ui.secondary);
  lv_obj_t *status_box = glass_box(card, 576, 28, 108, 34,
                                   g_hass_ui.variant, 17);
  g_hass_ui.top_status = hass_ui_text(status_box, "待同步", 8, 7, 92, 16,
                                      g_hass_ui.secondary);
  lv_obj_set_style_text_align(g_hass_ui.top_status, LV_TEXT_ALIGN_CENTER, 0);
  /* Paging lives above the grid, under the status chip. */
  g_hass_ui.page_prev = hass_ui_button(card, LV_SYMBOL_LEFT, 596, 66, 40, 34,
                                       g_hass_ui.variant, hass_ui_page_prev,
                                       NULL);
  g_hass_ui.page_next = hass_ui_button(card, LV_SYMBOL_RIGHT, 644, 66, 40, 34,
                                       g_hass_ui.variant, hass_ui_page_next,
                                       NULL);
  lv_obj_set_style_text_font(lv_obj_get_child(g_hass_ui.page_prev, 0),
                             &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_font(lv_obj_get_child(g_hass_ui.page_next, 0),
                             &lv_font_montserrat_24, 0);
  g_hass_ui.list = glass_box(card, 202, 106, 488, 376,
                             g_hass_ui.background, 0);

  /* Overview column: ha-card surface with the per-domain breakdown. */
  lv_obj_t *hero = glass_box(card, 706, 24, 294, 458, g_hass_ui.surface, 16);
  lv_obj_set_style_border_width(hero, 1, 0);
  lv_obj_set_style_border_color(hero, lv_color_hex(g_hass_ui.divider), 0);
  hass_ui_text(hero, "家庭中枢", 22, 18, 140, 16, g_hass_ui.secondary);
  lv_obj_t *hero_chip = glass_box(hero, 176, 12, 96, 30, g_hass_ui.accent,
                                  15);
  lv_obj_set_style_bg_opa(hero_chip, 40, 0);
  g_hass_ui.hero_badge = hass_ui_text(hero_chip, "未连接", 0, 4, 96, 16,
                                      g_hass_ui.accent);
  lv_obj_set_style_text_align(g_hass_ui.hero_badge, LV_TEXT_ALIGN_CENTER, 0);
  g_hass_ui.hero_state = hass_ui_text(hero, "准备连接", 22, 54, 250, 28,
                                      g_hass_ui.primary);
  g_hass_ui.hero_detail = hass_ui_text(hero, "等待配置地址", 22, 94,
                                       250, 16, g_hass_ui.secondary);
  lv_obj_t *rule = glass_box(hero, 22, 128, 250, 1, g_hass_ui.divider, 0);
  lv_obj_remove_flag(rule, LV_OBJ_FLAG_CLICKABLE);
  hass_ui_text(hero, "设备", 22, 146, 60, 16, g_hass_ui.secondary);
  hass_ui_text(hero, "在线", 108, 146, 60, 16, g_hass_ui.secondary);
  hass_ui_text(hero, "运行", 194, 146, 60, 16, g_hass_ui.secondary);
  g_hass_ui.total_value = hass_ui_text(hero, "0", 22, 170, 80, 28,
                                       g_hass_ui.primary);
  g_hass_ui.online_value = hass_ui_text(hero, "0", 108, 170, 80, 28,
                                        g_hass_ui.primary);
  g_hass_ui.running_value = hass_ui_text(hero, "0", 194, 170, 80, 28,
                                         g_hass_ui.primary);
  lv_obj_t *rule2 = glass_box(hero, 22, 218, 250, 1, g_hass_ui.divider, 0);
  lv_obj_remove_flag(rule2, LV_OBJ_FLAG_CLICKABLE);
  hass_ui_text(hero, "设备概览", 22, 234, 200, 16, g_hass_ui.secondary);
  static const char *const summary_names[5] =
    {"灯", "开关", "风扇", "传感器", "其他"};
  static const uint32_t summary_dots[5] =
    {HASS_HA_AMBER, HASS_HA_AMBER, HASS_HA_CYAN, HASS_HA_STATE_ICON,
     HASS_HA_GREY};
  for (unsigned i = 0; i < 5; i++)
    {
      lv_obj_t *row = glass_box(hero, 22, 262 + (int)i * 32, 250, 28,
                                g_hass_ui.surface, 0);
      lv_obj_t *dot = glass_box(row, 2, 9, 10, 10, summary_dots[i], 5);
      lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
      hass_ui_text(row, summary_names[i], 22, 4, 120, 16, g_hass_ui.primary);
      g_hass_ui.summary_count[i] = hass_ui_text(row, "0", 150, 0, 100, 20,
                                                g_hass_ui.primary);
      lv_obj_set_style_text_align(g_hass_ui.summary_count[i],
                                  LV_TEXT_ALIGN_RIGHT, 0);
      g_hass_ui.summary_row[i] = row;
    }
  g_hass_ui.sync_label = hass_ui_text(hero, "等待首次同步", 22, 426, 250,
                                      16, g_hass_ui.secondary);

  lv_obj_t *bar = glass_box(card, 0, 500, 1024, 100, g_hass_ui.surface, 0);
  lv_obj_t *bar_rule = glass_box(bar, 0, 0, 1024, 1, g_hass_ui.divider, 0);
  lv_obj_remove_flag(bar_rule, LV_OBJ_FLAG_CLICKABLE);
  g_hass_ui.bottom_badge = glass_box(bar, 24, 22, 56, 56,
                                     g_hass_ui.accent, 28);
  lv_obj_set_style_bg_opa(g_hass_ui.bottom_badge, HASS_HA_TINT_OPA, 0);
  g_hass_ui.bottom_icon = glass_symbol(g_hass_ui.bottom_badge,
                                       LV_SYMBOL_HOME, 0, 0,
                                       g_hass_ui.secondary);
  lv_obj_remove_flag(g_hass_ui.bottom_icon, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_center(g_hass_ui.bottom_icon);
  g_hass_ui.bottom_name = hass_ui_text(bar, "选择一个设备", 94, 20, 230, 20,
                                       g_hass_ui.primary);
  g_hass_ui.bottom_state = hass_ui_text(bar, "点按设备卡片查看并控制",
                                        94, 52, 230, 16,
                                        g_hass_ui.secondary);
  g_hass_ui.brightness_group = glass_box(bar, 330, 12, 250, 76,
                                         g_hass_ui.surface, 0);
  g_hass_ui.brightness_label = hass_ui_text(g_hass_ui.brightness_group,
                                             "亮度 100%", 0, 4, 200, 16,
                                             g_hass_ui.secondary);
  g_hass_ui.brightness = lv_slider_create(g_hass_ui.brightness_group);
  lv_obj_set_pos(g_hass_ui.brightness, 0, 38);
  lv_obj_set_size(g_hass_ui.brightness, 240, 8);
  lv_slider_set_range(g_hass_ui.brightness, 1, 100);
  lv_obj_set_style_bg_color(g_hass_ui.brightness,
                            lv_color_hex(g_hass_ui.variant), LV_PART_MAIN);
  lv_obj_set_style_radius(g_hass_ui.brightness, 4, LV_PART_MAIN);
  lv_obj_set_style_bg_color(g_hass_ui.brightness,
                            lv_color_hex(HASS_HA_AMBER), LV_PART_INDICATOR);
  lv_obj_set_style_radius(g_hass_ui.brightness, 4, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(g_hass_ui.brightness,
                            lv_color_hex(HASS_HA_AMBER), LV_PART_KNOB);
  lv_obj_set_style_radius(g_hass_ui.brightness, LV_RADIUS_CIRCLE, LV_PART_KNOB);
  lv_obj_set_style_pad_all(g_hass_ui.brightness, 6, LV_PART_KNOB);
  lv_obj_add_event_cb(g_hass_ui.brightness, hass_ui_brightness_changed,
                      LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(g_hass_ui.brightness, hass_ui_brightness_commit,
                      LV_EVENT_RELEASED, NULL);
  lv_obj_add_flag(g_hass_ui.brightness_group, LV_OBJ_FLAG_HIDDEN);
  hass_ui_button(bar, LV_SYMBOL_REFRESH, 766, 24, 52, 52,
                 g_hass_ui.variant, hass_ui_refresh_clicked, NULL);
  hass_ui_button(bar, "连接设置", 834, 26, 166, 48,
                 g_hass_ui.variant, hass_ui_settings, NULL);
  g_hass_ui.bottom_action = hass_ui_button(bar, "不可控制", 600, 24,
                                            150, 48, g_hass_ui.variant,
                                            hass_ui_bottom_toggle, NULL);
  g_hass_ui.bottom_action_label = lv_obj_get_child(g_hass_ui.bottom_action, 0);

  /* Reapply portal settings on entry so returning from QR configuration does
   * not require a reboot. panel_card() has already stopped the legacy QPK
   * client, so the configure handle can update the shared service safely. */
  int bootstrap = hass_portal_bootstrap();
  int client = hass_open(HASS_READ | HASS_CONTROL);
  if (client < 0)
    hass_ui_copy(g_hass_ui.error, sizeof(g_hass_ui.error),
                 "家庭服务暂时不可用");
  else
    g_hass_ui.client = (uint32_t)client;
  if (bootstrap < 0 && bootstrap != -EBUSY && !g_hass_ui.error[0])
    hass_ui_copy(g_hass_ui.error, sizeof(g_hass_ui.error),
                 "连接设置暂时无法载入");
  hass_status(&g_hass_ui.configured, &g_hass_ui.busy, NULL);
  if (g_hass_ui.client && g_hass_ui.configured)
    hass_get_url(g_hass_ui.client, g_hass_ui.url, sizeof(g_hass_ui.url));
  hass_ui_render_devices();
  hass_ui_render_summary();
  if (g_hass_ui.client && g_hass_ui.configured)
    {
      g_hass_ui.busy = false;
      hass_ui_refresh_request(false);
    }
  g_hass_ui.timer = lv_timer_create(hass_ui_poll, 250, NULL);
}

static void glass_homeassistant(lv_event_t *event)
{
  launch_builtin_homeassistant_qapp(event);
}
