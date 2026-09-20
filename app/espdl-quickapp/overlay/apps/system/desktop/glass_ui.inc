/* SPDX-License-Identifier: Apache-2.0
 * Glass desktop for the 355ef31 camera baseline, 1024 x 600.
 * 中文说明：仅重建桌面容器，应用页和相机运行时的生命周期仍由原桌面管理。
 * 渐变和半透明填充模拟磨砂层；不进行全屏模糊或连续背景重绘。
 */

#define GLASS_HISTORY 24
/* Shown clock offset used only while this build has no zoneinfo database. */
#define GLASS_CLOCK_OFFSET_SECONDS (8 * 3600)
#include <limits.h>
#include "glass_wallpaper.h"
#include "glass_music_assets.inc"
#include "glass_settings_store.h"
#include "glass_settings_service.h"
#include "glass_weather.h"
#include "glass_ime.h"
#define GLASS_SIMPLE_SETTINGS 1
#if defined(CONFIG_ESPRESSIF_LEDC_TIMER0) && \
    CONFIG_ESPRESSIF_LEDC_CHANNEL0_PIN == 26 && CONFIG_PWM_NCHANNELS == 1
#include <fcntl.h>
#include <sys/ioctl.h>
#include <nuttx/timers/pwm.h>
#define GLASS_PWM_BACKLIGHT 1
static int g_backlight_fd = -1;
#endif

static int g_screen_brightness = 100;

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
  lv_obj_t *clock;
  lv_obj_t *date;
  lv_obj_t *temperature;
  lv_obj_t *humidity;
  lv_obj_t *air;
  lv_obj_t *devices;
  lv_obj_t *sync;
  lv_obj_t *widget_title;
  lv_obj_t *widget_value;
  lv_obj_t *widget_hint;
  lv_obj_t *widget_badge;
  lv_obj_t *widget_category;
  lv_obj_t *chart;
  lv_chart_series_t *series;
  lv_obj_t *chart_hint;
  lv_obj_t *theme_button;
  lv_obj_t *wifi_button;
  lv_obj_t *settings_switch;
  lv_obj_t *widget_dropdown;
  lv_obj_t *home_cards[4];
  lv_obj_t *motion_switch;
  lv_obj_t *brightness_slider;
  lv_font_t *font72;
  lv_font_t *font48;
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
  unsigned int palette;
  unsigned int widget;
} g_glass;

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
  int available;
  int total;
  uint8_t hour;
  uint8_t minute;
  uint8_t month;
  uint8_t day;
  uint8_t weekday;
  uint8_t widget;
  uint8_t sync_unit;
  bool has_sample;
  bool clock_valid;
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

static uint32_t glass_tint(void)
{
  static const uint32_t light[] = {0x6358a0, 0xc64d36, 0x237f73};
  static const uint32_t dark[] = {0x413461, 0x963c35, 0x215c58};
  return g_desktop.light_theme ? light[g_glass.palette] : dark[g_glass.palette];
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
  lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
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

/* Labels have explicit text colors, so disabling only the parent does not
 * make a pager visibly inactive. Set both surfaces and label colors. */
static void glass_pager_enabled(lv_obj_t *button, bool enabled, uint32_t text_color)
{
  if (!button) return;
  uint32_t background = g_desktop.light_theme ? 0xe4e5e8 : 0x35363d;
  uint32_t foreground = g_desktop.light_theme ? 0x9598a1 : 0x777a84;
  lv_obj_set_style_bg_color(button, lv_color_hex(background), LV_STATE_DISABLED);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_DISABLED);
  lv_obj_set_style_border_opa(button, LV_OPA_TRANSP, LV_STATE_DISABLED);
  if (enabled) lv_obj_remove_state(button, LV_STATE_DISABLED);
  else lv_obj_add_state(button, LV_STATE_DISABLED);
  for (uint32_t i = 0; i < lv_obj_get_child_count(button); i++) {
    lv_obj_t *label = lv_obj_get_child(button, i);
    if (lv_obj_check_type(label, &lv_label_class))
      lv_obj_set_style_text_color(label, lv_color_hex(enabled ? text_color : foreground), 0);
  }
}

static void glass_clickable(lv_obj_t *card, lv_event_cb_t callback, void *data)
{
  lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_border_opa(card, 160, LV_STATE_PRESSED);
  lv_obj_add_event_cb(card, callback, LV_EVENT_CLICKED, data);
}

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
  for (i = 0; i < GLASS_HISTORY; i++)
    {
      if (i < g_glass.samples)
        {
          if (g_glass.history[i] < low) low = g_glass.history[i] - 5;
          if (g_glass.history[i] > high) high = g_glass.history[i] + 5;
        }
      lv_chart_set_value_by_id(g_glass.chart, g_glass.series, i,
        i < g_glass.samples ? g_glass.history[i] : LV_CHART_POINT_NONE);
    }
  lv_chart_set_range(g_glass.chart, LV_CHART_AXIS_PRIMARY_Y, low, high);
  if (g_glass.samples == 0)
    {
      lv_label_set_text(g_glass.chart_hint, "暂无温度记录 · 在智能家居中连接传感器");
    }
  else
    {
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

static void glass_update(lv_timer_t *timer)
{
  uint8_t portal_values[4];
  if(portal_desktop_take(portal_values)) {
    g_desktop.light_theme=portal_values[0]; g_glass.palette=portal_values[1];
    g_glass.widget=portal_values[2]; g_glass.reduced_motion=portal_values[3];
    glass_rebuild(); glass_save_preferences();
  }
  glass_preferences_update();
  struct timespec uptime = {0};
  struct tm local = {0};
  time_t now = time(NULL);
  char buffer[80];
  struct glass_weather_snapshot weather;
  glass_weather_get(&weather);
  bool initial = !g_glass_display.valid;
  LV_UNUSED(timer);
#ifndef CONFIG_LIBC_LOCALTIME
  /* This build has no zoneinfo database, so localtime_r() returns UTC. Only
   * the displayed clock is shifted; CLOCK_REALTIME stays UTC for TLS. */
  now += GLASS_CLOCK_OFFSET_SECONDS;
#endif
  if (g_glass.home == NULL)
    {
      return;
    }
  clock_gettime(CLOCK_MONOTONIC, &uptime);
  uint64_t weather_now = (uint64_t)uptime.tv_sec * 1000 + uptime.tv_nsec / 1000000;
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
      .weather_stale = weather.valid && weather_now - weather.received_ms >= 7200000,
      .available = g_glass.available,
      .total = g_glass.total,
      .widget = g_glass.widget < 2 ? g_glass.widget : 2,
      .has_sample = g_glass.has_sample,
      .clock_valid = localtime_r(&now, &local) != NULL && local.tm_year >= 120,
      .valid = true,
    };
  if (display.clock_valid)
    {
      display.hour = local.tm_hour;
      display.minute = local.tm_min;
      display.month = local.tm_mon;
      display.day = local.tm_mday;
      display.weekday = local.tm_wday;
    }
  bool clock_changed = display.clock_valid != g_glass_display.clock_valid;
  if (initial || clock_changed ||
      (display.clock_valid &&
       (display.hour != g_glass_display.hour ||
        display.minute != g_glass_display.minute)))
    {
      if (display.clock_valid)
        {
          strftime(buffer, sizeof(buffer), "%H:%M", &local);
          lv_label_set_text(g_glass.clock, buffer);
        }
      else
        lv_label_set_text(g_glass.clock, "--:--");
    }
  if (initial || clock_changed ||
      (display.clock_valid &&
       (display.month != g_glass_display.month ||
        display.day != g_glass_display.day ||
        display.weekday != g_glass_display.weekday)) ||
      (!display.clock_valid &&
       display.uptime_minutes != g_glass_display.uptime_minutes))
    {
      if (display.clock_valid)
        {
          static const char *week[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
          snprintf(buffer, sizeof(buffer), "%d 月 %d 日    %s",
                   local.tm_mon + 1, local.tm_mday, week[local.tm_wday]);
        }
      else
        snprintf(buffer, sizeof(buffer), "时钟待校准 · 已运行 %lu 分钟",
                 display.uptime_minutes);
      lv_label_set_text(g_glass.date, buffer);
    }
  if (initial || display.temperature != g_glass_display.temperature)
    {
      if (display.temperature != -999)
        lv_label_set_text_fmt(g_glass.temperature, "%d C", display.temperature);
      else
        lv_label_set_text(g_glass.temperature, "-- C");
    }
  if (initial || display.humidity != g_glass_display.humidity)
    {
      if (display.humidity >= 0)
        lv_label_set_text_fmt(g_glass.humidity, "湿度 %d%%", display.humidity);
      else
        lv_label_set_text(g_glass.humidity, "湿度 --%");
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
          lv_label_set_text_fmt(g_glass.sync, "上次同步 %lu %s前 · 可用 / 全部实体",
                                (unsigned long)display.sync_value,
                                units[display.sync_unit]);
        }
      else
        lv_label_set_text(g_glass.sync, "尚未同步 · 点击连接 Home Assistant");
    }
  bool widget_changed = initial || display.widget != g_glass_display.widget;
  bool weather_changed = widget_changed ||
    display.weather_revision != g_glass_display.weather_revision ||
    display.weather_stale != g_glass_display.weather_stale;
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
          lv_label_set_text(g_glass.air, "AQI 指数");
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
            lv_label_set_text_fmt(g_glass.widget_value, "%d%%", display.humidity);
          else
            lv_label_set_text(g_glass.widget_value, "--%");
        }
      if (widget_changed)
        {
          lv_label_set_text(g_glass.air, "相对湿度");
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
          lv_label_set_text(g_glass.air, "运行时间");
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
  glass_weather_detail_update(&weather, display.weather_stale);
  g_glass_display = display;
}

static void glass_theme_toggle(lv_event_t *e)
{
  LV_UNUSED(e);
  g_desktop.light_theme = !g_desktop.light_theme;
  glass_rebuild();
  glass_save_preferences();
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
  glass_save_preferences();
  if (g_glass.reduced_motion)
    {
      lv_anim_delete(g_glass.home, NULL);
      lv_obj_set_style_opa(g_glass.home, LV_OPA_COVER, 0);
      for (int i = 0; i < 4; i++)
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

static void glass_control_outside(lv_event_t *e)
{
  /* Settings is a page: background clicks never dismiss it. */
  LV_UNUSED(e);
}

static void glass_settings_back(lv_event_t *e)
{
  LV_UNUSED(e);
  glass_control_close_request();
}

static void glass_control_slide(void *object, int32_t y)
{
  lv_obj_set_y(object, y);
}

static void glass_control_gesture(lv_event_t *e)
{
  if (GLASS_SIMPLE_SETTINGS) return;
  lv_indev_t *input = lv_indev_active();
  if (input == NULL) return;
  lv_dir_t direction = lv_indev_get_gesture_dir(input);
  bool closed = lv_obj_has_flag(g_desktop.panel, LV_OBJ_FLAG_HIDDEN);
  if (closed && direction == LV_DIR_BOTTOM)
    {
      lv_indev_wait_release(input);
      if (!g_desktop.settings_rebuild_pending &&
          lv_async_call(settings_rebuild_async, NULL) == LV_RESULT_OK)
        g_desktop.settings_rebuild_pending = true;
    }
  else if (!closed && direction == LV_DIR_TOP)
    {
      lv_indev_wait_release(input);
      glass_control_close_request();
    }
  LV_UNUSED(e);
}

static void glass_control_switch_style(lv_obj_t *sw)
{
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

static bool g_control_preferences;
static void glass_settings(lv_event_t *e);

static void glass_preferences_open(lv_event_t *e)
{
  LV_UNUSED(e);
  g_control_preferences = true;
  if (!g_desktop.settings_rebuild_pending &&
      lv_async_call(settings_rebuild_async, NULL) == LV_RESULT_OK)
    g_desktop.settings_rebuild_pending = true;
}

static void glass_control_return(lv_event_t *e)
{
  LV_UNUSED(e);
  g_control_preferences = false;
  if (!g_desktop.settings_rebuild_pending &&
      lv_async_call(settings_rebuild_async, NULL) == LV_RESULT_OK)
    g_desktop.settings_rebuild_pending = true;
}

static void glass_icon_hint(lv_event_t *e)
{
  show_toast((const char *)lv_event_get_user_data(e));
}

static lv_obj_t *glass_control_icon(lv_obj_t *parent, const char *symbol,
                                   int x, int y, const char *hint,
                                   lv_event_cb_t action)
{
  lv_obj_t *button = glass_button(parent, "", x, y, 64, theme_surface(), action, NULL);
  lv_obj_set_size(button, 64, 56);
  lv_obj_set_style_radius(button, 16, 0);
  lv_obj_t *icon = glass_symbol(button, symbol, 0, 0, theme_primary());
  lv_obj_center(icon);
  lv_obj_add_event_cb(button, glass_icon_hint, LV_EVENT_LONG_PRESSED, (void *)hint);
  lv_obj_add_event_cb(button, glass_icon_hint, LV_EVENT_HOVER_OVER, (void *)hint);
  return button;
}

/* Shared form styling includes the placeholder, which otherwise uses
 * LVGL's Latin-only theme font. Keep keyboard symbols in Montserrat. */
static void glass_input_style(lv_obj_t *input)
{
  lv_obj_set_style_text_font(input, zh_font(20), LV_PART_MAIN);
  lv_obj_set_style_text_font(input, zh_font(20), LV_PART_TEXTAREA_PLACEHOLDER);
  lv_obj_set_style_text_color(input, lv_color_hex(theme_primary()), LV_PART_MAIN);
  lv_obj_set_style_text_color(input, lv_color_hex(theme_secondary()), LV_PART_TEXTAREA_PLACEHOLDER);
  lv_obj_set_style_bg_color(input, lv_color_hex(theme_surface()), 0);
  lv_obj_set_style_bg_opa(input, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(input, lv_color_hex(theme_secondary()), 0);
  lv_obj_set_style_border_opa(input, 55, 0);
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
#include "glass_mouse.inc"
#endif

static void glass_bluetooth_page(void *unused)
{
  LV_UNUSED(unused);
  lv_obj_t *card = panel_card("蓝牙");
  if (card == NULL) return;
#ifdef CONFIG_SYSTEM_GLASS_BLE
  glass_mouse_page(card);
#else
#if defined(CONFIG_SYSTEM_C6BLE_V3_EXPERIMENTAL) && defined(CONFIG_NIMBLE)
  glass_text(card, "BLE 协议栈已编译", 24, 92, 850, 28, theme_primary());
#else
  glass_text(card, "蓝牙服务未启用", 24, 92, 850, 28, theme_primary());
#endif
  glass_text(card, "ESP32-C6 固件状态：未检测", 24, 150, 850, 20, theme_secondary());
  glass_text(card, "BLE 扫描与连接暂不可用", 24, 192, 850, 20, theme_secondary());
#endif
}

static bool g_system_page_pending;
static unsigned int g_system_page_kind;

static void glass_system_page_async(void *unused)
{
  LV_UNUSED(unused);
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

static void glass_settings(lv_event_t *e)
{
  LV_UNUSED(e);
  glass_preferences_update();
  g_control_preferences = true;
  lv_obj_t *card = panel_card("控制面板");
  if (!card) return;
  g_control_card = card;
  lv_obj_add_event_cb(card, glass_control_deleted, LV_EVENT_DELETE, NULL);
  lv_obj_clean(card);
  lv_obj_remove_style_all(card);
  lv_obj_set_pos(card, 0, 0);
  lv_obj_set_size(card, 1024, 600);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(card, lv_color_hex(g_desktop.light_theme ? 0xf3f4f8 : 0x191d28), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_remove_event_cb(g_desktop.panel, glass_control_outside);

  glass_text(card, "控制面板", 40, 28, 240, 28, theme_primary());
  g_preferences_status = glass_text(card, glass_preferences_status_text(),
                                    298, 36, 460, 16, theme_secondary());
  g_preferences_shown = glass_preferences_display_state();
  lv_obj_add_event_cb(g_preferences_status, glass_preferences_status_deleted, LV_EVENT_DELETE, NULL);
  glass_button(card, "返回桌面", 864, 24, 120, theme_surface(), glass_settings_back, NULL);

  lv_obj_t *display = glass_box(card, 40, 96, 544, 208, theme_card(), 20);
  glass_text(display, "屏幕显示", 24, 20, 280, 20, theme_primary());
  glass_text(display, "亮度", 24, 62, 200, 16, theme_secondary());
  lv_obj_t *value = glass_text(display, "100%", 404, 60, 112, 20, theme_primary());
  lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_t *brightness = lv_slider_create(display);
  g_glass.brightness_slider = brightness;
  lv_obj_set_pos(brightness, 40, 110);
  lv_obj_set_size(brightness, 464, 8);
  lv_obj_set_ext_click_area(brightness, 18);
  lv_obj_set_style_radius(brightness, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_radius(brightness, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(brightness, lv_color_hex(theme_surface()), LV_PART_MAIN);
  lv_obj_set_style_bg_color(brightness, lv_color_hex(theme_accent()), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(brightness, lv_color_hex(0xffffff), LV_PART_KNOB);
  lv_obj_set_style_pad_all(brightness, 8, LV_PART_KNOB);
  lv_obj_set_style_radius(brightness, LV_RADIUS_CIRCLE, LV_PART_KNOB);
  lv_obj_set_style_border_width(brightness, 2, LV_PART_KNOB);
  lv_obj_set_style_border_color(brightness, lv_color_hex(theme_accent()), LV_PART_KNOB);
  lv_obj_set_style_shadow_width(brightness, 0, LV_PART_KNOB);
  lv_slider_set_range(brightness, 10, 100);
  lv_slider_set_value(brightness, g_screen_brightness, LV_ANIM_OFF);
#ifdef GLASS_PWM_BACKLIGHT
  lv_label_set_text_fmt(value, "%d%%", g_screen_brightness);
  lv_obj_add_event_cb(brightness, glass_brightness_changed, LV_EVENT_RELEASED, value);
  glass_text(display, "拖动滑块调整屏幕亮度", 24, 158, 496, 16, theme_secondary());
#else
  lv_obj_add_state(brightness, LV_STATE_DISABLED);
  lv_obj_set_style_bg_color(brightness, lv_color_hex(g_desktop.light_theme ? 0xaab2c5 : 0x64718a),
                            LV_PART_INDICATOR | LV_STATE_DISABLED);
  lv_obj_set_style_border_color(brightness, lv_color_hex(theme_secondary()),
                                LV_PART_KNOB | LV_STATE_DISABLED);
  glass_text(display, "当前屏幕使用固定亮度", 24, 158, 496, 16, theme_secondary());
#endif

  lv_obj_t *appearance = glass_box(card, 40, 324, 544, 236, theme_card(), 20);
  glass_text(appearance, "个性化", 24, 20, 180, 20, theme_primary());
  glass_text(appearance, "浅色外观", 24, 70, 160, 20, theme_primary());
  lv_obj_t *sw = lv_switch_create(appearance);
  g_glass.settings_switch = sw;
  lv_obj_set_pos(sw, 200, 64); lv_obj_set_size(sw, 60, 34);
  glass_control_switch_style(sw);
  if (g_desktop.light_theme) lv_obj_add_state(sw, LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw, theme_changed, LV_EVENT_VALUE_CHANGED, NULL);
  glass_text(appearance, "减少动效", 292, 70, 140, 20, theme_primary());
  sw = lv_switch_create(appearance);
  g_glass.motion_switch = sw;
  lv_obj_set_pos(sw, 460, 64); lv_obj_set_size(sw, 60, 34);
  glass_control_switch_style(sw);
  if (g_glass.reduced_motion) lv_obj_add_state(sw, LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw, glass_motion_changed, LV_EVENT_VALUE_CHANGED, NULL);
  glass_text(appearance, "主视觉配色", 24, 124, 160, 16, theme_secondary());
  const uint32_t colors[] = {0x6c619e, 0xb77860, 0x498d82};
  for (unsigned i = 0; i < 3; i++)
    {
      lv_obj_t *choice = glass_button(appearance, "", 216 + i * 104, 112, 88,
                                      colors[i], glass_palette_select, (void *)(uintptr_t)i);
      lv_obj_set_style_border_width(choice, g_glass.palette == i ? 2 : 0, 0);
      lv_obj_set_style_border_color(choice, lv_color_hex(0xffffff), 0);
      lv_obj_set_style_border_opa(choice, LV_OPA_COVER, 0);
      if (g_glass.palette == i)
        {
          lv_obj_t *check = glass_symbol(choice, LV_SYMBOL_OK, 0, 0, 0xffffff);
          lv_obj_center(check);
        }
    }
  glass_text(appearance, "桌面小组件", 24, 184, 176, 16, theme_secondary());
  lv_obj_t *dropdown = lv_dropdown_create(appearance);
  g_glass.widget_dropdown = dropdown;
  lv_dropdown_set_options(dropdown, "空气质量\n环境湿度\n系统运行");
  lv_dropdown_set_selected(dropdown, g_glass.widget);
  lv_obj_set_pos(dropdown, 216, 174); lv_obj_set_size(dropdown, 304, 44);
  glass_input_style(dropdown);
  lv_obj_set_style_text_font(dropdown, &lv_font_montserrat_24, LV_PART_INDICATOR);
  lv_obj_t *list = lv_dropdown_get_list(dropdown);
  lv_obj_set_style_text_font(list, zh_font(20), 0);
  lv_obj_set_style_text_color(list, lv_color_hex(theme_primary()), 0);
  lv_obj_set_style_bg_color(list, lv_color_hex(theme_card()), 0);
  lv_obj_set_style_radius(list, 12, 0);
  lv_obj_add_event_cb(dropdown, glass_widget_select, LV_EVENT_VALUE_CHANGED, NULL);

  lv_obj_t *connectivity = glass_box(card, 608, 96, 376, 208, theme_card(), 20);
  glass_text(connectivity, "网络与连接", 24, 20, 328, 20, theme_primary());
  glass_button(connectivity, "Wi-Fi 网络", 24, 70, 328, theme_surface(), glass_system_page_open, NULL);
  glass_button(connectivity, "蓝牙设备", 24, 136, 328, theme_surface(), glass_system_page_open, (void *)2);
  lv_obj_t *system = glass_box(card, 608, 324, 376, 236, theme_card(), 20);
  glass_text(system, "系统管理", 24, 20, 328, 20, theme_primary());
  glass_button(system, "存储与文件", 24, 70, 328, theme_surface(), glass_system_page_open, (void *)1);
  glass_button(system, "系统性能", 24, 136, 154, theme_surface(), glass_system_page_open, (void *)3);
  glass_button(system, "扫码配置", 194, 136, 158, theme_surface(), glass_portal_open, NULL);
  glass_text(system, "扫码设置、传文件、安装应用", 24, 202, 328, 16, theme_secondary());
}

#include "glass_music_ui.inc"
#ifdef CONFIG_SYSTEM_HASS
#include "glass_hass_ui.inc"
#else
static void glass_homeassistant(lv_event_t *e)
{
  launch_builtin_homeassistant_qapp(e);
}
#endif
#include "glass_chat_ui.inc"

static void glass_music(lv_event_t *e)
{
  LV_UNUSED(e);
  lv_obj_t *card = panel_card("音乐空间");
  if (card) glass_music_app(card);
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

static int g_apps_page_index;
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
  int ret = glass_qpk_remove_async(g_apps_remove_target.dir, g_apps_remove_target.package);
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
static void glass_apps_page(lv_event_t *e)
{
  g_apps_page_index += (int)(intptr_t)lv_event_get_user_data(e);
  glass_apps_render();
}

static void glass_apps(lv_event_t *e)
{
  LV_UNUSED(e);
  qpk_scan();
  g_apps_page_index = 0;
  glass_apps_render();
}

static void glass_apps_render(void)
{
  lv_obj_t *card = panel_card("全部应用");
  if (card == NULL) return;
  g_desktop.apps_page = true;
  lv_obj_t *grid = glass_box(card, 20, 80, 880, 336, theme_card(), 0);
  const char *names[] = {"ESPClaw AI 助手", "相机", "录音机", "智能家居", "蓝牙鼠标", "OuO", "2048", "你好快应用", "ESP-DL 离线识别", "大肥鱼桌宠"};
  const char *symbols[] = {LV_SYMBOL_EDIT, LV_SYMBOL_IMAGE, LV_SYMBOL_AUDIO, LV_SYMBOL_HOME, LV_SYMBOL_GPS,
                           LV_SYMBOL_EYE_OPEN, LV_SYMBOL_KEYBOARD, LV_SYMBOL_EDIT, LV_SYMBOL_EYE_OPEN, LV_SYMBOL_PLAY};
  lv_event_cb_t callbacks[] = {glass_chat_open, launch_builtin_camera_qapp, launch_builtin_recorder_qapp, glass_homeassistant,
    glass_mouse_open, launch_builtin_ouo_qapp, launch_builtin_2048_qapp, launch_builtin_qapp, launch_builtin_espdl_qapp, launch_builtin_dafeiyu_qapp};
  const int builtin_count=sizeof(names)/sizeof(names[0]);
  const int total = builtin_count + g_desktop.nqpk;
  const int pages = (total + 8) / 9;
  if (g_apps_page_index < 0) g_apps_page_index = 0;
  if (g_apps_page_index >= pages) g_apps_page_index = pages - 1;
  const int first = g_apps_page_index * 9;
  for (int i = first; i < total && i < first + 9; i++)
    {
      int slot = i - first;
      lv_obj_t *tile = glass_card(grid, (slot % 3) * 292 + 4, (slot / 3) * 112 + 4,
                                   276, 98, theme_surface());
      glass_symbol(tile, i < builtin_count ? symbols[i] : LV_SYMBOL_FILE, 20, 20, theme_accent());
      glass_text(tile, i < builtin_count ? names[i] : g_desktop.qpk[i - builtin_count].name,
                 62, 18, 198, 20, theme_primary());
      glass_text(tile, i < builtin_count ? "内置应用" : "快应用", 62, 57, 108, 16, theme_secondary());
      glass_clickable(tile, i < builtin_count ? callbacks[i] : external_qapp_clicked,
                       i < builtin_count ? NULL : (void *)(intptr_t)(i - builtin_count));
      if (i >= builtin_count && glass_qpk_removable(g_desktop.qpk[i - builtin_count].dir, g_desktop.qpk[i - builtin_count].package)) {
        lv_obj_t *remove = glass_button(tile, "删除", 186, 50, 78, theme_card(), glass_apps_remove_open, (void *)(intptr_t)(i - builtin_count));
        lv_obj_set_height(remove, 40);
        lv_obj_remove_flag(remove, LV_OBJ_FLAG_EVENT_BUBBLE);
        if (g_apps_remove_timer) lv_obj_add_state(remove, LV_STATE_DISABLED);
      }
      if (g_apps_remove_timer && i >= builtin_count) lv_obj_add_state(tile, LV_STATE_DISABLED);
    }
  char page_text[96];
  snprintf(page_text, sizeof(page_text), "%d 个应用 · %d / %d 页", total, g_apps_page_index + 1, pages);
  glass_text(card, page_text, 40, 450, 410, 16, theme_secondary());
  lv_obj_t *previous = glass_button(card, "上一页", 590, 434, 130, theme_surface(), glass_apps_page, (void *)(intptr_t)-1);
  lv_obj_t *next = glass_button(card, "下一页", 734, 434, 130, theme_surface(), glass_apps_page, (void *)(intptr_t)1);
  glass_pager_enabled(previous, g_apps_page_index > 0, theme_primary());
  glass_pager_enabled(next, g_apps_page_index + 1 < pages, theme_primary());
  if (g_desktop.qpk_scan_failed) show_toast("内存不足，部分应用暂未列出；请稍后重新打开");
}

static void glass_fade(void *object, int32_t value)
{
  lv_obj_set_style_opa(object, (lv_opa_t)value, 0);
}

static void glass_card_offset(void *object, int32_t value)
{
  lv_obj_set_style_translate_y(object, value, 0);
}

static void glass_homeassistant_enter(lv_obj_t *card)
{
  if (g_glass.reduced_motion) return;
  lv_obj_t *page = lv_obj_get_parent(card);
  lv_anim_t animation;
  lv_anim_init(&animation);
  lv_anim_set_var(&animation, page);
  lv_anim_set_values(&animation, 18, 0);
  lv_anim_set_duration(&animation, 240);
  lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&animation, glass_card_offset);
  lv_anim_start(&animation);
  lv_anim_set_values(&animation, 150, 255);
  lv_anim_set_exec_cb(&animation, glass_fade);
  lv_anim_start(&animation);
}

/* Home-only styling keeps application panels and their hit targets stable. */
static void glass_home_finish(void)
{
  for (unsigned int i = 0; i < 4; i++)
    {
      lv_obj_t *card = g_glass.home_cards[i];
      lv_obj_set_style_shadow_width(card, 22, 0);
      lv_obj_set_style_shadow_ofs_y(card, 8, 0);
      lv_obj_set_style_shadow_color(card, lv_color_hex(0x34333f), 0);
      lv_obj_set_style_shadow_opa(card, g_desktop.light_theme ? 42 : 65, 0);
      lv_obj_set_style_border_width(card, 1, 0);
      lv_obj_set_style_border_opa(card, g_desktop.light_theme ? 180 : 65, 0);
      lv_obj_set_style_shadow_width(card, 10, LV_STATE_PRESSED);
      lv_obj_set_style_shadow_ofs_y(card, 3, LV_STATE_PRESSED);
      lv_obj_set_style_translate_y(card, 2, LV_STATE_PRESSED);
      if (!g_glass.reduced_motion)
        {
          lv_anim_t animation;
          lv_obj_set_style_translate_y(card, 12, 0);
          lv_anim_init(&animation);
          lv_anim_set_var(&animation, card);
          lv_anim_set_values(&animation, 12, 0);
          lv_anim_set_delay(&animation, i * 45);
          lv_anim_set_duration(&animation, 260);
          lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
          lv_anim_set_exec_cb(&animation, glass_card_offset);
          lv_anim_start(&animation);
        }
    }
}

static void glass_rebuild(void)
{
  lv_obj_t *root;
  lv_obj_t *card;
  uint32_t primary = theme_primary();
  uint32_t secondary = theme_secondary();
  bool light = g_desktop.light_theme;
  if (g_glass.home != NULL)
    {
      lv_anim_delete(g_glass.home, glass_fade);
      lv_obj_delete(g_glass.home);
    }
  g_glass_display.valid = false;
  root = glass_box(g_desktop.screen, 0, 0, 1024, 600,
                    light ? 0xf3f3f6 : 0x191b20, 0);
  g_glass.home = root;
  /* The shell stays behind the original modal/application container. */
  lv_obj_move_to_index(root, 0);
  glass_gradient(root, light ? 0xededf2 : 0x25272c, LV_GRAD_DIR_HOR);
  glass_text(root, "VelaDesk", 112, 30, 230, 20, primary);
  g_glass.wifi_button = glass_box(root, 16, 16, 72, 64, theme_card(), 8);
  /* Centre the glyph and its caption on the same axis: their font metrics
   * differ, so aligning both to the box beats tuned offsets. */
  lv_obj_t *wifi_symbol = glass_symbol(g_glass.wifi_button, LV_SYMBOL_WIFI,
                                       0, 0, primary);
  lv_obj_align(wifi_symbol, LV_ALIGN_TOP_MID, 0, 7);
  lv_obj_t *wifi_label = glass_text(g_glass.wifi_button, "Wi-Fi", 0, 0,
                                    68, 16, primary);
  lv_obj_set_style_text_align(wifi_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(wifi_label, LV_ALIGN_TOP_MID, 0, 36);
  glass_clickable(g_glass.wifi_button, glass_system_page_open, NULL);
  g_glass.date = glass_text(root, "时钟待校准", 386, 34, 380, 16, secondary);
  g_glass.clock = glass_text(root, "--:--", 814, 28, 116, 28, primary);
  lv_obj_t *pull_zone = glass_box(root, 800, 0, 224, 78, theme_surface(), 0);
  lv_obj_set_style_bg_opa(pull_zone, LV_OPA_TRANSP, 0);
  lv_obj_remove_flag(pull_zone, LV_OBJ_FLAG_GESTURE_BUBBLE);
  if (!GLASS_SIMPLE_SETTINGS)
    lv_obj_add_event_cb(pull_zone, glass_control_gesture, LV_EVENT_GESTURE, NULL);
  g_glass.theme_button = glass_button(root, "", 948, 22, 48,
    theme_surface(), settings_clicked, NULL);
  lv_obj_set_style_radius(g_glass.theme_button, 12, 0);
  /* Two opposing sliders form the familiar control-center glyph. */
  for (int row = 0; row < 2; row++)
    {
      lv_obj_t *track = glass_box(g_glass.theme_button, 12, 15 + row * 14,
                                  24, 3, primary, 2);
      lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_t *knob = glass_box(g_glass.theme_button, row ? 25 : 14,
                                 11 + row * 14, 9, 11, primary, 4);
      lv_obj_remove_flag(knob, LV_OBJ_FLAG_CLICKABLE);
    }

  card = glass_card(root, 112, 96, 884, 288, 0x202734);
  g_glass.home_cards[0] = card;
  lv_obj_set_style_radius(card, 16, 0);
  static const uint32_t ends[] = {0x565076, 0x71423f, 0x326461};
  glass_gradient(card, ends[g_glass.palette], LV_GRAD_DIR_HOR);
  if (g_wallpaper.data != NULL)
    {
      lv_obj_t *image = lv_image_create(card);
      lv_image_set_src(image, &g_wallpaper);
      lv_obj_set_pos(image, 0, 0);
      lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_clip_corner(card, true, 0);
    }
  lv_obj_set_style_border_width(card, 2, 0);
  lv_obj_set_style_border_opa(card, 150, 0);
  glass_text(card, "MUSIC / 音乐空间", 32, 30, 370, 20, 0xf3d983);
  glass_text(card, "此刻，听见喜欢", 32, 132, 418, 28, 0xffffff);
  glass_text(card, "搜索好歌，收藏你的热爱", 34, 185, 400, 20, 0xe4dfe3);
  glass_symbol(card, LV_SYMBOL_RIGHT, 36, 239, 0xffffff);
  const lv_image_dsc_t *feature_covers[] = {&img_lv_demo_music_cover_1,
                                           &img_lv_demo_music_cover_3};
  for (int i = 0; i < 2; i++)
    {
      lv_obj_t *art = lv_image_create(card);
      lv_image_set_src(art, feature_covers[i]);
      lv_image_set_pivot(art, 0, 0);
      lv_image_set_scale(art, i ? 300 : 240);
      lv_obj_set_pos(art, i ? 638 : 475, i ? 60 : 34);
      lv_obj_remove_flag(art, LV_OBJ_FLAG_CLICKABLE);
    }
  glass_clickable(card, glass_music, NULL);

  card = glass_card(root, 400, 404, 308, 172, light ? 0xe0efeb : 0x323a38);
  g_glass.home_cards[2] = card;
  lv_obj_set_style_radius(card, 16, 0);
  glass_text(card, "智能家居", 22, 18, 220, 20, primary);
  glass_symbol(card, LV_SYMBOL_HOME, 258, 22, light ? 0x27776b : 0x99dbca);
  g_glass.devices = glass_number(card, "-- / --", 21, 52, 266, false, primary);
  g_glass.sync = glass_text(card, "尚未同步 · 点击连接 Home Assistant",
                            22, 137, 264, 16, secondary);
  glass_clickable(card, glass_homeassistant, NULL);

  card = glass_card(root, 728, 404, 268, 172, theme_card());
  g_glass.home_cards[3] = card;
  lv_obj_set_style_radius(card, 16, 0);
  g_glass.widget_title = glass_text(card, "空气质量", 20, 18, 128, 20, primary);
  g_glass.widget_badge = glass_box(card, 164, 16, 84, 26, secondary, 8);
  lv_obj_set_style_bg_opa(g_glass.widget_badge, 24, 0);
  lv_obj_remove_flag(g_glass.widget_badge, LV_OBJ_FLAG_CLICKABLE);
  g_glass.widget_category = glass_text(g_glass.widget_badge, "暂无数据", 4, 4, 76, 16, secondary);
  lv_obj_set_style_text_align(g_glass.widget_category, LV_TEXT_ALIGN_CENTER, 0);
  /* Four-digit AQI and 100% humidity fit without colliding with the weather column. */
  g_glass.widget_value = glass_number(card, "--", 20, 48, 132, false, primary);
  g_glass.air = glass_text(card, "AQI 指数", 22, 106, 128, 16, secondary);
  lv_obj_t *rule = glass_box(card, 152, 56, 1, 64, secondary, 0);
  lv_obj_set_style_bg_opa(rule, 40, 0);
  lv_obj_remove_flag(rule, LV_OBJ_FLAG_CLICKABLE);
  glass_text(card, "温度", 164, 52, 84, 16, secondary);
  g_glass.temperature = glass_text(card, "-- C", 164, 74, 84, 24, primary);
  g_glass.humidity = glass_text(card, "湿度 --%", 164, 106, 84, 16, secondary);
  rule = glass_box(card, 20, 132, 228, 1, secondary, 0);
  lv_obj_set_style_bg_opa(rule, 32, 0);
  lv_obj_remove_flag(rule, LV_OBJ_FLAG_CLICKABLE);
  g_glass.widget_hint = glass_text(card, "等待联网", 20, 142, 228, 16, secondary);
  glass_clickable(card, glass_widget_open, NULL);

  card = glass_card(root, 112, 404, 268, 172, 0xd63858);
  g_glass.home_cards[1] = card;
  lv_obj_set_style_radius(card, 16, 0);
  glass_gradient(card, 0xee725f, LV_GRAD_DIR_HOR);
  glass_text(card, "音乐空间", 22, 18, 224, 20, 0xffffff);
  glass_text(card, "搜索 · 播放 · 收藏", 22, 137, 224, 16, 0xffffff);
  lv_obj_t *cover = lv_image_create(card);
  lv_image_set_src(cover, &img_lv_demo_music_cover_1);
  lv_obj_set_pos(cover, 140, 48);
  lv_image_set_scale(cover, 144);
  lv_image_set_pivot(cover, 0, 0);
  lv_obj_remove_flag(cover, LV_OBJ_FLAG_CLICKABLE);
  glass_clickable(card, glass_music, NULL);

  const char *symbols[] = {LV_SYMBOL_IMAGE, LV_SYMBOL_HOME, LV_SYMBOL_EDIT, LV_SYMBOL_AUDIO,
                           LV_SYMBOL_EYE_OPEN, LV_SYMBOL_LIST};
  const char *dock_names[] = {"相机", "家居", "AI", "音乐", "OuO", "应用"};
  lv_event_cb_t callbacks[] = {launch_builtin_camera_qapp, glass_homeassistant,
    glass_chat_open, glass_music, launch_builtin_ouo_qapp, qpk_clicked};
  for (int i = 0; i < 6; i++)
    {
      card = glass_box(root, 16, 96 + i * 80, 72, 76, theme_card(), 18);
      lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, 0);
      glass_symbol(card, symbols[i], 24, 14, secondary);
      lv_obj_t *dock_label = glass_text(card, dock_names[i], 0, 49, 72, 16, secondary);
      lv_obj_set_style_text_align(dock_label, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_style_bg_opa(card, 100, LV_STATE_PRESSED);
      glass_clickable(card, callbacks[i], NULL);
    }
  glass_update(NULL);
  glass_home_finish();
  if (!g_glass.reduced_motion)
    {
      lv_anim_t animation;
      lv_anim_init(&animation);
      lv_anim_set_var(&animation, root);
      lv_anim_set_values(&animation, 200, 255);
      lv_anim_set_duration(&animation, 220);
      lv_anim_set_exec_cb(&animation, glass_fade);
      lv_anim_start(&animation);
    }
}

static bool g_edge_back_pending;
static bool g_edge_tracking;
static lv_point_t g_edge_start;
static lv_obj_t *g_edge_page;
static lv_obj_t *g_edge_strip;

static void glass_edge_back_async(void *data)
{
  LV_UNUSED(data);
  g_edge_back_pending = false;
  /* Ignore a gesture if another action changed the page before dispatch. */
  if (g_edge_page != g_desktop.current_card) return;
  if (g_desktop.dialog != NULL)
    dialog_cancel();
  else if (g_control_card != NULL && g_control_card == g_desktop.current_card &&
           g_control_preferences && !GLASS_SIMPLE_SETTINGS)
    glass_control_return(NULL);
  else if (!lv_obj_has_flag(g_desktop.panel, LV_OBJ_FLAG_HIDDEN))
    {
      if (g_desktop.chat_page) glass_chat_back(NULL);
      else if (portal_page == g_desktop.current_card) portal_go_back(NULL);
      else if (g_desktop.qapp_page) qapp_back(NULL);
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
      lv_indev_get_point(input, &g_edge_start);
      g_edge_tracking = true;
      g_edge_strip = strip;
      g_edge_page = g_desktop.current_card;
    }
  else if (code == LV_EVENT_PRESSING && g_edge_tracking && strip == g_edge_strip)
    {
      lv_point_t point;
      lv_indev_get_point(input, &point);
      int dx = point.x - g_edge_start.x;
      int dy = abs(point.y - g_edge_start.y);
      bool right = (uintptr_t)lv_event_get_user_data(e) != 0;
      int inward = right ? -dx : dx;
      if (dy > 48) g_edge_tracking = false;
      else if (inward >= 72 && inward > 2 * dy)
        {
          g_edge_tracking = false;
          lv_indev_wait_release(input);
          if (!g_edge_back_pending &&
              lv_async_call(glass_edge_back_async, NULL) == LV_RESULT_OK)
            g_edge_back_pending = true;
        }
    }
  else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
    g_edge_tracking = false;
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
  g_glass.palette = 1;
  g_desktop.light_theme = true;
  uint8_t preferences[4] = {1, 1, 0, 0};
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
  g_desktop.screen = lv_screen_active();
  lv_obj_set_style_text_font(g_desktop.screen, zh_font(20), 0);
  lv_obj_remove_flag(g_desktop.screen, LV_OBJ_FLAG_SCROLLABLE);
  qpk_scan();
  g_desktop.panel = lv_obj_create(g_desktop.screen);
  lv_obj_set_pos(g_desktop.panel, 0, 0);
  lv_obj_set_size(g_desktop.panel, 1024, 600);
  lv_obj_set_style_pad_all(g_desktop.panel, 0, 0);
  lv_obj_set_style_border_width(g_desktop.panel, 0, 0);
  lv_obj_set_style_radius(g_desktop.panel, 0, 0);
  lv_obj_remove_flag(g_desktop.panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(g_desktop.panel, LV_OBJ_FLAG_HIDDEN);
  glass_rebuild();
  glass_edge_back_create();
  g_glass.timer = lv_timer_create(glass_update, 1000, NULL);
  pet_engine_init(lv_obj_get_width(g_desktop.screen),
                  lv_obj_get_height(g_desktop.screen));
  pet_engine_set_weather_cb(pet_fill_weather);
  if (pet_lvgl_create(g_desktop.screen, zh_font(16)) < 0)
    {
      printf("desktop: pet overlay skipped\n");
    }
}
