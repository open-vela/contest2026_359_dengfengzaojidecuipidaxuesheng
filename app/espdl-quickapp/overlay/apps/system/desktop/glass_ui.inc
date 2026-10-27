/* SPDX-License-Identifier: Apache-2.0
 * Glass desktop and shared application lifecycle, 1024 x 600.
 * 渐变和半透明填充模拟磨砂层；不进行全屏模糊或连续背景重绘。
 */

#include "glass_status.h"
#include "glass_pager.h"
static void glass_status_tick(void);
static void glass_status_visibility(bool locked);
#define GLASS_HISTORY 24
#define GLASS_HOME_CARD_COUNT 5
/* Shown clock offset used only while this build has no zoneinfo database. */
#define GLASS_CLOCK_OFFSET_SECONDS (8 * 3600)
#include <limits.h>
#include "glass_wallpaper.h"
#include "glass_settings_store.h"
#include "glass_settings_service.h"
#include "glass_weather.h"
#include "glass_ime.h"
#include "glass_sd_font.h"
#if defined(CONFIG_ESPRESSIF_LEDC_TIMER0) && \
    CONFIG_ESPRESSIF_LEDC_CHANNEL0_PIN == 26 && CONFIG_PWM_NCHANNELS == 1
#include <fcntl.h>
#include <sys/ioctl.h>
#include <nuttx/timers/pwm.h>
#define GLASS_PWM_BACKLIGHT 1
static int g_backlight_fd = -1;
#endif

static int g_screen_brightness = 100;

static void glass_brightness_preview(lv_event_t *e)
{
  lv_obj_t *slider = lv_event_get_target(e);
  lv_obj_t *label = lv_event_get_user_data(e);
  const int value = lv_event_get_code(e) == LV_EVENT_PRESS_LOST ?
                    g_screen_brightness : lv_slider_get_value(slider);
  if (lv_event_get_code(e) == LV_EVENT_PRESS_LOST)
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
  lv_label_set_text_fmt(label, "%d%%", value);
}

static void glass_brightness_changed(lv_event_t *e)
{
#ifdef GLASS_PWM_BACKLIGHT
  lv_obj_t *slider = lv_event_get_target(e);
  lv_obj_t *label = lv_event_get_user_data(e);
  int value = lv_slider_get_value(slider);
  struct pwm_info_s info;
  memset(&info, 0, sizeof(info));
  info.frequency = 5000;
  info.channels[0].channel = 0;
  info.channels[0].duty = (uint32_t)value * 65536 / 100;
  if (g_backlight_fd < 0) g_backlight_fd = open("/dev/pwm0", O_RDWR);
  bool success = g_backlight_fd >= 0 &&
    ioctl(g_backlight_fd, PWMIOC_SETCHARACTERISTICS, (unsigned long)&info) >= 0 &&
    ioctl(g_backlight_fd, PWMIOC_START, 0) >= 0;
  if (!success)
    {
      lv_slider_set_value(slider, g_screen_brightness, LV_ANIM_OFF);
      lv_label_set_text(label, "调节失败");
      return;
    }
  g_screen_brightness = value;
  lv_label_set_text_fmt(label, "%d%%", value);
#else
  LV_UNUSED(e);
#endif
}

static struct
{
  lv_obj_t *home;
  lv_obj_t *temperature;
  lv_obj_t *humidity;
  lv_obj_t *humidity_status;
  lv_obj_t *weather_condition;
  lv_obj_t *air;
  lv_obj_t *devices;
  lv_obj_t *sync;
  lv_obj_t *device_rows[2], *device_names[2], *device_states[2], *device_pins[2];
  lv_obj_t *device_empty;
  lv_obj_t *widget_title;
  lv_obj_t *widget_value;
  lv_obj_t *widget_hint;
  lv_obj_t *widget_badge;
  lv_obj_t *widget_category;
  lv_obj_t *chart;
  lv_chart_series_t *series;
  lv_obj_t *chart_hint;
  lv_obj_t *settings_switch;
  lv_obj_t *widget_dropdown;
  lv_obj_t *lyric_font_dropdown;
  lv_obj_t *home_cards[GLASS_HOME_CARD_COUNT];
  lv_obj_t *motion_switch;
  lv_obj_t *brightness_slider;
  lv_font_t *font72;
  lv_font_t *font48;
  lv_font_t *music_font72;
  lv_timer_t *timer;
  int temperature_c;
  int humidity_pct;
  int pm25;
  int available;
  int total;
  int history[GLASS_HISTORY];
  unsigned int samples;
  uint32_t received;
  bool has_sample;
  bool reduced_motion;
  bool music_original_only;
  unsigned int palette;
  unsigned int widget;
} g_glass;

static unsigned music_font_selected,music_font_requested;
static void home_music_refresh(void);
static void home_music_obscure(void);
static void music_full_fonts_poll(void);
static void music_full_font_selected(lv_event_t *event);

/* Cache display semantics, never lv_label_get_text(): LONG_DOT modifies that
 * buffer in place. No per-tick allocation or retained label text pointers.
 */
static struct glass_display_s
{
  unsigned long uptime_minutes;
  uint32_t sync_value;
  int temperature;
  int humidity;
  int aqi;
  unsigned weather_revision;
  bool weather_stale;
  enum glass_weather_state weather_state;
  bool weather_valid;
  int available;
  int total;
  uint8_t widget;
  uint8_t sync_unit;
  bool has_sample;
  bool valid;
} g_glass_display;

static bool g_preferences_save_failed;
static bool g_preferences_waiting;
static bool g_preferences_requested;
static unsigned g_preferences_shown = UINT_MAX;
static lv_obj_t *g_preferences_status;

static unsigned glass_preferences_display_state(void)
{
  return g_preferences_waiting ? 1 : g_preferences_save_failed ? 2 :
         g_preferences_requested ? 3 : 0;
}

static const char *glass_preferences_status_text(void)
{
  static const char *text[] = {"", "保存中", "保存失败", "已保存"};
  return text[glass_preferences_display_state()];
}

static void glass_preferences_show_status(void)
{
  unsigned state = glass_preferences_display_state();
  if (g_preferences_status != NULL && state != g_preferences_shown)
    {
      lv_label_set_text(g_preferences_status, glass_preferences_status_text());
      g_preferences_shown = state;
    }
}

static void glass_preferences_status_deleted(lv_event_t *e)
{
  LV_UNUSED(e);
  g_preferences_status = NULL;
  g_preferences_shown = UINT_MAX;
}

static void glass_preferences_update(void)
{
  if (!g_preferences_waiting) return;
  struct glass_preferences_status status;
  if (glass_preferences_get_status(&status) != 0 || status.pending) return;
  g_preferences_waiting = false;
  g_preferences_save_failed = status.error != 0;
  glass_preferences_show_status();
  if (g_preferences_save_failed) show_toast("设置保存失败");
}

static void glass_save_preferences(void)
{
  uint8_t values[4] = {g_desktop.light_theme, g_glass.palette,
                        g_glass.widget, g_glass.reduced_motion};
  portal_desktop_publish(values);
  g_preferences_save_failed = glass_preferences_submit(values) != 0;
  g_preferences_requested = true;
  g_preferences_waiting = !g_preferences_save_failed;
  glass_preferences_show_status();
  if (g_preferences_save_failed) show_toast("设置保存失败");
}

static lv_obj_t *glass_box(lv_obj_t *parent, int x, int y, int w, int h,
                           uint32_t color, int radius)
{
  lv_obj_t *obj = lv_obj_create(parent);
  lv_obj_remove_style_all(obj);
  lv_obj_set_pos(obj, x, y);
  lv_obj_set_size(obj, w, h);
  lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(obj, radius, 0);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  return obj;
}

static void glass_gradient(lv_obj_t *obj, uint32_t color, lv_grad_dir_t dir)
{
  lv_obj_set_style_bg_grad_color(obj, lv_color_hex(color), 0);
  lv_obj_set_style_bg_grad_dir(obj, dir, 0);
}

static lv_obj_t *glass_card(lv_obj_t *parent, int x, int y, int w, int h,
                            uint32_t color)
{
  lv_obj_t *card = glass_box(parent, x, y, w, h, color, 24);
  lv_obj_set_style_bg_opa(card, 238, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_border_opa(card, g_desktop.light_theme ? 210 : 40, 0);
  lv_obj_set_style_shadow_width(card, 12, 0);
  lv_obj_set_style_shadow_ofs_y(card, 5, 0);
  lv_obj_set_style_shadow_color(card, lv_color_hex(0x101428), 0);
  lv_obj_set_style_shadow_opa(card, 18, 0);
  return card;
}

static lv_obj_t *glass_text(lv_obj_t *parent, const char *text,
                            int x, int y, int width, int size, uint32_t color)
{
  lv_obj_t *label = make_label(parent, text, color, size);
  lv_obj_set_pos(label, x, y);
  lv_obj_set_width(label, width);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
  return label;
}

static lv_obj_t *glass_number(lv_obj_t *parent, const char *text,
                              int x, int y, int width, bool large,
                              uint32_t color)
{
  lv_obj_t *label = glass_text(parent, text, x, y, width, 28, color);
  const lv_font_t *font = large ? g_glass.font72 : g_glass.font48;
  if (font != NULL)
    {
      lv_obj_set_style_text_font(label, font, 0);
    }
  return label;
}

static lv_obj_t *glass_symbol(lv_obj_t *parent, const char *text,
                              int x, int y, uint32_t color)
{
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, text);
  lv_obj_set_pos(label, x, y);
  lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
  lv_obj_set_style_text_font(label, glass_icon_font(24), 0);
  return label;
}

static lv_obj_t *glass_button(lv_obj_t *parent, const char *text,
                              int x, int y, int w, uint32_t color,
                              lv_event_cb_t callback, void *data)
{
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_remove_style_all(button);
  lv_obj_set_pos(button, x, y);
  lv_obj_set_size(button, w, 48);
  style_button(button, color);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_border_opa(button, 25, 0);
  lv_obj_set_style_bg_opa(button, 180, LV_STATE_PRESSED);
  if (callback != NULL)
    {
      lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, data);
    }
  lv_obj_t *label = make_label(button, text, theme_primary(), 16);
  lv_obj_set_width(label, w - 24);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(label);
  return button;
}

static void glass_clickable(lv_obj_t *card, lv_event_cb_t callback, void *data)
{
  lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_border_opa(card, 160, LV_STATE_PRESSED);
  glass_motion_button(card);
  lv_obj_add_event_cb(card, callback, LV_EVENT_CLICKED, data);
}

#include "glass_detail_ui.inc"

static void glass_chart_deleted(lv_event_t *e)
{
  LV_UNUSED(e);
  g_glass.chart = NULL;
  g_glass.series = NULL;
  g_glass.chart_hint = NULL;
}

static void glass_chart_refresh(void)
{
  unsigned int i;
  int low = -40;
  int high = 60;
  if (g_glass.chart == NULL)
    {
      return;
    }
  /* Render only real samples. This LVGL chart backend performs arithmetic
   * on POINT_NONE before clipping, which overflows on an empty history. */
  lv_chart_set_point_count(g_glass.chart, g_glass.samples ? g_glass.samples : 1);
  for (i = 0; i < g_glass.samples; i++)
    {
      if (i < g_glass.samples)
        {
          if (g_glass.history[i] < low) low = g_glass.history[i] - 5;
          if (g_glass.history[i] > high) high = g_glass.history[i] + 5;
        }
      lv_chart_set_value_by_id(g_glass.chart, g_glass.series, i, g_glass.history[i]);
    }
  lv_chart_set_range(g_glass.chart, LV_CHART_AXIS_PRIMARY_Y, low, high);
  if (g_glass.samples == 0)
    {
      lv_chart_set_value_by_id(g_glass.chart, g_glass.series, 0, 0);
      lv_obj_add_flag(g_glass.chart, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(g_glass.chart_hint, "暂无温度记录 · 在智能家居中连接传感器");
    }
  else
    {
      lv_obj_remove_flag(g_glass.chart, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text_fmt(g_glass.chart_hint,
        "本次运行已采集 %u 个温度样本 · 轻触折线查看数值", g_glass.samples);
    }
}

static void glass_chart_pressed(lv_event_t *e)
{
  int32_t point = lv_chart_get_pressed_point(lv_event_get_target(e));
  if (point >= 0 && (unsigned int)point < g_glass.samples)
    {
      lv_label_set_text_fmt(g_glass.chart_hint, "样本 %ld / %u     %d C",
        (long)point + 1, g_glass.samples, g_glass.history[point]);
    }
}

static void glass_environment(lv_event_t *e)
{
  LV_UNUSED(e);
  lv_obj_t *card = panel_card("环境趋势");
  glass_text(card, "温度 / 摄氏度", 28, 80, 300, 20, theme_primary());
  glass_text(card, "来自 Home Assistant · 最近 24 次有效采样",
             28, 116, 800, 16, theme_secondary());
  g_glass.chart = lv_chart_create(card);
  lv_obj_set_pos(g_glass.chart, 40, 168);
  lv_obj_set_size(g_glass.chart, 836, 230);
  lv_obj_set_style_bg_color(g_glass.chart, lv_color_hex(theme_surface()), 0);
  lv_obj_set_style_border_width(g_glass.chart, 0, 0);
  lv_obj_set_style_line_color(g_glass.chart, lv_color_hex(theme_secondary()), LV_PART_MAIN);
  lv_obj_set_style_line_opa(g_glass.chart, 40, LV_PART_MAIN);
  lv_obj_set_style_line_width(g_glass.chart, 3, LV_PART_ITEMS);
  lv_chart_set_type(g_glass.chart, LV_CHART_TYPE_LINE);
  lv_chart_set_range(g_glass.chart, LV_CHART_AXIS_PRIMARY_Y, -40, 60);
  lv_chart_set_div_line_count(g_glass.chart, 5, 6);
  lv_chart_set_point_count(g_glass.chart, GLASS_HISTORY);
  g_glass.series = lv_chart_add_series(g_glass.chart,
    lv_color_hex(g_desktop.light_theme ? 0x7761c9 : 0xbdadff), LV_CHART_AXIS_PRIMARY_Y);
  lv_obj_add_event_cb(g_glass.chart, glass_chart_deleted, LV_EVENT_DELETE, NULL);
  lv_obj_add_event_cb(g_glass.chart, glass_chart_pressed, LV_EVENT_PRESSING, NULL);
  g_glass.chart_hint = glass_text(card, "", 28, 426, 870, 16, theme_secondary());
  glass_chart_refresh();
}

/* Only the LVGL owner thread publishes data. No network calls, locks or flash
 * writes happen here; the existing asynchronous HA worker still owns HTTP.
 */
void glass_dashboard_publish(int temperature, int humidity, int pm25,
                             int available, int total)
{
  unsigned int index;
  if (total < 0 || total > 100000 || available < 0 || available > total)
    {
      return;
    }
  g_glass.temperature_c = temperature >= -80 && temperature <= 80 ? temperature : -999;
  g_glass.humidity_pct = humidity >= 0 && humidity <= 100 ? humidity : -1;
  g_glass.pm25 = pm25 >= 0 && pm25 <= 2000 ? pm25 : -1;
  g_glass.available = available;
  g_glass.total = total;
  g_glass.received = lv_tick_get();
  g_glass.has_sample = true;
  if (g_glass.temperature_c != -999)
    {
      if (g_glass.samples == GLASS_HISTORY)
        {
          memmove(g_glass.history, g_glass.history + 1,
                  sizeof(g_glass.history) - sizeof(g_glass.history[0]));
          index = GLASS_HISTORY - 1;
        }
      else
        {
          index = g_glass.samples++;
        }
      g_glass.history[index] = g_glass.temperature_c;
    }
  glass_chart_refresh();
}

#include "glass_weather_ui.inc"
#include "glass_home_devices.inc"

static void glass_update(lv_timer_t *timer)
{
  glass_status_tick();
  uint8_t portal_values[4];
  if(portal_desktop_take(portal_values)) {
    g_desktop.light_theme=portal_values[0]; g_glass.palette=portal_values[1];
    g_glass.widget=portal_values[2]; g_glass.reduced_motion=portal_values[3];
    glass_rebuild(); glass_save_preferences();
  }
  glass_preferences_update();
  home_devices_tick();
  struct timespec uptime = {0};
  struct glass_weather_snapshot weather;
  glass_weather_get(&weather);
  bool initial = !g_glass_display.valid;
  LV_UNUSED(timer);
  if (g_glass.home == NULL)
    {
      return;
    }
  clock_gettime(CLOCK_MONOTONIC, &uptime);
  uint64_t weather_now = (uint64_t)uptime.tv_sec * 1000 + uptime.tv_nsec / 1000000;
  bool weather_stale=weather.valid&&weather_now>=weather.received_ms&&
    weather_now-weather.received_ms>=7200000;
  /* Keep service polling and status alive, but do not repaint or decode a
   * fully covered home. Its cached image stays ready for the back gesture. */
  if(glass_page_covers_home()||!glass_ui_can_paint(g_glass.home)){
    home_music_obscure();
    g_weather_home.deferred=true;
    glass_weather_detail_update(&weather,weather_stale);
    return;
  }
  glass_home_devices_paint();
  home_music_refresh();
  uint32_t sync_seconds = lv_tick_elaps(g_glass.received) / 1000;
  struct glass_display_s display =
    {
      .uptime_minutes = (unsigned long)uptime.tv_sec / 60,
      .sync_value = sync_seconds / (sync_seconds >= 86400 ? 86400 :
                                    sync_seconds >= 3600 ? 3600 :
                                    sync_seconds >= 60 ? 60 : 1),
      .sync_unit = sync_seconds >= 86400 ? 3 : sync_seconds >= 3600 ? 2 :
                   sync_seconds >= 60 ? 1 : 0,
      .temperature = weather.valid ? weather.data.temperature : -999,
      .humidity = weather.valid ? weather.data.humidity : -1,
      .aqi = weather.valid ? weather.data.aqi : -1,
      .weather_revision = weather.revision,
      .weather_stale = weather_stale,
      .weather_state = weather.state,
      .weather_valid = weather.valid,
      .available = g_glass.available,
      .total = g_glass.total,
      .widget = g_glass.widget < 2 ? g_glass.widget : 2,
      .has_sample = g_glass.has_sample,
      .valid = true,
    };
  if (initial || display.temperature != g_glass_display.temperature)
    {
      if (display.temperature != -999)
        lv_label_set_text_fmt(g_glass.temperature, "%d°", display.temperature);
      else
        lv_label_set_text(g_glass.temperature, "--°");
    }
  if (initial || display.humidity != g_glass_display.humidity)
    {
      if (display.humidity >= 0)
        lv_label_set_text_fmt(g_glass.humidity, "%d%%", display.humidity);
      else
        lv_label_set_text(g_glass.humidity, "--%");
    }
  bool sample_changed = display.has_sample != g_glass_display.has_sample;
  if (initial || sample_changed ||
      (display.has_sample &&
       (display.available != g_glass_display.available ||
        display.total != g_glass_display.total)))
    {
      if (display.has_sample)
        lv_label_set_text_fmt(g_glass.devices, "%d / %d",
                              display.available, display.total);
      else
        lv_label_set_text(g_glass.devices, "-- / --");
    }
  if (initial || sample_changed ||
      (display.has_sample &&
       (display.sync_value != g_glass_display.sync_value ||
        display.sync_unit != g_glass_display.sync_unit)))
    {
      if (display.has_sample)
        {
          /* Expired snapshots do not need a perpetual once-per-second redraw. */
          static const char *units[] = {"秒", "分钟", "小时", "天"};
          lv_label_set_text_fmt(g_glass.sync, "%lu %s前",
                                (unsigned long)display.sync_value,
                                units[display.sync_unit]);
        }
      else
        lv_label_set_text(g_glass.sync, "尚未同步");
    }
  bool widget_changed = initial || display.widget != g_glass_display.widget;
  bool weather_changed = widget_changed ||
    weather_home_data_changed(&weather) ||
    display.weather_state != g_glass_display.weather_state ||
    display.weather_valid != g_glass_display.weather_valid ||
    display.weather_stale != g_glass_display.weather_stale;
  if (initial || display.humidity != g_glass_display.humidity)
    lv_label_set_text(g_glass.humidity_status,
      display.humidity < 0 ? "暂无数据" : display.humidity < 30 ? "偏干燥" :
      display.humidity > 60 ? "偏潮湿" : "舒适");
  if (weather_changed)
    lv_label_set_text(g_glass.weather_condition,
      weather.valid ? weather.data.condition :
      weather.state == WEATHER_WAIT_NET ? "等待联网" :
      weather.state == WEATHER_WAIT_TIME ? "等待校时" :
      weather.state == WEATHER_FAILED ? "更新失败" : "正在更新");
  if (widget_changed)
    {
      lv_label_set_text(g_glass.widget_title, display.widget == 0 ? "空气质量" :
                        display.widget == 1 ? "环境湿度" : "系统运行");
      if (display.widget == 0)
        lv_obj_remove_flag(g_glass.widget_badge, LV_OBJ_FLAG_HIDDEN);
      else
        lv_obj_add_flag(g_glass.widget_badge, LV_OBJ_FLAG_HIDDEN);
    }
  if (display.widget == 0)
    {
      if (widget_changed || display.aqi != g_glass_display.aqi)
        {
          if (display.aqi >= 0)
            lv_label_set_text_fmt(g_glass.widget_value, "%d", display.aqi);
          else
            lv_label_set_text(g_glass.widget_value, "--");
        }
      if (weather_changed)
        {
          lv_label_set_text(g_glass.air, "AQI");
          lv_label_set_text(g_glass.widget_category,
            weather.valid && weather.data.aqi >= 0 ? weather.data.category : "暂无数据");
          static const uint32_t colors[] = {0x969aa5, 0x49b894, 0xba9e40,
            0xd89256, 0xda7275, 0xac80ae, 0xbd6989};
          static const uint32_t light_colors[] = {0x606b7e, 0x24765b, 0x80651b,
            0x9a581e, 0xa43c48, 0x81548c, 0x943453};
          unsigned level = weather.valid && weather.data.aqi >= 0 ? weather.data.level : 0;
          uint32_t color = (g_desktop.light_theme ? light_colors : colors)[level <= 6 ? level : 0];
          lv_obj_set_style_text_color(g_glass.widget_value,
                                      lv_color_hex(color), 0);
          lv_obj_set_style_text_color(g_glass.widget_category, lv_color_hex(color), 0);
          lv_obj_set_style_bg_color(g_glass.widget_badge, lv_color_hex(color), 0);
        }
    }
  else if (display.widget == 1)
    {
      if (widget_changed || display.humidity != g_glass_display.humidity)
        {
          if (display.humidity >= 0)
            lv_label_set_text_fmt(g_glass.widget_value, "%d", display.humidity);
          else
            lv_label_set_text(g_glass.widget_value, "--");
        }
      if (widget_changed)
        {
          lv_label_set_text(g_glass.air, "%");
          lv_obj_set_style_text_color(g_glass.widget_value, lv_color_hex(theme_primary()), 0);
        }
    }
  else
    {
      if (widget_changed ||
          display.uptime_minutes != g_glass_display.uptime_minutes)
        lv_label_set_text_fmt(g_glass.widget_value, "%lu", display.uptime_minutes);
      if (widget_changed)
        {
          lv_label_set_text(g_glass.widget_hint, "分钟 · 本机实时数据");
          lv_label_set_text(g_glass.air, "分钟");
          lv_obj_set_style_text_color(g_glass.widget_value, lv_color_hex(theme_primary()), 0);
        }
    }
  if (display.widget != 2 && weather_changed)
    {
      if (weather.valid)
        {
          const char *status = weather.state == WEATHER_WAIT_NET ? "离线数据" :
            weather.state == WEATHER_WAIT_TIME ? "等待校时" :
            weather.state == WEATHER_FETCHING ? "更新中" :
            weather.state == WEATHER_FAILED ? "更新失败" :
            display.weather_stale ? "待更新" : weather.data.condition;
          lv_label_set_text_fmt(g_glass.widget_hint, "%s · %s", weather.data.location, status);
        }
      else
        lv_label_set_text(g_glass.widget_hint,
          weather.state == WEATHER_WAIT_NET ? "等待联网" :
          weather.state == WEATHER_WAIT_TIME ? "等待校时" :
          weather.state == WEATHER_FAILED ? "天气暂不可用 · 稍后重试" : "正在更新天气");
    }
  weather_home_update(&weather, display.weather_stale, weather_is_night());
  glass_weather_detail_update(&weather, display.weather_stale);
  g_glass_display = display;
  g_weather_home.deferred=false;
}

static void glass_weather_home_resume(void)
{
  if(!g_weather_home.deferred)return;
  g_weather_home.deferred=false;
  glass_update(NULL);
}

static void glass_palette_select(lv_event_t *e)
{
  g_glass.palette = (unsigned int)(uintptr_t)lv_event_get_user_data(e);
  glass_rebuild();
  glass_save_preferences();
  /* A deferred rebuild keeps the pressed object's event traversal valid. */
  if (!g_desktop.settings_rebuild_pending &&
      lv_async_call(settings_rebuild_async, NULL) == LV_RESULT_OK)
    {
      g_desktop.settings_rebuild_pending = true;
    }
}

static void glass_widget_select(lv_event_t *e)
{
  g_glass.widget = lv_dropdown_get_selected(lv_event_get_target(e));
  glass_update(NULL);
  glass_save_preferences();
}

static void glass_motion_changed(lv_event_t *e)
{
  g_glass.reduced_motion = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
  glass_motion_set_reduced(g_glass.reduced_motion);
  glass_save_preferences();
  if (g_glass.reduced_motion)
    {
      lv_anim_delete(g_glass.home, NULL);
      lv_obj_set_style_opa(g_glass.home, LV_OPA_COVER, 0);
      for (int i = 0; i < GLASS_HOME_CARD_COUNT; i++)
        {
          lv_anim_delete(g_glass.home_cards[i], NULL);
          lv_obj_set_style_translate_y(g_glass.home_cards[i], 0, 0);
        }
    }
  lv_obj_set_style_anim_duration(g_glass.settings_switch,
                                 g_glass.reduced_motion ? 0 : 160, 0);
  lv_obj_set_style_anim_duration(lv_event_get_target(e),
                                 g_glass.reduced_motion ? 0 : 160, 0);
}

static lv_obj_t *g_control_card;
static bool g_control_close_pending;

static void glass_control_close_async(void *data)
{
  LV_UNUSED(data);
  g_control_close_pending = false;
  if (g_control_card != NULL && g_desktop.current_card == g_control_card)
    panel_hide(NULL);
}

static void glass_control_deleted(lv_event_t *e)
{
  LV_UNUSED(e);
  if (g_control_close_pending)
    lv_async_call_cancel(glass_control_close_async, NULL);
  g_control_close_pending = false;
  g_control_card = NULL;
}

static void glass_control_close_request(void)
{
  if (!g_control_close_pending &&
      lv_async_call(glass_control_close_async, NULL) == LV_RESULT_OK)
    g_control_close_pending = true;
}

static void glass_settings_back(lv_event_t *e)
{
  LV_UNUSED(e);
  glass_control_close_request();
}

static void glass_control_switch_style(lv_obj_t *sw)
{
  lv_obj_remove_style_all(sw);
  lv_obj_set_ext_click_area(sw, 8);
  if (g_glass.reduced_motion)
    {
      static const lv_style_prop_t properties[] = {0};
      static lv_style_transition_dsc_t no_motion;
      /* This LVGL version requires a valid descriptor, including for no animation. */
      lv_style_transition_dsc_init(&no_motion, properties, lv_anim_path_linear, 0, 0, NULL);
      lv_obj_set_style_transition(sw, &no_motion, LV_PART_MAIN);
      lv_obj_set_style_transition(sw, &no_motion, LV_PART_INDICATOR);
      lv_obj_set_style_transition(sw, &no_motion, LV_PART_KNOB);
    }
  lv_obj_set_style_pad_all(sw, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(sw, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_KNOB);
  lv_obj_set_style_bg_opa(sw, LV_OPA_TRANSP, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_bg_color(sw, lv_color_hex(g_desktop.light_theme ?
                            0xcbd0d8 : 0x505765), LV_PART_MAIN);
  lv_obj_set_style_bg_color(sw, lv_color_hex(0x3a9a87),
                            LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(sw, lv_color_hex(0xffffff), LV_PART_KNOB);
  lv_obj_set_style_pad_all(sw, -4, LV_PART_KNOB);
  lv_obj_set_style_shadow_width(sw, 5, LV_PART_KNOB);
  lv_obj_set_style_shadow_opa(sw, 40, LV_PART_KNOB);
  lv_obj_set_style_anim_duration(sw, g_glass.reduced_motion ? 0 : 160, 0);
}

static void glass_settings(lv_event_t *e);

/* Shared form styling includes the placeholder, which otherwise uses
 * LVGL's Latin-only theme font. Keep keyboard symbols in Montserrat. */
static void glass_input_style(lv_obj_t *input)
{
  glass_app_input_style(input,glass_app_palette(g_desktop.light_theme));
  lv_obj_set_style_text_font(input, zh_font(20), LV_PART_MAIN);
  lv_obj_set_style_text_font(input, zh_font(20), LV_PART_TEXTAREA_PLACEHOLDER);
  lv_obj_set_style_text_color(input, lv_color_hex(theme_primary()), LV_PART_MAIN);
  lv_obj_set_style_text_color(input, lv_color_hex(theme_secondary()), LV_PART_TEXTAREA_PLACEHOLDER);
  lv_obj_set_style_bg_color(input, lv_color_hex(theme_surface()), 0);
  lv_obj_set_style_bg_opa(input, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(input, lv_color_hex(detail_line()), 0);
  lv_obj_set_style_border_opa(input, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(input, 1, 0);
  lv_obj_set_style_border_color(input, lv_color_hex(theme_accent()), LV_STATE_FOCUSED);
  lv_obj_set_style_border_opa(input, LV_OPA_COVER, LV_STATE_FOCUSED);
  lv_obj_set_style_radius(input, 12, 0);
  lv_obj_set_style_pad_hor(input, 14, 0);
  lv_obj_set_style_pad_ver(input, 10, 0);
}

static void glass_keyboard_style(lv_obj_t *keyboard)
{
  lv_obj_set_style_bg_color(keyboard, lv_color_hex(theme_card()), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(keyboard, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(keyboard, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_all(keyboard, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(keyboard, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_column(keyboard, 5, LV_PART_MAIN);
  lv_obj_set_style_bg_color(keyboard, lv_color_hex(theme_surface()), LV_PART_ITEMS);
  lv_obj_set_style_text_color(keyboard, lv_color_hex(theme_primary()), LV_PART_ITEMS);
  lv_obj_set_style_radius(keyboard, 7, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(keyboard, 0, LV_PART_ITEMS);
  lv_obj_set_style_border_width(keyboard, 0, LV_PART_ITEMS);
}

#include "glass_netboot.inc"

#include "glass_wifi.inc"

#include "glass_files.inc"
#include "glass_resources.inc"

static void glass_storage_page(void *unused)
{
  glass_files_render(unused);
}

#ifdef CONFIG_SYSTEM_GLASS_BLE
static void glass_ble_devices_open(lv_event_t *event);
#include "glass_mouse.inc"
#include "glass_bluetooth.inc"
#endif

static void glass_bluetooth_page(void *unused)
{
  LV_UNUSED(unused);
  lv_obj_t *card = panel_card("蓝牙");
  if (card == NULL) return;
#ifdef CONFIG_SYSTEM_GLASS_BLE
  glass_ble_devices_page(card);
#else
  detail_page(card);
  detail_sidebar(card, "蓝牙", LV_SYMBOL_BLUETOOTH, panel_hide);
  detail_text(card, "蓝牙设备", 216, 26, 500, 28, detail_ink());
  lv_obj_t *status = detail_tile(card, 216, 104, 784, 160);
  detail_text(status, "蓝牙鼠标服务未启用", 24, 28, 720, 20, detail_ink());
  detail_text(status, "当前配置无法配对或发送鼠标操作", 24, 80, 720, 16, detail_muted());
#endif
}

static bool g_system_page_pending;
static unsigned int g_system_page_kind;
static void glass_system_page_async(void *unused);

static void glass_system_page_cancel(void)
{
  if (g_system_page_pending)
    lv_async_call_cancel(glass_system_page_async, NULL);
  g_system_page_pending = false;
}

static void glass_system_page_async(void *unused)
{
  LV_UNUSED(unused);
  if (!g_system_page_pending) return;
  g_system_page_pending = false;
  if (g_system_page_kind == 1) glass_storage_page(NULL);
  else if (g_system_page_kind == 2) glass_bluetooth_page(NULL);
  else if (g_system_page_kind == 3) glass_resources_page(NULL);
  else glass_wifi_page(NULL);
}

static void glass_system_page_open(lv_event_t *e)
{
  if (g_system_page_pending) return;
  uintptr_t kind = (uintptr_t)lv_event_get_user_data(e);
  if (kind > 3) return;
  g_system_page_kind = (unsigned int)kind;
  if (lv_async_call(glass_system_page_async, NULL) == LV_RESULT_OK)
    g_system_page_pending = true;
}

#include "glass_portal_ui.inc"

#include "glass_settings_ui.inc"
#include "glass_about_ui.inc"

static void glass_lock_open(lv_event_t *e);
#include "glass_music_cover_ui.inc"
#include "glass_music_ui.inc"
static void glass_homeassistant(lv_event_t *e)
{
  launch_builtin_homeassistant_qapp(e);
}
#include "glass_speech_ui.inc"
#include "glass_chat_ui.inc"

static void glass_music(lv_event_t *e)
{
  LV_UNUSED(e);
  home_music_obscure();
  bool return_apps=g_desktop.apps_page || (g_music_ui.root&&g_music_ui.return_apps) ||
    (portal_page&&portal_page==g_desktop.current_card&&portal_return==glass_music&&music_config_return_apps);
  lv_obj_t *card = panel_card("音乐空间");
  if (card) glass_music_app(card,return_apps);
}

static void glass_widget_open(lv_event_t *e)
{
  if (g_glass.widget == 2)
    {
      about_clicked(e);
    }
  else
    {
      glass_weather_open(e);
    }
}

static int g_apps_page_index,g_apps_page_count;
static lv_obj_t *g_apps_card,*g_apps_grid,*g_apps_pager,*g_apps_count;
static int g_apps_direction;
struct glass_app_slot
{
  lv_obj_t *tile,*badge,*icon,*name,*source,*remove;
  lv_event_cb_t callback;
  unsigned revision,pressed_revision;
  bool pressed;
};
static struct glass_app_slot g_apps_slots[9];
/* Rebinding a retained tile while a finger is down must never launch the
 * replacement application (or open its removal confirmation). */
static void glass_apps_press_guard(lv_event_t *e)
{
  struct glass_app_slot *slot=lv_event_get_user_data(e);
  lv_event_code_t code=lv_event_get_code(e);
  if(code==LV_EVENT_PRESSED){slot->pressed=true;slot->pressed_revision=slot->revision;}
  else if(code==LV_EVENT_CLICKED){
    bool stale=slot->pressed&&slot->pressed_revision!=slot->revision;
    slot->pressed=false;
    if(stale||lv_obj_has_flag(lv_event_get_target(e),LV_OBJ_FLAG_HIDDEN))
      lv_event_stop_processing(e);
  }
  else if(code==LV_EVENT_PRESS_LOST)slot->pressed=false;
}
static void glass_apps_deleted(lv_event_t *event)
{
  LV_UNUSED(event);g_apps_card=g_apps_grid=g_apps_pager=g_apps_count=NULL;
  g_apps_page_count=0;
  memset(g_apps_slots,0,sizeof(g_apps_slots));
}

static void glass_apps_render(void);
static struct qpk_entry_s g_apps_remove_target;
static lv_timer_t *g_apps_remove_timer;

static void glass_apps_remove_poll(lv_timer_t *timer)
{
  struct qpk_draft_info state; glass_qpk_get(&state);
  if (state.removing) return;
  g_apps_remove_timer = NULL; lv_timer_delete(timer);
  qpk_scan();
  if (g_desktop.apps_page) glass_apps_render();
  if (!state.remove_error) show_toast("应用和应用数据已删除");
  else { char message[112]; snprintf(message, sizeof(message), "删除未完成（%d），请重试", state.remove_error); show_toast(message); }
}

static void glass_apps_remove_confirm(lv_event_t *e)
{
  LV_UNUSED(e);
  if (g_apps_remove_timer) return;
  lv_timer_t *timer = lv_timer_create(glass_apps_remove_poll, 150, NULL);
  if (!timer) { show_toast("内存不足，请稍后重试"); return; }
  int ret = glass_qpk_remove_async_from(g_apps_remove_target.dir, g_apps_remove_target.package,g_apps_remove_target.volume);
  if (ret) { lv_timer_delete(timer); show_toast(ret == -EBUSY ? "正在处理应用，请稍后重试" : "无法删除这个应用"); return; }
  g_apps_remove_timer = timer;
  dialog_cancel();
  glass_apps_render();
  show_toast("正在删除应用和数据…");
}

static void glass_apps_remove_cancel(lv_event_t *e)
{ LV_UNUSED(e); dialog_cancel(); }

static void glass_apps_remove_open(lv_event_t *e)
{
  lv_event_stop_bubbling(e);
  int index = (int)(intptr_t)lv_event_get_user_data(e);
  if (index < 0 || index >= g_desktop.nqpk || g_apps_remove_timer) return;
  struct qpk_entry_s *entry = &g_desktop.qpk[index];
  if (!glass_qpk_removable(entry->dir, entry->package)) return;
  g_apps_remove_target = *entry;
  dialog_cancel();
  lv_obj_t *shade = glass_box(lv_layer_top(), 0, 0, 1024, 600, 0x000000, 0);
  g_desktop.dialog = shade;
  lv_obj_set_style_bg_opa(shade, LV_OPA_70, 0);
  lv_obj_add_flag(shade, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *box = glass_box(shade, 232, 140, 560, 320, theme_card(), 24);
  glass_text(box, "删除这个应用？", 28, 24, 504, 28, theme_primary());
  glass_text(box, entry->name, 28, 82, 504, 20, theme_primary());
  glass_text(box, "应用文件和应用数据将一并删除，无法恢复。", 28, 140, 504, 20, theme_secondary());
  glass_button(box, "取消", 28, 238, 238, theme_surface(), glass_apps_remove_cancel, NULL);
  glass_button(box, "删除应用", 282, 238, 250, theme_accent(), glass_apps_remove_confirm, NULL);
}
static void glass_apps_page(void *context,unsigned page)
{
  (void)context;g_apps_direction=(int)page>g_apps_page_index?1:-1;g_apps_page_index = (int)page;
  glass_apps_render();
}

static void glass_apps_swipe(lv_event_t *event)
{
  lv_indev_t *input=lv_indev_active();
  if(!input||lv_indev_get_type(input)!=LV_INDEV_TYPE_POINTER||
     !g_desktop.apps_page||g_desktop.current_card!=g_apps_card||g_desktop.dialog)return;
  lv_dir_t direction=lv_indev_get_gesture_dir(input);
  /* Consume the entire stroke, including at the first/last page and on a
   * delete button. Its release must never become an application click. */
  lv_indev_wait_release(input);
  lv_event_stop_bubbling(event);
  if(g_apps_remove_timer)return;
  int page=g_apps_page_index;
  if(direction==LV_DIR_LEFT)page++;
  else if(direction==LV_DIR_RIGHT)page--;
  if(page>=0&&page<g_apps_page_count&&page!=g_apps_page_index)
    glass_apps_page(NULL,(unsigned)page);
}

static void glass_apps(lv_event_t *e)
{
  LV_UNUSED(e);
  qpk_scan();
  g_apps_page_index = 0;g_apps_direction=0;
  glass_apps_render();
}

static void glass_apps_render(void)
{
  bool reuse=g_desktop.apps_page&&g_apps_card&&g_desktop.current_card==g_apps_card;
  lv_obj_t *card=reuse?g_apps_card:panel_card("全部应用");
  if(!card)return;
  g_desktop.apps_page=true;
  if(!reuse){
    g_apps_card=card;lv_obj_add_event_cb(card,glass_apps_deleted,LV_EVENT_DELETE,NULL);
    /* Descendant gestures stop here; screen-edge strips retain back navigation. */
    lv_obj_remove_flag(card,LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(card,glass_apps_swipe,LV_EVENT_GESTURE,NULL);
    g_apps_grid=glass_box(card,48,0,928,332,theme_card(),0);
    lv_obj_set_style_bg_opa(g_apps_grid,LV_OPA_TRANSP,0);
    g_apps_count=glass_text(card,"",28,64,868,16,theme_secondary());
    g_apps_pager=glass_pager_create(card,theme_surface(),theme_primary(),theme_secondary(),glass_apps_page,NULL);
    if(g_apps_pager)lv_obj_align(g_apps_pager,LV_ALIGN_BOTTOM_MID,0,-14);
  }
  lv_obj_t *grid=g_apps_grid;
  if(!g_apps_direction)glass_motion_cancel(grid);
  const char *names[] = {
#ifdef CONFIG_SYSTEM_BILIBILI_LIVE
"网络直播", "相册",
#endif
"ESPClaw AI 助手", "相机", "音乐", "录音机", "智能家居", "蓝牙鼠标", "Grok Ball", "2048", "你好快应用", "ESP-DL 离线识别", "大肥鱼桌宠"};
  const char *symbols[] = {
#ifdef CONFIG_SYSTEM_BILIBILI_LIVE
LV_SYMBOL_VIDEO, LV_SYMBOL_IMAGE,
#endif
GLASS_ICON_SPARKLES, GLASS_ICON_CAMERA, GLASS_ICON_HEADPHONES, GLASS_ICON_MIC, LV_SYMBOL_HOME, GLASS_ICON_MOUSE,
                           GLASS_ICON_FACE, LV_SYMBOL_KEYBOARD, LV_SYMBOL_EDIT, LV_SYMBOL_EYE_OPEN, GLASS_ICON_FISH};
  const char *descriptions[] = {
#ifdef CONFIG_SYSTEM_BILIBILI_LIVE
"观看在线直播", "浏览照片与视频",
#endif
"对话、创作与助手", "拍照与实时预览", "听音乐、看歌词", "录制与回放声音", "连接与控制设备", "连接电脑与控制指针",
"语音与趣味互动", "数字合成小游戏", "探索快应用", "本地模型识别", "陪伴你的桌面伙伴"};
  lv_event_cb_t callbacks[] = {
#ifdef CONFIG_SYSTEM_BILIBILI_LIVE
launch_bilibili, launch_local_video,
#endif
glass_chat_open, launch_builtin_camera_qapp, glass_music, launch_builtin_recorder_qapp, glass_homeassistant,
    glass_mouse_open, launch_builtin_ouo_qapp, launch_builtin_2048_qapp, launch_builtin_qapp, launch_builtin_espdl_qapp, launch_builtin_dafeiyu_qapp};
  const int builtin_count=sizeof(names)/sizeof(names[0]);
  const int total = builtin_count + g_desktop.nqpk;
  const int pages = (total + 8) / 9;
  g_apps_page_count=pages;
  if (g_apps_page_index < 0) g_apps_page_index = 0;
  if (g_apps_page_index >= pages) g_apps_page_index = pages - 1;
  const int first = g_apps_page_index * 9;
  lv_obj_update_layout(card);
  const int card_width=lv_obj_get_width(card),card_height=lv_obj_get_height(card);
  lv_obj_set_pos(grid,(card_width-928)/2,(96+card_height-80-332)/2);
  struct glass_app_palette palette=glass_app_palette(g_desktop.light_theme);
  for(unsigned i=0;i<9;i++){
    struct glass_app_slot *slot=&g_apps_slots[i];slot->revision++;
    if(slot->tile)lv_obj_add_flag(slot->tile,LV_OBJ_FLAG_HIDDEN);
  }
  for (int i = first; i < total && i < first + 9; i++)
    {
      int index = i - first;
      struct glass_app_slot *slot=&g_apps_slots[index];
      if(!slot->tile){
      slot->tile = glass_box(grid,0,0,296,100,theme_card(),GLASS_APP_CARD_RADIUS);
      glass_app_panel_style(slot->tile,theme_card(),glass_app_palette(g_desktop.light_theme).line);
      slot->badge=glass_box(slot->tile,16,25,48,48,theme_surface(),14);
      slot->icon=glass_symbol(slot->badge, "", 0, 0, theme_accent());
      lv_obj_remove_flag(slot->badge,LV_OBJ_FLAG_CLICKABLE);
      lv_obj_remove_flag(slot->icon,LV_OBJ_FLAG_CLICKABLE);
      slot->name=glass_text(slot->tile, "",
                 80, 18, 200, 20, theme_primary());
      slot->source=glass_text(slot->tile,"",80,57,200,16,theme_secondary());
      lv_obj_add_flag(slot->tile,LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_border_opa(slot->tile,160,LV_STATE_PRESSED);
      lv_obj_set_style_bg_color(slot->tile,lv_color_hex(theme_surface()),LV_STATE_PRESSED);
      lv_obj_set_style_border_color(slot->tile,lv_color_hex(theme_accent()),LV_STATE_PRESSED);
      glass_motion_button(slot->tile);
      lv_obj_add_event_cb(slot->tile,glass_apps_press_guard,LV_EVENT_ALL,slot);
      slot->remove=glass_button(slot->tile,"删除",212,50,68,theme_surface(),NULL,NULL);
      lv_obj_set_height(slot->remove,44);
      lv_obj_set_style_text_color(lv_obj_get_child(slot->remove,0),lv_color_hex(palette.danger),0);
      lv_obj_remove_flag(slot->remove,LV_OBJ_FLAG_EVENT_BUBBLE);
      lv_obj_add_event_cb(slot->remove,glass_apps_press_guard,LV_EVENT_ALL,slot);
      }
      lv_obj_t *tile=slot->tile;
      /* Retain the same reading order and positions on a partially filled page. */
      lv_obj_set_pos(tile,(index%3)*316,(index/3)*116);
      lv_label_set_text(slot->icon,i<builtin_count?symbols[i]:LV_SYMBOL_FILE);
      static const uint32_t light_ink[]={0x6c5b92,0x3e749b,0x39816a,0xa76c35,0xa25773,0x5a72a0};
      static const uint32_t dark_ink[]={0xc7b5e5,0x9bc8eb,0x93d3bb,0xedc18e,0xe5abc2,0xacbeec};
      uint32_t ink=(g_desktop.light_theme?light_ink:dark_ink)[i%6];
      lv_obj_set_style_bg_color(slot->badge,lv_color_mix(lv_color_hex(ink),lv_color_hex(palette.paper),g_desktop.light_theme?26:40),0);
      lv_obj_set_style_text_color(slot->icon,lv_color_hex(ink),0);
      lv_obj_center(slot->icon);
      lv_label_set_text(slot->name,i<builtin_count?names[i]:g_desktop.qpk[i-builtin_count].name);
      lv_label_set_text(slot->source,i<builtin_count?descriptions[i]:g_desktop.qpk[i-builtin_count].volume==QPK_VOLUME_SD?"SD 卡应用":"内置存储");
      if(slot->callback)lv_obj_remove_event_cb(tile,slot->callback);
      slot->callback=i<builtin_count?callbacks[i]:external_qapp_clicked;
      lv_obj_add_event_cb(tile,slot->callback,LV_EVENT_CLICKED,i<builtin_count?NULL:(void *)(intptr_t)(i-builtin_count));
      lv_obj_remove_event_cb(slot->remove,glass_apps_remove_open);
      bool removable=i>=builtin_count&&glass_qpk_removable(g_desktop.qpk[i-builtin_count].dir,g_desktop.qpk[i-builtin_count].package);
      lv_obj_set_width(slot->source,removable?124:200);
      if(removable){
        lv_obj_remove_flag(slot->remove,LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(slot->remove,glass_apps_remove_open,LV_EVENT_CLICKED,(void *)(intptr_t)(i-builtin_count));
      }else lv_obj_add_flag(slot->remove,LV_OBJ_FLAG_HIDDEN);
      if(g_apps_remove_timer)lv_obj_add_state(slot->remove,LV_STATE_DISABLED);
      else lv_obj_remove_state(slot->remove,LV_STATE_DISABLED);
      if(g_apps_remove_timer&&i>=builtin_count)lv_obj_add_state(tile,LV_STATE_DISABLED);
      else lv_obj_remove_state(tile,LV_STATE_DISABLED);
      lv_obj_remove_flag(tile,LV_OBJ_FLAG_HIDDEN);
    }
  char count_text[64];snprintf(count_text,sizeof(count_text),"共 %d 个应用 · 第 %d / %d 页",total,g_apps_page_index+1,pages);
  lv_label_set_text(g_apps_count,count_text);
  glass_pager_update(g_apps_pager,g_apps_page_index,pages,!g_apps_remove_timer);
  /* Page selection replaces content in place, without a floating translation. */
  g_apps_direction=0;
  if (g_desktop.qpk_scan_failed) show_toast("内存不足，部分应用暂未列出；请稍后重新打开");
}

#include "glass_home_ui.inc"
#include "glass_lockscreen.inc"
#include "glass_status_ui.inc"

static bool g_edge_back_pending;
static bool g_edge_tracking;
static lv_point_t g_edge_start;
static lv_obj_t *g_edge_page;
static void glass_edge_back_async(void *data);
static lv_obj_t *g_edge_strip;
static lv_style_value_t g_edge_saved;
static bool g_edge_had_offset;
static int g_edge_offset;
static void glass_edge_deleted(lv_event_t *event)
{
  if(g_edge_page==lv_event_get_target(event)){
    lv_async_call_cancel(glass_edge_back_async,NULL);
    g_edge_page=NULL;g_edge_tracking=false;g_edge_back_pending=false;g_edge_offset=0;
  }
}
static void glass_edge_restore(bool animate)
{
  LV_UNUSED(animate);
  if(!g_edge_page||g_edge_page!=g_desktop.current_card)return;
  if(g_edge_had_offset)lv_obj_set_local_style_prop(g_edge_page,LV_STYLE_TRANSLATE_X,g_edge_saved,0);
  else lv_obj_remove_local_style_prop(g_edge_page,LV_STYLE_TRANSLATE_X,0);
  /* Gesture cancellation restores position immediately. */
  g_edge_offset=0;
}


static void glass_edge_back_async(void *data)
{
  LV_UNUSED(data);
  g_edge_back_pending = false;
  /* Ignore a gesture if another action changed the page before dispatch. */
  if (g_lock_ui.root || g_edge_page != g_desktop.current_card) return;
  if(g_music_ui.full){music_full_close(NULL);return;}
  if (g_desktop.dialog != NULL)
    dialog_cancel();
  else if (!lv_obj_has_flag(g_desktop.panel, LV_OBJ_FLAG_HIDDEN))
    {
      if (g_desktop.chat_page) glass_chat_back(NULL);
      else if (portal_page == g_desktop.current_card) portal_go_back(NULL);
      else if (g_desktop.qapp_page) qapp_back(NULL);
      else if (g_music_ui.root==g_desktop.current_card) music_ui_back(NULL);
      else panel_hide(NULL);
    }
}

static void glass_edge_event(lv_event_t *e)
{
  lv_indev_t *input = lv_indev_active();
  if (input == NULL) return;
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *strip = lv_event_get_target(e);
  if (code == LV_EVENT_PRESSED)
    {
      if(g_lock_ui.root||!g_desktop.current_card||g_edge_back_pending)return;
      glass_edge_restore(false);
      glass_motion_cancel(g_desktop.current_card);
      lv_indev_get_point(input, &g_edge_start);
      g_edge_tracking = true;
      g_edge_strip = strip;
      g_edge_page = g_desktop.current_card;g_edge_offset=0;
      g_edge_had_offset=lv_obj_get_local_style_prop(g_edge_page,LV_STYLE_TRANSLATE_X,&g_edge_saved,0)==LV_STYLE_RES_FOUND;
      bool watched=false;
      for(unsigned i=0;i<lv_obj_get_event_count(g_edge_page);i++)
        if(lv_event_dsc_get_cb(lv_obj_get_event_dsc(g_edge_page,i))==glass_edge_deleted)watched=true;
      if(!watched)lv_obj_add_event_cb(g_edge_page,glass_edge_deleted,LV_EVENT_DELETE,NULL);
    }
  else if (code == LV_EVENT_PRESSING && g_edge_tracking && strip == g_edge_strip)
    {
      lv_point_t point;
      lv_indev_get_point(input, &point);
      int dx = point.x - g_edge_start.x;
      int dy = abs(point.y - g_edge_start.y);
      bool right = (uintptr_t)lv_event_get_user_data(e) != 0;
      int inward = right ? -dx : dx;
      if(g_edge_page!=g_desktop.current_card){g_edge_tracking=false;return;}
      if (dy > 48){g_edge_tracking=false;glass_edge_restore(true);}
      else if (inward >= 72 && inward > 2 * dy)
        {
          g_edge_tracking = false;
          lv_indev_wait_release(input);
          glass_edge_restore(false);
          /* Delete after pointer dispatch, without an exit-animation delay. */
          if(!g_edge_back_pending&&lv_async_call(glass_edge_back_async,NULL)==LV_RESULT_OK)g_edge_back_pending=true;
        }
      else if(!g_motion_reduced&&!g_desktop.dialog){
        int distance=inward<0?0:inward/4;if(distance>24)distance=24;
        g_edge_offset=right?-distance:distance;
        lv_obj_set_style_translate_x(g_edge_page,(g_edge_had_offset?g_edge_saved.num:0)+g_edge_offset,0);
      }
    }
  else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST){
    if(g_edge_tracking)glass_edge_restore(true);
    g_edge_tracking=false;
  }
}

static void glass_edge_back_create(void)
{
  for (int side = 0; side < 2; side++)
    {
      lv_obj_t *strip = lv_obj_create(lv_layer_top());
      lv_obj_remove_style_all(strip);
      lv_obj_set_pos(strip, side ? 1004 : 0, 0);
      lv_obj_set_size(strip, 20, 600);
      lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_remove_flag(strip, LV_OBJ_FLAG_GESTURE_BUBBLE);
      lv_obj_add_flag(strip, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
      lv_obj_add_event_cb(strip, glass_edge_event, LV_EVENT_ALL, (void *)(uintptr_t)side);
    }
}

static int pet_fill_weather(char *buf, size_t size)
{
  struct glass_weather_snapshot weather;

  if (buf == NULL || size == 0)
    {
      return -1;
    }

  glass_weather_get(&weather);
  if (weather.valid && weather.state == WEATHER_READY)
    {
      snprintf(buf, size, "%s今天%d°，%s",
               weather.data.location[0] ? weather.data.location : "这里",
               weather.data.temperature,
               weather.data.condition[0] ? weather.data.condition : "不明");
      return 0;
    }

  if (weather.state == WEATHER_FETCHING ||
      weather.state == WEATHER_WAIT_NET ||
      weather.state == WEATHER_WAIT_TIME)
    {
      snprintf(buf, size, "天气还在路上，先摸会鱼");
      return 0;
    }

  snprintf(buf, size, "今天适合摸鱼，别问温度。");
  return 0;
}

static void desktop_ui_create(void)
{
  size_t size = (size_t)(g_desktop_font_end - g_desktop_font_start);
  memset(&g_desktop, 0, sizeof(g_desktop));
  memset(&g_glass, 0, sizeof(g_glass));
  g_glass.temperature_c = -999;
  g_glass.humidity_pct = -1;
  g_glass.pm25 = -1;
  g_glass.palette = 0;
  g_desktop.light_theme = true;
  uint8_t preferences[4] = {1, 0, 0, 0};
  if (glass_settings_load(preferences) == 0)
    {
      g_desktop.light_theme = preferences[0];
      g_glass.palette = preferences[1];
      g_glass.widget = preferences[2];
      g_glass.reduced_motion = preferences[3];
    }
  glass_wallpaper_load();
  portal_desktop_publish(preferences);
#ifdef CONFIG_SYSTEM_HASS
  int hass_bootstrap = hass_portal_bootstrap();
  if (hass_bootstrap < 0)
    printf("desktop: HA service configuration unavailable (%d)\n",
           -hass_bootstrap);
#endif
  g_desktop.font16 = lv_tiny_ttf_create_data_ex(g_desktop_font_start, size,
    16, LV_FONT_KERNING_NORMAL, DESKTOP_FONT_CACHE_GLYPHS);
  g_desktop.font20 = lv_tiny_ttf_create_data_ex(g_desktop_font_start, size,
    20, LV_FONT_KERNING_NORMAL, DESKTOP_FONT_CACHE_GLYPHS);
  g_desktop.font28 = lv_tiny_ttf_create_data_ex(g_desktop_font_start, size,
    28, LV_FONT_KERNING_NORMAL, DESKTOP_FONT_CACHE_GLYPHS);
  g_glass.font72 = lv_tiny_ttf_create_data_ex(g_desktop_font_start, size,
    128, LV_FONT_KERNING_NORMAL, 24);
  g_glass.font48 = lv_tiny_ttf_create_data_ex(g_desktop_font_start, size,
    48, LV_FONT_KERNING_NORMAL, 24);
  /* The home temperature face above is 128 px despite its historical name.
   * Lyrics need their own actual 72 px face for measurement and clipping. */
  g_glass.music_font72 = lv_tiny_ttf_create_data_ex(g_desktop_font_start, size,
    72, LV_FONT_KERNING_NORMAL, 24);
  glass_home_fonts_init(g_desktop_font_start, size);
  if (g_desktop.font16) g_desktop.font16->fallback = glass_icon_font(16);
  if (g_desktop.font20) g_desktop.font20->fallback = glass_icon_font(20);
  if (g_desktop.font28) g_desktop.font28->fallback = glass_icon_font(24);
  glass_sd_font_start();
  g_desktop.screen = glass_box(lv_screen_active(), 0, GLASS_STATUS_HEIGHT,
                                1024, GLASS_CONTENT_HEIGHT, theme_card(), 0);
  lv_obj_set_style_text_font(g_desktop.screen, zh_font(20), 0);
  lv_obj_remove_flag(g_desktop.screen, LV_OBJ_FLAG_SCROLLABLE);
  qpk_scan();
  g_desktop.panel = lv_obj_create(g_desktop.screen);
  lv_obj_set_pos(g_desktop.panel, 0, 0);
  lv_obj_set_size(g_desktop.panel, 1024, GLASS_CONTENT_HEIGHT);
  lv_obj_set_style_pad_all(g_desktop.panel, 0, 0);
  lv_obj_set_style_border_width(g_desktop.panel, 0, 0);
  lv_obj_set_style_radius(g_desktop.panel, 0, 0);
  lv_obj_remove_flag(g_desktop.panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(g_desktop.panel, LV_OBJ_FLAG_HIDDEN);
  glass_rebuild();
  glass_edge_back_create();
  glass_status_create();
  g_glass.timer = lv_timer_create(glass_update, 1000, NULL);
  pet_engine_init(lv_obj_get_width(g_desktop.screen),
                  lv_obj_get_height(g_desktop.screen));
  pet_engine_set_weather_cb(pet_fill_weather);
  if (pet_lvgl_create(g_desktop.screen, zh_font(16)) < 0)
    {
      printf("desktop: pet overlay skipped\n");
    }
}
