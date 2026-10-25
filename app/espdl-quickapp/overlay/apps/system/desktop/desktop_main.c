/****************************************************************************
 * apps/system/desktop/desktop_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Glass desktop and application host for ESP32-P4 Function-EV-Board.
 ****************************************************************************/

#include <nuttx/config.h>

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/mount.h>
#include <time.h>
#include <unistd.h>
#include <cJSON.h>

#include <nuttx/sched.h>
#include <lvgl/lvgl.h>
#include "glass_ui_visibility.h"
#ifdef CONFIG_NETUTILS_NETINIT
#  include <netutils/netinit.h>
#endif

#include "qpk_runtime.h"
#ifdef CONFIG_SYSTEM_BILIBILI_LIVE
#include "../bilibili_live/bili_live.h"
#include "../bilibili_live/local_video_ui.h"
#include "../bilibili_live/local_album_ui.h"
#include "../bilibili_live/local_video_media.h"
#include "../bilibili_live/bili_cpu.h"
#include "../bilibili_live/bili_ppa.h"
#include "../bilibili_live/bili_api.h"
#endif
#include "qpk_security.h"
#include "qpk_storage.h"
#include "qpk_volume.h"
#ifdef GLASS_LOCAL_SPEECH
#include "glass_speech.h"
#endif
#include "qpk_limits.h"
#include "glass_qpk_builder.h"
#include "glass_portal.h"
#include "glass_ai_config.h"
#include "glass_board_music.h"
#include "glass_assistant.h"
#include "glass_agent.h"
#include "qpk_hardware.h"
#include "qpk_espdl.h"
#include "qpk_camera_clip.h"
#ifdef CONFIG_SYSTEM_HASS
#include "hass_portal.h"
#include "hass_service.h"
#include "glass_board_home.h"
#endif
#include "glass_dashboard.h"
#include "glass_icons.h"
#include "pet_engine.h"
#include "pet_lvgl.h"

#define QPK_DIR       CONFIG_SYSTEM_DESKTOP_QPK_DIR
#define NAME_MAXLEN   48
#define QAPP_HEADER_HEIGHT  64
#include "glass_status.h"
/* Long Chinese chat pages exceed 64 unique glyphs even within one viewport.
 * Keep glyph metrics and rasterized bitmaps across scrolling in PSRAM. */
#define DESKTOP_FONT_CACHE_GLYPHS 2048

static bool chat_qpk_preview_active, chat_qpk_preview_origin;
static uint32_t chat_qpk_preview_revision;
static void chat_qpk_preview_back(void);
static lv_obj_t *unresponsive_shade;

static void chat_qpk_preview_finish(void)
{
  if (chat_qpk_preview_active) {
    const char *error = qpk_runtime_last_error();
    if (error[0]) glass_qpk_preview_result(chat_qpk_preview_revision, error);
    chat_qpk_preview_active = false;
  }
}

extern const uint8_t g_desktop_font_start[];
extern const uint8_t g_desktop_font_end[];

struct qpk_entry_s
{
  enum qpk_volume volume;
  char dir[64];
  char name[NAME_MAXLEN];
  char package[NAME_MAXLEN];
  char version[24];
  char entry[64];
};

struct builtin_qpk_s
{
  struct qpk_entry_s manifest;
  const char *kind;
  const char *format;
};

/* Built-in QPK metadata.  The manifest name is used consistently by the
 * launcher list, the full-screen title bar, and app.getInfo().
 */

static const struct builtin_qpk_s g_builtin_qpk =
{
  .manifest =
    {
      .name = "你好快应用",
      .package = "com.example.hello",
      .version = "1.0.2",
      .entry = "builtin:/hello/app.js",
    },
  .kind = "内置示例",
  .format = "QPK 1.0",
};

static const struct builtin_qpk_s g_builtin_2048_qpk =
{
  .manifest =
    {
      .name = "2048",
      .package = "com.example.game2048",
      .version = "1.0.0",
      .entry = "builtin:/2048/index.js",
    },
  .kind = "休闲游戏",
  .format = "QPK 1.0",
};

static const struct builtin_qpk_s g_builtin_camera_qpk =
{
  .manifest =
    {
      .name = "相机",
      .package = "com.openvela.camera.preview",
      .version = "1.0.0",
      .entry = "builtin:/camera/app.js",
    },
  .kind = "设备能力",
  .format = "V4L2 快应用",
};

static const struct builtin_qpk_s g_builtin_homeassistant_qpk =
{
  .manifest =
    {
      .name = "米家 HA",
      .package = "com.openvela.homeassistant",
      .version = "0.7.0",
      .entry = "builtin:/homeassistant/app.js",
    },
  .kind = "智能家居",
  .format = "REST 快应用",
};

struct desktop_env_s
{
  lv_obj_t *screen;
  lv_obj_t *statusbar;
  lv_obj_t *clock_label;
  lv_obj_t *home_title;
  lv_obj_t *home_hint;
  lv_obj_t *grid;
  lv_obj_t *panel;
  lv_obj_t *current_card;
  lv_obj_t *toast;
  lv_obj_t *dialog;
  lv_timer_t *toast_timer;
  lv_font_t *font16;
  lv_font_t *font20;
  lv_font_t *font28;
  struct qpk_entry_s *qpk;
  int qpk_capacity;
  bool qpk_scan_failed;
  int nqpk;
  bool light_theme;
  bool settings_rebuild_pending;
  bool apps_page;
  bool chat_page;
  bool chat_return_apps;
  bool qapp_page;
  bool qapp_return_apps;
};

static struct desktop_env_s g_desktop;

enum desktop_camera_command_e
{
  DESKTOP_CAMERA_NONE = 0,
  DESKTOP_CAMERA_LAUNCH,
  DESKTOP_CAMERA_STOP,
  DESKTOP_CAMERA_VIDEO_MODE,
  DESKTOP_CAMERA_VIDEO_START,
  DESKTOP_CAMERA_VIDEO_STOP,
};

static pthread_mutex_t g_camera_command_lock = PTHREAD_MUTEX_INITIALIZER;
static enum desktop_camera_command_e g_camera_command;
static bool g_camera_launch_pending; /* UI thread only. */
static lv_obj_t *g_camera_album_page;
static void launch_builtin_camera_qapp(lv_event_t *e);

enum builtin_id_e
{
  APP_SETTINGS = 0,
  APP_HOME_ASSISTANT,
  APP_QPK,
  APP_ABOUT
};

static void glass_chat_ball_open(lv_event_t *event);
static void chat_display_restore(void);
static void chat_display_resume(void);
static void settings_rebuild_async(void *data);
static void glass_system_page_cancel(void);
static void glass_save_preferences(void);
static const lv_font_t *hass_qpk_font(int size);

static const lv_font_t *zh_font(int size)
{
  if (size >= 28 && g_desktop.font28 != NULL)
    {
      return g_desktop.font28;
    }

  if (size >= 20 && g_desktop.font20 != NULL)
    {
      return g_desktop.font20;
    }

  if (g_desktop.font16 != NULL)
    {
      return g_desktop.font16;
    }

  return &lv_font_montserrat_24;
}

#include "glass_app_controls.h"

static uint32_t theme_primary(void)
{
  return glass_app_palette(g_desktop.light_theme).ink;
}

static uint32_t theme_secondary(void)
{
  return glass_app_palette(g_desktop.light_theme).muted;
}

static uint32_t theme_card(void)
{
  return glass_app_palette(g_desktop.light_theme).paper;
}

static uint32_t theme_surface(void)
{
  return glass_app_palette(g_desktop.light_theme).soft;
}

static uint32_t theme_accent(void)
{
  return glass_app_palette(g_desktop.light_theme).accent;
}

#include "glass_motion.h"
#include "glass_ui_metrics.h"

void desktop_motion_button(lv_obj_t *button)
{
  glass_motion_button(button);
}
bool desktop_motion_geometry(lv_obj_t *obj,int x,int y,int w,int h,unsigned mask,bool animate)
{
  return glass_motion_geometry(obj,x,y,w,h,mask,animate);
}

bool desktop_motion_color(lv_obj_t *obj,uint32_t color,bool animate)
{ return glass_motion_color(obj,color,animate); }

void desktop_motion_present(lv_obj_t **objects,unsigned count,int offset,bool horizontal)
{ if(horizontal)glass_motion_present_axis(objects,count,offset,true);else glass_motion_present(objects,count,offset); }
bool desktop_motion_range(lv_obj_t *obj,int stop,uint32_t accent,uint32_t background,bool animate)
{ return glass_motion_range(obj,stop,accent,background,animate); }
bool desktop_motion_range_linked(lv_obj_t *obj,lv_obj_t *thumb,int stop,uint32_t accent,uint32_t background,bool animate)
{ return glass_motion_range_linked(obj,thumb,stop,accent,background,animate); }

static void style_button(lv_obj_t *button, uint32_t color)
{
  glass_app_control_style(button,color,glass_app_palette(g_desktop.light_theme).line);
  glass_motion_button(button);
}

static void style_panel(lv_obj_t *panel, uint32_t color)
{
  glass_app_panel_style(panel,color,glass_app_palette(g_desktop.light_theme).line);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            uint32_t color, int size)
{
  lv_obj_t *label = lv_label_create(parent);

  if (label == NULL)
    {
      return NULL;
    }

  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
  lv_obj_set_style_text_font(label, zh_font(size), 0);
  return label;
}

static bool qpk_path_component_valid(const char *text)
{
  const char *cursor;

  if (text == NULL || text[0] == '\0' || text[0] == '/' ||
      text[0] == '\\')
    {
      return false;
    }

  for (cursor = text; *cursor != '\0'; cursor++)
    {
      if (*cursor == ':' || *cursor == '\\' || *cursor == '/' ||
          (*cursor == '.' && cursor[1] == '.'))
        {
          return false;
        }
    }

  return true;
}

static void json_get_string(const cJSON *json, const char *field,
                            char *out, size_t outlen)
{
  out[0] = '\0';
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(json, field);
  if (cJSON_IsString(value) && strlen(value->valuestring) < outlen)
    strcpy(out, value->valuestring);
}

static bool qpk_probe(const char *dirpath, struct qpk_entry_s *entry)
{
  char path[192];
  FILE *fp;
  char buf[1024];
  size_t n;

  if (snprintf(path, sizeof(path), "%s/manifest.json", dirpath) >=
      sizeof(path))
    {
      return false;
    }

  fp = fopen(path, "r");
  if (fp == NULL)
    {
      return false;
    }

  n = fread(buf, 1, sizeof(buf) - 1, fp);
  bool failed = ferror(fp);
  if (fclose(fp) != 0 || failed || n == sizeof(buf) - 1)
    {
      return false;
    }

  buf[n] = '\0';

  cJSON *manifest = cJSON_ParseWithOpts(buf, NULL, true);
  if (!cJSON_IsObject(manifest)) { cJSON_Delete(manifest); return false; }
  json_get_string(manifest, "name", entry->name, sizeof(entry->name));
  json_get_string(manifest, "package", entry->package,
                  sizeof(entry->package));
  json_get_string(manifest, "versionName", entry->version,
                  sizeof(entry->version));
  json_get_string(manifest, "entry", entry->entry, sizeof(entry->entry));
  cJSON_Delete(manifest);
  return entry->name[0] != '\0' &&
         qpk_path_component_valid(entry->package) &&
         !qpk_reserved_package(entry->package) &&
         (entry->entry[0] == '\0' ||
          qpk_path_component_valid(entry->entry));
}

static void qpk_scan(void)
{
  DIR *dp;
  struct dirent *ent;
  char path[192];
  struct stat st;

  g_desktop.nqpk = 0;
  g_desktop.qpk_scan_failed = false;
  for(unsigned volume=0;volume<2;volume++) {
  const char *scan_root=qpk_volume_root(volume);
  if(qpk_volume_ready(volume,false))continue;
  dp = opendir(scan_root);
  if (dp == NULL)
    {
      continue;
    }

  while ((ent = readdir(dp)) != NULL)
    {
      struct qpk_entry_s candidate;
      struct qpk_entry_s *qpk = &candidate;

      if (ent->d_name[0] == '.' ||
          snprintf(path, sizeof(path), "%s/%s", scan_root, ent->d_name) >=
          sizeof(path) || lstat(path, &st) != 0 || !S_ISDIR(st.st_mode))
        {
          continue;
        }

      memset(qpk, 0, sizeof(*qpk));
      if (!qpk_probe(path, qpk))
        {
          continue;
        }

      bool duplicate=false;
      for(int j=0;j<g_desktop.nqpk;j++)if(!strcmp(g_desktop.qpk[j].package,qpk->package))duplicate=true;
      if(duplicate)continue;
      qpk->volume=volume;
      strlcpy(qpk->dir, ent->d_name, sizeof(qpk->dir));
      if (g_desktop.nqpk == g_desktop.qpk_capacity) {
        if (g_desktop.qpk_capacity > (INT_MAX - 16) / 2) { g_desktop.qpk_scan_failed = true; break; }
        int capacity = g_desktop.qpk_capacity ? g_desktop.qpk_capacity * 2 : 16;
        if (capacity <= g_desktop.qpk_capacity || (size_t)capacity > SIZE_MAX / sizeof(*qpk)) {
          g_desktop.qpk_scan_failed = true; break;
        }
        struct qpk_entry_s *entries = realloc(g_desktop.qpk, capacity * sizeof(*entries));
        if (!entries) { g_desktop.qpk_scan_failed = true; break; }
        g_desktop.qpk = entries; g_desktop.qpk_capacity = capacity;
      }
      g_desktop.qpk[g_desktop.nqpk++] = candidate;
    }

  closedir(dp);
  }
}

static void glass_rebuild(void);
static void glass_settings(lv_event_t *e);
static void glass_apps(lv_event_t *e);

static void apply_theme(void)
{
  glass_rebuild();
}

static void toast_cancel(void)
{
  lv_timer_t *timer = g_desktop.toast_timer;
  lv_obj_t *toast = g_desktop.toast;

  g_desktop.toast_timer = NULL;
  g_desktop.toast = NULL;

  if (timer != NULL)
    {
      lv_timer_delete(timer);
    }

  if (toast != NULL)
    {
      lv_obj_delete(toast);
    }
}

static void dialog_cancel(void)
{
  lv_obj_t *dialog = g_desktop.dialog;

  g_desktop.dialog = NULL;
  /* dialog_cancel owns every dialog on the top layer, including the
   * unresponsive prompt; keep the static pointer in sync. */
  unresponsive_shade = NULL;
  if (dialog != NULL)
    {
      lv_obj_delete(dialog);
    }
}

static struct {
  lv_obj_t *page;
  int x,y,w,h;
  int screen_x,screen_y,screen_w,screen_h;
  bool status_hidden;
} g_camera_page;

static void camera_page_restore(void)
{
  if(!g_camera_page.page)return;
  g_camera_page.page=NULL;
  lv_obj_set_pos(g_desktop.screen,g_camera_page.screen_x,g_camera_page.screen_y);
  lv_obj_set_size(g_desktop.screen,g_camera_page.screen_w,g_camera_page.screen_h);
  lv_obj_set_pos(g_desktop.panel,g_camera_page.x,g_camera_page.y);
  lv_obj_set_size(g_desktop.panel,g_camera_page.w,g_camera_page.h);
  if(g_desktop.statusbar&&!g_camera_page.status_hidden)
    lv_obj_remove_flag(g_desktop.statusbar,LV_OBJ_FLAG_HIDDEN);
}

static void camera_page_deleted(lv_event_t *event)
{if(g_camera_page.page==lv_event_get_target(event))g_camera_page.page=NULL;}

/* Only inner lists may scroll. LVGL's active screen is scrollable by default,
 * so a drag propagated past a fixed app can otherwise pull the entire shell. */
static void desktop_fixed_shell(void)
{
  lv_obj_t *surfaces[]={lv_screen_active(),lv_layer_top(),g_desktop.screen,g_desktop.panel};
  for(unsigned i=0;i<sizeof(surfaces)/sizeof(surfaces[0]);i++){
    lv_obj_t *obj=surfaces[i];if(!obj)continue;
    lv_obj_remove_flag(obj,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_SCROLL_ELASTIC|
      LV_OBJ_FLAG_SCROLL_MOMENTUM|LV_OBJ_FLAG_SCROLL_CHAIN_HOR|LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_set_scroll_dir(obj,LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(obj,LV_SCROLLBAR_MODE_OFF);
    lv_obj_scroll_to(obj,0,0,LV_ANIM_OFF);
  }
}

static void desktop_navigation_cancel(void)
{
  glass_system_page_cancel();
  if (g_desktop.settings_rebuild_pending)
    {
      lv_async_call_cancel(settings_rebuild_async, NULL);
      g_desktop.settings_rebuild_pending = false;
    }
}

static void desktop_transients_reset(void)
{
  camera_page_restore();
  /* Retire full-screen ownership before measuring any replacement page. */
  chat_display_restore();
  desktop_fixed_shell();
  toast_cancel();
  dialog_cancel();

  desktop_navigation_cancel();
}

static void glass_weather_home_resume(void);
static void panel_hide(lv_event_t *e)
{
  LV_UNUSED(e);
  chat_qpk_preview_finish();
  g_camera_launch_pending = false;
  qpk_runtime_stop();
#ifdef CONFIG_SYSTEM_BILIBILI_LIVE
  bili_live_stop();
#endif
  desktop_transients_reset();
  lv_obj_add_flag(g_desktop.panel, LV_OBJ_FLAG_HIDDEN);
  g_desktop.current_card = NULL;
  lv_obj_clean(g_desktop.panel);
  g_desktop.apps_page = false;
  g_desktop.chat_page = false;
  g_desktop.chat_return_apps = false;
  g_desktop.qapp_page = false;
  g_desktop.qapp_return_apps = false;
  glass_weather_home_resume();
}

static lv_obj_t *panel_card(const char *title)
{
  lv_obj_t *card;
  lv_obj_t *close;
  lv_obj_t *label;

  chat_qpk_preview_finish();
  g_camera_launch_pending = false;
  qpk_runtime_stop();
#ifdef CONFIG_SYSTEM_BILIBILI_LIVE
  bili_live_stop();
#endif
  desktop_transients_reset();
  g_desktop.apps_page = false;
  g_desktop.chat_page = false;
  g_desktop.chat_return_apps = false;
  g_desktop.qapp_page = false;
  g_desktop.qapp_return_apps = false;
  lv_obj_remove_flag(g_desktop.panel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clean(g_desktop.panel);
  lv_obj_set_style_bg_color(g_desktop.panel, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(g_desktop.panel, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(g_desktop.panel, 0, 0);

  card = lv_obj_create(g_desktop.panel);
  if (card == NULL)
    {
      return NULL;
    }

  g_desktop.current_card = card;
  lv_obj_set_size(card, 1024, GLASS_CONTENT_HEIGHT);
  lv_obj_set_pos(card, 0, 0);
  style_panel(card, g_desktop.light_theme ? 0xf4f5f8 : 0x15171e);
  lv_obj_set_style_radius(card, 0, 0);
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_set_style_shadow_width(card, 0, 0);
  lv_obj_set_style_pad_all(card, 0, 0);
  lv_obj_set_style_border_color(card,
                                lv_color_hex(g_desktop.light_theme ?
                                             0xcbd1dd : 0x35415e), 0);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  label = make_label(card, title, theme_primary(), 28);
  if (label == NULL)
    {
      lv_obj_delete(card);
      g_desktop.current_card = NULL;
      return NULL;
    }

  lv_obj_set_width(label, 1024 - 120);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  lv_obj_align(label, LV_ALIGN_TOP_LEFT, 24, 18);

  close = lv_button_create(card);
  if (close == NULL)
    {
      lv_obj_delete(card);
      g_desktop.current_card = NULL;
      return NULL;
    }

  lv_obj_set_size(close, 54, 42);
  lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -20, 14);
  style_button(close, theme_surface());
  lv_obj_add_event_cb(close, panel_hide, LV_EVENT_CLICKED, NULL);
  label = lv_label_create(close);
  if (label == NULL)
    {
      lv_obj_delete(card);
      g_desktop.current_card = NULL;
      return NULL;
    }

  lv_label_set_text(label, LV_SYMBOL_CLOSE);
  lv_obj_set_style_text_font(label, glass_icon_font(24), 0);
  lv_obj_set_style_text_color(label, lv_color_hex(theme_primary()), 0);
  lv_obj_center(label);
  /* Cards open at their final position; no deferred upward entrance. */
  return card;
}

static void toast_motion_done(lv_obj_t *toast)
{
  if(g_desktop.toast==toast)g_desktop.toast=NULL;
  lv_obj_delete(toast);
}

static void toast_delete_cb(lv_timer_t *timer)
{
  if (g_desktop.toast_timer == timer)
    {
      lv_obj_t *toast = g_desktop.toast;

      g_desktop.toast_timer = NULL;
      if (toast != NULL)
        {
          if(!glass_motion_dismiss(toast,toast_motion_done))toast_motion_done(toast);
        }
    }

  lv_timer_delete(timer);
}

static void show_toast(const char *text)
{
  toast_cancel();

  g_desktop.toast = make_label(g_desktop.panel, text, theme_primary(), 20);
  if (g_desktop.toast == NULL)
    {
      return;
    }

  lv_obj_set_style_bg_color(g_desktop.toast, lv_color_hex(theme_surface()), 0);
  lv_obj_set_style_bg_opa(g_desktop.toast, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(g_desktop.toast, 12, 0);
  lv_obj_set_style_max_width(g_desktop.toast, 880, 0);
  lv_obj_set_style_pad_hor(g_desktop.toast, 22, 0);
  lv_obj_set_style_pad_ver(g_desktop.toast, 12, 0);
  lv_obj_align(g_desktop.toast, LV_ALIGN_BOTTOM_MID, 0, -34);
  glass_motion_notice(g_desktop.toast);
  g_desktop.toast_timer = lv_timer_create(toast_delete_cb, 1600, NULL);
  if (g_desktop.toast_timer == NULL)
    {
      toast_cancel();
    }
}

static const char g_hello_qpk_js[] =
"'use strict';\n"
"const info = app.getInfo();\n"
"let launches = Number(system.storage.get('launches') || 0) + 1;\n"
"system.storage.set('launches', String(launches));\n"
"const left = Math.floor((ui.getSize().width - 400) / 2);\n"
"const height=ui.getSize().height;\n"
"ui.background(ui.theme?ui.theme.background:ui.card);\n"
"ui.text(info.packageName + '  v' + info.versionName, left, 72, 16, ui.secondary);\n"
"ui.text(info.name, left, 28, 28, ui.primary);\n"
"ui.panel(left, 112, 400, 96, ui.theme?ui.theme.card:ui.surface, 18, 255);\n"
"ui.number(String(launches), left + 20, 126, 80, 64, ui.primary);\n"
"ui.text('累计启动次数', left + 120, 142, 20, ui.secondary);\n"
"ui.button('Toast 提示', left, height-168, 400, 44, () => {\n"
"  prompt.showToast({message: '来自真正 QuickJS QPK 的问候'});\n"
"}, ui.accent);\n"
"ui.button('对话框', left, height-112, 400, 44, () => {\n"
"  prompt.dialog({title: 'QPK 运行时',\n"
"    message: '界面由 JavaScript 创建，事件由 QuickJS 执行。'});\n"
"}, ui.surface);\n"
"let secs = 0;\n"
"const tick = ui.text('已运行 0 秒', left, height-44, 16, ui.secondary);\n"
"setInterval(() => { secs++; ui.setText(tick, '已运行 ' + secs + ' 秒'); }, 1000);\n"
"console.log('hello QPK initialized');\n"
;

static const char g_2048_qpk_js[] =
"'use strict';\n"
"const viewport = ui.getSize();\n"
"const W = viewport.width, H = viewport.height;\n"
"const controlsY = H - 56;\n"
"const theme=ui.theme||{background:0xfaf8ef,surface:0xbbada0,card:0xcdc1b4,primary:0x776e65,secondary:0x776e65,accent:0xf0a04b};\n"
"const scoreWidth=Math.floor((W-56)/3);\n"
"const tileGap = 8;\n"
"const boardSize = Math.min(W - 40, controlsY - 54);\n"
"const tileSize = Math.floor((boardSize - 16 - tileGap * 3) / 4);\n"
"const actualBoard = tileSize * 4 + tileGap * 3 + 16;\n"
"const boardX = Math.floor((W - actualBoard) / 2);\n"
"const boardY = 46;\n"
"const scoreLabel = ui.text('分数 0', 20, 10, 20, theme.primary);\n"
"const bestLabel = ui.text('最高 0', 28+scoreWidth, 10, 20, theme.primary);\n"
"ui.setSize(scoreLabel, scoreWidth, 32);\n"
"ui.setSize(bestLabel, scoreWidth, 32);\n"
"const statusLabel = ui.text('准备开始',36+scoreWidth*2,12,16,theme.secondary);\n"
"ui.setSize(statusLabel,scoreWidth,28);\n"
"ui.background(theme.background);\n"
"const boardPanel = ui.panel(boardX, boardY, actualBoard, actualBoard, theme.surface, 18, 255);\n"
"const tilePanels = [], tileLabels = [];\n"
"for (let i = 0; i < 16; i++) {\n"
"  const x = boardX + 8 + (i % 4) * (tileSize + tileGap);\n"
"  const y = boardY + 8 + Math.floor(i / 4) * (tileSize + tileGap);\n"
"  tilePanels.push(ui.panel(x, y, tileSize, tileSize, theme.card, 12, 255));\n"
"  tileLabels.push(ui.number('', x, y, tileSize, tileSize, theme.primary));\n"
"}\n"
"const board = [];\n"
"let score = 0;\n"
"let best = 0;\n"
"let gameOver = false;\n"
"let won = false;\n"
"try { const value = Number(system.storage.get('best') || 0); if (isFinite(value) && value > 0) best = Math.floor(value); } catch (e) {}\n"
"function setText(handle, value) { ui.setText(handle, String(value)); }\n"
"function tileColor(value) {\n"
"  if (value === 2) return ui.theme?(theme.light?0xeee4f4:0x41384d):0xeee4da;\n"
"  if (value === 4) return ui.theme?(theme.light?0xe2d2ec:0x574568):0xede0c8;\n"
"  if (value === 8) return 0xf2b179;\n"
"  if (value === 16) return 0xf59563;\n"
"  if (value === 32) return 0xf67c5f;\n"
"  if (value === 64) return 0xf65e3b;\n"
"  if (value === 128) return 0xedcf72;\n"
"  if (value === 256) return 0xedcc61;\n"
"  if (value === 512) return 0xedc850;\n"
"  if (value === 1024) return 0xedc53f;\n"
"  if (value >= 2048) return 0xedc22e;\n"
"  return theme.card;\n"
"}\n"
"function textColor(value) { return value <= 4 ? theme.primary : 0x292c38; }\n"
"function addTile() {\n"
"  const empty = [];\n"
"  for (let i = 0; i < 16; i++) if (board[i] === 0) empty.push(i);\n"
"  if (!empty.length) return;\n"
"  const index = empty[Math.floor(Math.random() * empty.length)];\n"
"  board[index] = Math.random() < 0.9 ? 2 : 4;\n"
"}\n"
"function reset() {\n"
"  board.length = 0;\n"
"  for (let i = 0; i < 16; i++) board.push(0);\n"
"  score = 0; gameOver = false; won = false; addTile(); addTile(); draw();\n"
"}\n"
"function slide(line) {\n"
"  const values = [];\n"
"  const result = [];\n"
"  for (let i = 0; i < 4; i++) if (line[i]) values.push(line[i]);\n"
"  for (let i = 0; i < values.length; i++) {\n"
"    if (i + 1 < values.length && values[i] === values[i + 1]) {\n"
"      const merged = values[i] * 2; result.push(merged); score += merged;\n"
"      if (merged === 2048) won = true; i++;\n"
"    } else result.push(values[i]);\n"
"  }\n"
"  while (result.length < 4) result.push(0);\n"
"  for (let i = 0; i < 4; i++) if (result[i] !== line[i]) return {line:result, changed:true};\n"
"  return {line:result, changed:false};\n"
"}\n"
"function move(direction) {\n"
"  if (gameOver) return;\n"
"  let changed = false;\n"
"  for (let n = 0; n < 4; n++) {\n"
"    const line = [];\n"
"    for (let k = 0; k < 4; k++) {\n"
"      const source = direction === 'right' || direction === 'down' ? 3 - k : k;\n"
"      const p = direction === 'left' || direction === 'right' ? n * 4 + source : source * 4 + n;\n"
"      line[k] = board[p];\n"
"    }\n"
"    const result = slide(line);\n"
"    if (result.changed) changed = true;\n"
"    for (let k = 0; k < 4; k++) {\n"
"      const target = direction === 'right' || direction === 'down' ? 3 - k : k;\n"
"      const p = direction === 'left' || direction === 'right' ? n * 4 + target : target * 4 + n;\n"
"      board[p] = result.line[k];\n"
"    }\n"
"  }\n"
"  if (!changed) { if (!canMove()) { gameOver = true; setText(statusLabel, '游戏结束'); } return; }\n"
"  if (score > best) { best = score; try { system.storage.set('best', String(best)); } catch (e) {} }\n"
"  addTile();\n"
"  if (!canMove()) gameOver = true;\n"
"  draw();\n"
"}\n"
"function canMove() {\n"
"  for (let i = 0; i < 16; i++) {\n"
"    if (board[i] === 0) return true;\n"
"    if (i % 4 < 3 && board[i] === board[i + 1]) return true;\n"
"    if (i < 12 && board[i] === board[i + 4]) return true;\n"
"  }\n"
"  return false;\n"
"}\n"
"function draw() {\n"
"  setText(scoreLabel, '分数 ' + score); setText(bestLabel, '最高 ' + best);\n"
"  if (gameOver) setText(statusLabel, '游戏结束'); else if (won) setText(statusLabel, '达成 2048'); else setText(statusLabel, '准备开始');\n"
"  for (let i = 0; i < 16; i++) { const value = board[i]; ui.setColor(tilePanels[i], tileColor(value)); ui.setColor(tileLabels[i], textColor(value)); setText(tileLabels[i], value ? value : ''); }\n"
"}\n"
"const restart = ui.button('重新开始', 20, controlsY, 132, 44, reset, theme.accent);\n"
"const left = ui.button('左', W - 292, controlsY, 62, 44, function () { move('left'); }, theme.surface);\n"
"const up = ui.button('上', W - 224, controlsY, 62, 44, function () { move('up'); }, theme.surface);\n"
"const down = ui.button('下', W - 156, controlsY, 62, 44, function () { move('down'); }, theme.surface);\n"
"const right = ui.button('右', W - 88, controlsY, 62, 44, function () { move('right'); }, theme.surface);\n"
"ui.onSwipe(function (direction) { move(direction); });\n"
"reset();\n"
;

static void qpk_dialog_close(lv_event_t *e)
{
  lv_obj_t *shade = lv_event_get_user_data(e);

  if (g_desktop.dialog == shade)
    {
      g_desktop.dialog = NULL;
    }

  lv_obj_delete(shade);
}

static void qpk_show_dialog(const char *text)
{
  lv_obj_t *shade;
  lv_obj_t *box;
  lv_obj_t *button;
  lv_obj_t *label;

  dialog_cancel();
  shade = lv_obj_create(lv_layer_top());
  if (shade == NULL)
    {
      return;
    }

  g_desktop.dialog = shade;
  lv_obj_set_size(shade, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(shade, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(shade, LV_OPA_70, 0);
  lv_obj_set_style_border_width(shade, 0, 0);
  box = lv_obj_create(shade);
  if (box == NULL)
    {
      dialog_cancel();
      return;
    }

  lv_obj_set_size(box, 560, 320);
  lv_obj_center(box);
  style_panel(box, theme_card());
  lv_obj_set_style_pad_all(box, 24, 0);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *body = lv_obj_create(box);
  if (!body) { dialog_cancel(); return; }
  lv_obj_remove_style_all(body);
  lv_obj_set_size(body, 508, 202);
  lv_obj_set_scroll_dir(body, LV_DIR_VER);
  lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
  label = make_label(body, text ? text : "快应用", theme_primary(), 20);
  if (label == NULL)
    {
      dialog_cancel();
      return;
    }

  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(label, 508);
  lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);
  button = lv_button_create(box);
  if (button == NULL)
    {
      dialog_cancel();
      return;
    }

  lv_obj_set_size(button, 96, 42);
  lv_obj_align(button, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  style_button(button, theme_accent());
  lv_obj_add_event_cb(button, qpk_dialog_close, LV_EVENT_CLICKED, shade);
  label = make_label(button, "确定", 0xffffff, 20);
  if (label == NULL)
    {
      dialog_cancel();
      return;
    }

  lv_obj_center(label);
  glass_motion_enter(box,false,0);
}

static void qpk_show_unresponsive(bool unresponsive);

static void qpk_unresponsive_wait(lv_event_t *e)
{
  (void)e;
  qpk_show_unresponsive(false);
}

static void qpk_unresponsive_terminate(lv_event_t *e)
{
  (void)e;
  qpk_show_unresponsive(false);
  qpk_runtime_stop();
  panel_hide(NULL);
}

/* Shown only when the runtime reports a JavaScript entry that ran for
 * seconds without yielding. It does not stop the app: timers keep running,
 * and the prompt hides itself when the app recovers or stops. "等待"
 * dismisses it; "终止程序" stops the runtime and returns home. */
static void qpk_show_unresponsive(bool unresponsive)
{
  lv_obj_t *shade;
  lv_obj_t *box;
  lv_obj_t *button;
  lv_obj_t *label;

  if (!unresponsive)
    {
      if (unresponsive_shade != NULL)
        {
          if (g_desktop.dialog == unresponsive_shade)
            {
              g_desktop.dialog = NULL;
            }

          lv_obj_delete(unresponsive_shade);
          unresponsive_shade = NULL;
        }

      return;
    }

  if (unresponsive_shade != NULL)
    {
      return;
    }

  dialog_cancel();
  shade = lv_obj_create(lv_layer_top());
  if (shade == NULL)
    {
      return;
    }

  g_desktop.dialog = shade;
  unresponsive_shade = shade;
  lv_obj_set_size(shade, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(shade, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(shade, LV_OPA_70, 0);
  lv_obj_set_style_border_width(shade, 0, 0);
  box = lv_obj_create(shade);
  if (box == NULL)
    {
      qpk_show_unresponsive(false);
      return;
    }

  lv_obj_set_size(box, 560, 260);
  lv_obj_center(box);
  style_panel(box, theme_card());
  lv_obj_set_style_pad_all(box, 24, 0);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  label = make_label(box, "快应用长时间未响应。\n等待它继续运行，或终止程序？",
                     theme_primary(), 20);
  if (label == NULL)
    {
      qpk_show_unresponsive(false);
      return;
    }

  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(label, 508);
  lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);
  button = lv_button_create(box);
  if (button == NULL)
    {
      qpk_show_unresponsive(false);
      return;
    }

  lv_obj_set_size(button, 120, 42);
  lv_obj_align(button, LV_ALIGN_BOTTOM_RIGHT, -132, 0);
  style_button(button, theme_surface());
  lv_obj_add_event_cb(button, qpk_unresponsive_wait, LV_EVENT_CLICKED, NULL);
  label = make_label(button, "等待", theme_primary(), 20);
  if (label == NULL)
    {
      qpk_show_unresponsive(false);
      return;
    }

  lv_obj_center(label);
  button = lv_button_create(box);
  if (button == NULL)
    {
      qpk_show_unresponsive(false);
      return;
    }

  lv_obj_set_size(button, 120, 42);
  lv_obj_align(button, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  style_button(button, theme_accent());
  lv_obj_add_event_cb(button, qpk_unresponsive_terminate, LV_EVENT_CLICKED, NULL);
  label = make_label(button, "终止程序", 0xffffff, 20);
  if (label == NULL)
    {
      qpk_show_unresponsive(false);
      return;
    }

  lv_obj_center(label);
}

static char *qpk_load_entry(const struct qpk_entry_s *qpk,
                            char *filename, size_t filename_size,
                            size_t *source_size)
{
  static const char *patterns[] = {"%s/%s/%s", "%s/%s/%s.js",
                                   "%s/%s/%s/index.js"};
  FILE *file;
  char *source;
  long size;
  size_t got;
  int i;

  if(qpk_volume_ready(qpk->volume,false))return NULL;
  for (i = 0; i < 3; i++)
    {
      if (snprintf(filename, filename_size, patterns[i], qpk_volume_root(qpk->volume), qpk->dir,
                   qpk->entry[0] ? qpk->entry : "app.js") >= filename_size)
        {
          continue;
        }

      file = fopen(filename, "rb");
      if (file == NULL)
        {
          continue;
        }

      if (fseek(file, 0, SEEK_END) < 0 || (size = ftell(file)) <= 0 ||
          (uintmax_t)size >= SIZE_MAX || fseek(file, 0, SEEK_SET) < 0)
        {
          fclose(file);
          continue;
        }

      source = malloc((size_t)size + 1);
      if (source == NULL)
        {
          fclose(file);
          return NULL;
        }

      got = fread(source, 1, (size_t)size, file);
      fclose(file);
      if (got != (size_t)size)
        {
          free(source);
          continue;
        }

      source[got] = '\0';
      *source_size = got;
      return source;
    }

  return NULL;
}

static void qpk_clicked(lv_event_t *e);
static void glass_chat_back(lv_event_t *e);

static void qapp_back(lv_event_t *e)
{
  LV_UNUSED(e);
  if(g_camera_album_page==g_desktop.current_card&&g_camera_album_page){launch_builtin_camera_qapp(NULL);return;}
  if (chat_qpk_preview_active) { chat_qpk_preview_back(); return; }
  if (g_desktop.chat_page) { glass_chat_back(NULL); return; }
  if (g_desktop.qapp_page && g_desktop.qapp_return_apps)
    qpk_clicked(NULL);
  else
    panel_hide(NULL);
}

/* Roll back a staged page without retiring the native page underneath it. */
static void qapp_page_discard(lv_obj_t *page)
{
  qpk_runtime_stop();
  camera_page_restore();
  if (page) lv_obj_delete(page);
  if (!g_desktop.current_card)
    lv_obj_add_flag(g_desktop.panel, LV_OBJ_FLAG_HIDDEN);
  chat_display_resume();
}

static lv_obj_t *qapp_page_prepare(const char *title)
{
  lv_obj_t *page;
  lv_obj_t *content;
  lv_obj_t *back;
  lv_obj_t *label;
  int height;

  chat_qpk_preview_finish();
  g_camera_launch_pending = false;
  qpk_runtime_stop();
#ifdef CONFIG_SYSTEM_BILIBILI_LIVE
  bili_live_stop();
#endif
  desktop_transients_reset();
  lv_obj_remove_flag(g_desktop.panel, LV_OBJ_FLAG_HIDDEN);

  page = lv_obj_create(g_desktop.panel);
  if (page == NULL)
    {
      qapp_page_discard(NULL);
      return NULL;
    }

  lv_obj_set_style_opa(page, LV_OPA_TRANSP, 0);
  lv_obj_set_size(page, lv_pct(100), lv_pct(100));
  style_panel(page, theme_card());
  lv_obj_set_style_radius(page, 0, 0);
  lv_obj_set_style_border_width(page, 0, 0);
  lv_obj_set_style_pad_all(page, 0, 0);
  lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);

  back = lv_button_create(page);
  if (back == NULL)
    {
      qapp_page_discard(page);
      return NULL;
    }

  lv_obj_set_size(back, 52, 42);
  lv_obj_set_pos(back, 12, 10);
  style_button(back, theme_surface());
  lv_obj_add_event_cb(back, qapp_back, LV_EVENT_CLICKED, NULL);
  label = lv_label_create(back);
  if (label == NULL)
    {
      qapp_page_discard(page);
      return NULL;
    }

  lv_label_set_text(label, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_font(label, glass_icon_font(24), 0);
  lv_obj_set_style_text_color(label, lv_color_hex(theme_primary()), 0);
  lv_obj_center(label);

  label = make_label(page, title, theme_primary(), 28);
  if (label == NULL)
    {
      qapp_page_discard(page);
      return NULL;
    }

  lv_obj_set_pos(label, 78, 15);

  content = lv_obj_create(page);
  if (content == NULL)
    {
      qapp_page_discard(page);
      return NULL;
    }

  lv_obj_update_layout(page);
  height = lv_obj_get_height(page) - QAPP_HEADER_HEIGHT;
  lv_obj_set_size(content, lv_pct(100), height);
  lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(content, lv_color_hex(theme_card()), 0);
  lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(content, 0, 0);
  lv_obj_set_style_radius(content, 0, 0);
  lv_obj_set_style_pad_all(content, 0, 0);
  lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

  return content;
}

/* HA supplies its own toolbar. Keep native back navigation in the sidebar
 * instead of reserving a second header above the entire quick app. */
static bool qapp_homeassistant_sidebar_header(lv_obj_t *content)
{
  lv_obj_t *page = lv_obj_get_parent(content);
  lv_obj_t *back = lv_obj_get_child(page, 0);
  lv_obj_t *title = lv_obj_get_child(page, 1);
  lv_obj_t *footer = lv_obj_create(page);
  if (!footer) return false;
  lv_obj_remove_style_all(footer);
  lv_obj_set_size(footer, 164, 64);
  lv_obj_align(footer, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_set_style_bg_color(footer, lv_color_hex(g_desktop.light_theme ? 0xffffff : 0x22252f), 0);
  lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, 0);
  lv_obj_remove_flag(footer, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(title, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_size(content, lv_pct(100), lv_pct(100));
  lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_parent(back, footer);
  lv_obj_set_pos(back, 12, 8);
  lv_obj_set_size(back, 140, 44);
  lv_obj_t *caption = lv_obj_get_child(back, 0);
  lv_label_set_text(caption, "返回");
  lv_obj_set_style_text_font(caption, zh_font(16), 0);
  lv_obj_center(caption);
  return true;
}

static void qapp_page_commit(lv_obj_t *content)
{
  lv_obj_t *page = lv_obj_get_parent(content);
  lv_obj_t *previous = g_desktop.current_card;

  /* Commit the origin only after launch succeeds, before retiring its page.
   * A replacement app inherits the original entry point. */
  if (!g_desktop.qapp_page)
    g_desktop.qapp_return_apps = g_desktop.apps_page ||
      (g_desktop.chat_page && g_desktop.chat_return_apps);
  g_desktop.apps_page = false;
  g_desktop.chat_page = false;
  g_desktop.chat_return_apps = false;
  g_desktop.qapp_page = true;

  if (previous != NULL && previous != page)
    {
      lv_obj_delete(previous);
    }

  lv_obj_set_style_bg_color(g_desktop.panel,
                            lv_color_hex(theme_card()), 0);
  lv_obj_set_style_bg_opa(g_desktop.panel, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(g_desktop.panel, 0, 0);
  lv_obj_set_style_opa(page, LV_OPA_COVER, 0);
  g_desktop.current_card = page;
  /* Camera owns the framebuffer during capture; leave that path immediate. */
  if(!qpk_runtime_camera_busy())glass_motion_enter(page,true,0);
}

static void launch_builtin_qapp(lv_event_t *e)
{
  lv_obj_t *card;
  const struct qpk_entry_s *manifest = &g_builtin_qpk.manifest;
  int ret;

  LV_UNUSED(e);
  card = qapp_page_prepare(manifest->name);
  if (card == NULL)
    {
      show_toast("页面内存不足");
      return;
    }

  ret = qpk_runtime_launch(card, manifest->name, manifest->package,
                           manifest->version, manifest->entry,
                           g_hello_qpk_js, sizeof(g_hello_qpk_js) - 1,
                           zh_font, show_toast, qpk_show_dialog, qpk_show_unresponsive);
  if (ret == 0)
    {
      qapp_page_commit(card);
    }
  else
    {
      qapp_page_discard(lv_obj_get_parent(card));
    }
}

static void launch_builtin_2048_qapp(lv_event_t *e)
{
  lv_obj_t *card;
  const struct qpk_entry_s *manifest = &g_builtin_2048_qpk.manifest;
  int ret;

  LV_UNUSED(e);
  card = qapp_page_prepare(manifest->name);
  if (card == NULL)
    {
      show_toast("页面内存不足");
      return;
    }

  ret = qpk_runtime_launch(card, manifest->name, manifest->package,
                           manifest->version, manifest->entry,
                           g_2048_qpk_js, sizeof(g_2048_qpk_js) - 1,
                           zh_font, show_toast, qpk_show_dialog, qpk_show_unresponsive);
  if (ret == 0)
    {
      qapp_page_commit(card);
    }
  else
    {
      qapp_page_discard(lv_obj_get_parent(card));
    }
}

static void launch_builtin_ouo_qapp(lv_event_t *e)
{
  glass_chat_ball_open(e);
}

static void launch_builtin_espdl_qapp(lv_event_t *e)
{
  LV_UNUSED(e);
  extern const char *espdl_get_app_js(unsigned int *len);
  unsigned int length;
  const char *source=espdl_get_app_js(&length);
  lv_obj_t *card=qapp_page_prepare("ESP-DL 离线识别");
  if (!card) {show_toast("页面内存不足");return;}
  int ret=qpk_runtime_launch(card,"ESP-DL 离线识别","org.openvela.espdl","0.1.0",
                             "app.js",source,length,zh_font,show_toast,qpk_show_dialog, qpk_show_unresponsive);
  if (!ret) qapp_page_commit(card);
  else qapp_page_discard(lv_obj_get_parent(card));
}

static void launch_builtin_camera_qapp(lv_event_t *e)
{
  lv_obj_t *card;
  const struct qpk_entry_s *manifest = &g_builtin_camera_qpk.manifest;
  extern const char *camera_get_app_js(unsigned int *len);
  unsigned int source_size;
  const char *source;
  int ret;

  if (e != NULL)
    {
      /* Finish this LVGL pass before giving its draw pages to capture. */
      g_camera_launch_pending = true;
      return;
    }

  if (qpk_runtime_camera_busy())
    {
      /* The old page/JS may retire now; its hardware has a separate lifetime. */
      qpk_runtime_stop();
#ifdef CONFIG_SYSTEM_BILIBILI_LIVE
  bili_live_stop();
#endif
      g_camera_launch_pending = true;
      return;
    }

  g_camera_launch_pending = false;
  source = camera_get_app_js(&source_size);
  if (source == NULL || source_size == 0)
    {
      show_toast("应用资源不可用");
      return;
    }
  card = qapp_page_prepare(manifest->name);
  if (card == NULL)
    {
      show_toast("页面内存不足");
      return;
    }

  /* Camera alone owns the full display; normal pages keep their safe area. */
  lv_obj_t *page=lv_obj_get_parent(card);
  g_camera_page.page=page;
  g_camera_page.x=lv_obj_get_x(g_desktop.panel);g_camera_page.y=lv_obj_get_y(g_desktop.panel);
  g_camera_page.w=lv_obj_get_width(g_desktop.panel);g_camera_page.h=lv_obj_get_height(g_desktop.panel);
  g_camera_page.screen_x=lv_obj_get_x(g_desktop.screen);g_camera_page.screen_y=lv_obj_get_y(g_desktop.screen);
  g_camera_page.screen_w=lv_obj_get_width(g_desktop.screen);g_camera_page.screen_h=lv_obj_get_height(g_desktop.screen);
  g_camera_page.status_hidden=!g_desktop.statusbar||lv_obj_has_flag(g_desktop.statusbar,LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_pos(g_desktop.screen,0,0);lv_obj_set_size(g_desktop.screen,lv_display_get_horizontal_resolution(NULL),lv_display_get_vertical_resolution(NULL));
  lv_obj_set_pos(g_desktop.panel,0,0);lv_obj_set_size(g_desktop.panel,lv_display_get_horizontal_resolution(NULL),lv_display_get_vertical_resolution(NULL));
  if(g_desktop.statusbar)lv_obj_add_flag(g_desktop.statusbar,LV_OBJ_FLAG_HIDDEN);
  for(unsigned i=0;i<lv_obj_get_child_count(page);i++){
    lv_obj_t *child=lv_obj_get_child(page,i);if(child!=card)lv_obj_add_flag(child,LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_set_size(card,lv_pct(100),lv_pct(100));lv_obj_set_pos(card,0,0);
  lv_obj_add_event_cb(page,camera_page_deleted,LV_EVENT_DELETE,NULL);
  lv_obj_update_layout(page);

  ret = qpk_runtime_launch(card, manifest->name, manifest->package,
                           manifest->version, manifest->entry,
                           source, source_size, zh_font,
                           show_toast, qpk_show_dialog, qpk_show_unresponsive);
  if (ret == 0)
    {
      qapp_page_commit(card);
      lv_obj_t *close=lv_button_create(page);
      if(close){
      lv_obj_set_pos(close,lv_obj_get_width(page)-36,lv_obj_get_height(page)/2-90);
      lv_obj_set_size(close,36,180);style_button(close,0x000000);
      lv_obj_set_style_bg_opa(close,LV_OPA_TRANSP,0);
      lv_obj_set_style_radius(close,32,0);lv_obj_set_style_border_width(close,0,0);
      lv_obj_add_event_cb(close,qapp_back,LV_EVENT_CLICKED,NULL);
      lv_obj_t *icon=lv_label_create(close);if(icon){lv_label_set_text(icon,LV_SYMBOL_CLOSE);
      lv_obj_set_style_text_font(icon,glass_icon_font(24),0);
      lv_obj_set_style_text_color(icon,lv_color_hex(0xffffff),0);
      lv_obj_set_style_text_opa(icon,LV_OPA_TRANSP,0);lv_obj_center(icon);
      }}
    }
  else
    {
      camera_page_restore();
      qapp_page_discard(lv_obj_get_parent(card));
    }
}

static void launch_builtin_recorder_qapp(lv_event_t *e)
{
  LV_UNUSED(e);
  extern const char *recorder_get_app_js(unsigned int *len);
  unsigned int length;
  const char *source = recorder_get_app_js(&length);
  lv_obj_t *card = qapp_page_prepare("录音机");
  if (!card) { show_toast("页面内存不足"); return; }
  int ret = qpk_runtime_launch(card, "录音机", "com.openvela.recorder", "1.0.0",
                               "builtin:/recorder/app.js", source, length,
                               zh_font, show_toast, qpk_show_dialog, qpk_show_unresponsive);
  if (!ret) qapp_page_commit(card);
  else qapp_page_discard(lv_obj_get_parent(card));
}

static void launch_builtin_dafeiyu_qapp(lv_event_t *e)
{
  LV_UNUSED(e);
  extern const char *dafeiyu_get_app_js(unsigned int *len);
  unsigned int length;
  const char *source = dafeiyu_get_app_js(&length);
  if (source == NULL || length == 0) { show_toast("应用资源不可用"); return; }
  lv_obj_t *card = qapp_page_prepare("大肥鱼桌宠");
  if (!card)
    {
      show_toast("页面内存不足");
      return;
    }

  int ret = qpk_runtime_launch(card, "大肥鱼桌宠", "org.flash.dafeiyu", "1.0.1",
                               "builtin:/dafeiyu/app.js", source, length,
                               zh_font, show_toast, qpk_show_dialog, qpk_show_unresponsive);
  if (!ret) qapp_page_commit(card);
  else qapp_page_discard(lv_obj_get_parent(card));
}

static void glass_homeassistant(lv_event_t *e);

static void launch_builtin_homeassistant_qapp(lv_event_t *e)
{
  lv_obj_t *card;
  const struct qpk_entry_s *manifest =
    &g_builtin_homeassistant_qpk.manifest;
  extern const char *homeassistant_get_app_js(unsigned int *len);
  unsigned int source_size;
  const char *source;
  int ret;

  LV_UNUSED(e);
  source = homeassistant_get_app_js(&source_size);
  if (source == NULL || source_size == 0)
    {
      show_toast("应用资源不可用");
      return;
    }
  card = qapp_page_prepare(manifest->name);
  if (card == NULL)
    {
      show_toast("页面内存不足");
      return;
    }

  if (!qapp_homeassistant_sidebar_header(card))
    {
      qapp_page_discard(lv_obj_get_parent(card));
      show_toast("页面内存不足");
      return;
    }

  ret = qpk_runtime_launch(card, manifest->name, manifest->package,
                           manifest->version, manifest->entry,
                           source, source_size, hass_qpk_font,
                           show_toast, qpk_show_dialog, qpk_show_unresponsive);
  if (ret == 0)
    {
      qapp_page_commit(card);
    }
  else
    {
      qapp_page_discard(lv_obj_get_parent(card));
    }
}

static void launch_external_qapp(intptr_t index)
{
  struct qpk_draft_info operation; glass_qpk_get(&operation);
  if (operation.removing) { show_toast("正在删除应用，请稍候"); return; }
  struct qpk_entry_s *qpk;
  lv_obj_t *card;
  lv_obj_t *label;
  char *source;
  char filename[256];
  char text[256];
  size_t source_size = 0;

  if (index < 0 || index >= g_desktop.nqpk)
    {
      return;
    }

  qpk = &g_desktop.qpk[index];
  card = qapp_page_prepare(qpk->name);
  if (card == NULL)
    {
      show_toast("页面内存不足");
      return;
    }

  source = qpk_load_entry(qpk, filename, sizeof(filename), &source_size);
  if (source != NULL)
    {
      int ret = qpk_runtime_launch(card, qpk->name, qpk->package,
                                   qpk->version, filename, source,
                                   source_size, zh_font, show_toast,
                                   qpk_show_dialog, qpk_show_unresponsive);
      free(source);
      if (ret == 0)
        {
          qapp_page_commit(card);
          return;
        }
    }

  snprintf(text, sizeof(text),
           "包名：%s\n版本：%s\n入口：%s\n目录：%s/%s\n\n"
           "无法加载 JavaScript 入口。当前运行时支持 .js QPK；"
           "MicroReactor 风格 .ux 解析器将在下一阶段接入。",
           qpk->package[0] ? qpk->package : "未声明",
           qpk->version[0] ? qpk->version : "未声明",
           qpk->entry[0] ? qpk->entry : "未声明", qpk_volume_root(qpk->volume), qpk->dir);
  label = make_label(card, text, theme_secondary(), 20);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(label, 690);
  lv_obj_align(label, LV_ALIGN_TOP_LEFT, 24, 76);
  qapp_page_commit(card);
}

static void external_qapp_clicked(lv_event_t *e)
{
  launch_external_qapp((intptr_t)lv_event_get_user_data(e));
}

static void qpk_clicked(lv_event_t *e)
{
  glass_apps(e);
}

static void settings_clicked(lv_event_t *e);

static void settings_rebuild_async(void *data)
{
  LV_UNUSED(data);
  if (!g_desktop.settings_rebuild_pending) return;
  g_desktop.settings_rebuild_pending = false;
  glass_settings(NULL);
}

static void theme_changed(lv_event_t *e)
{
  lv_obj_t *sw = lv_event_get_target(e);

  g_desktop.light_theme = lv_obj_has_state(sw, LV_STATE_CHECKED);
  apply_theme();
  glass_save_preferences();
  if (!g_desktop.settings_rebuild_pending &&
      lv_async_call(settings_rebuild_async, NULL) == LV_RESULT_OK)
    {
      g_desktop.settings_rebuild_pending = true;
    }
}

static void settings_clicked(lv_event_t *e)
{
  LV_UNUSED(e);
  if (!g_desktop.settings_rebuild_pending &&
      lv_async_call(settings_rebuild_async, NULL) == LV_RESULT_OK)
    g_desktop.settings_rebuild_pending = true;
}

#ifdef CONFIG_SYSTEM_BILIBILI_LIVE
static void launch_local_video(lv_event_t *e)
{
  LV_UNUSED(e);
  lv_obj_t *card = qapp_page_prepare("相册");
  if (card) { local_album_show(card, zh_font(16)); qapp_page_commit(card); }
}
static void launch_bilibili(lv_event_t *e)
{
  LV_UNUSED(e);
  lv_obj_t *card = qapp_page_prepare("网络直播");
  if (card) { bili_live_show(card, zh_font(18)); qapp_page_commit(card); }
}
#endif
static void about_clicked(lv_event_t *e);

static void camera_album_deleted(lv_event_t *e)
{if(g_camera_album_page==lv_event_get_target(e))g_camera_album_page=NULL;}
static void desktop_camera_album_service(void)
{
  if(!qpk_runtime_camera_take_album())return;
#ifdef CONFIG_SYSTEM_BILIBILI_LIVE
  lv_obj_t *card=qapp_page_prepare("相册");if(!card)return;
  local_album_show_camera(card,zh_font(16));
  qapp_page_commit(card);g_camera_album_page=lv_obj_get_parent(card);
  lv_obj_add_event_cb(g_camera_album_page,camera_album_deleted,LV_EVENT_DELETE,NULL);
#else
  show_toast("当前配置未启用相册");
#endif
}

#include "glass_ui.inc"
#include "glass_wake_mini.inc"
#include "glass_ui_probe.inc"
#include "glass_board_ui.inc"

static void desktop_camera_request(enum desktop_camera_command_e command)
{
  /* NSH producers never touch LVGL. The last command replaces pending input. */
  pthread_mutex_lock(&g_camera_command_lock);
  g_camera_command = command;
  pthread_mutex_unlock(&g_camera_command_lock);
}

static uint32_t desktop_loop_once(void)
{
  enum desktop_camera_command_e command;
  uint32_t idle = 10;
#ifdef GLASS_LOCAL_SPEECH
  /* Reuse the UI loop rather than add a housekeeping thread/timer. */
  static uint32_t speech_cache_at;
  uint32_t now=lv_tick_get();
  if((uint32_t)(now-speech_cache_at)>=1000){
    speech_cache_at=now;glass_speech_trim_cache();
  }
#endif

  if(glass_ai_config_ui_pending()){
    /* Only the desktop task may retire LVGL session owners. Ack even when
     * another page is visible; this barrier must not depend on UI timers. */
    chat_ball_stop();chat_ball_local_stop();g_chat_ui.external_result_turn=0;
    g_wake_mini.voice_owned=g_wake_mini.speech_owned=g_wake_mini.result_pending=false;
    g_wake_mini.pending_prompt[0]=0;g_wake_mini.turn=0;
#ifdef GLASS_LOCAL_SPEECH
    struct speech_snapshot speech;glass_speech_get(&speech);
    g_wake_mini.event=speech.wake_event;g_wake_mini.command_event=speech.command_event;
    g_speech_ui.recording=false;g_speech_ui.turn=0;
#endif
    wake_mini_hide();glass_ai_config_ui_ack();
  }

  int font_change=glass_sd_font_poll();
  if(font_change) {
    lv_font_t *primary[]={g_desktop.font16,g_desktop.font20,g_desktop.font28,
                          g_glass.font48,g_glass.music_font72};
    const unsigned pixels[]={16,20,28,48,72};
    for(unsigned i=0;i<5;i++)if(primary[i]) {
      lv_font_t *fallback=glass_sd_font_get(pixels[i]);
      const lv_font_t *icons=i<3?glass_icon_font(i==2?24:pixels[i]):NULL;
      if(fallback)fallback->fallback=icons;
      primary[i]->fallback=fallback?fallback:icons;
    }
    lv_obj_report_style_change(NULL);
    if(g_music_ui.full)g_music_ui.full->pv.valid=false;
    lv_obj_invalidate(lv_screen_active());
  }
  desktop_ui_probe_service();
  glass_board_ui_service();
  if(!glass_ai_config_active())wake_mini_service();
  if(!qpk_runtime_camera_active()){
    char notice[256];if(glass_assistant_take_notice(notice,sizeof(notice)))show_toast(notice);
  }

#ifdef CONFIG_SYSTEM_C6_DESKTOP
  /* Network results arrive from a worker thread; the desktop shows them. */
  {
    const char *notice = glass_net_take_notice();
    if (notice != NULL)
      {
        show_toast(notice);
        lv_obj_invalidate(lv_screen_active());
      }
  }
#endif

  /* This service must run even when capture owns all display pages and the
   * JS/LVGL timers are paused. It also recovers a failed capture worker.
   */
  if (qpk_runtime_camera_poll())
    {
      lv_obj_invalidate(lv_screen_active());
    }

  pthread_mutex_lock(&g_camera_command_lock);
  command = g_camera_command;
  g_camera_command = DESKTOP_CAMERA_NONE;
  pthread_mutex_unlock(&g_camera_command_lock);

  if (command == DESKTOP_CAMERA_STOP)
    {
      panel_hide(NULL);
      lv_obj_invalidate(lv_screen_active());
    }
  else if (command == DESKTOP_CAMERA_LAUNCH)
    {
      launch_builtin_camera_qapp(NULL);
    }
  else if(command>=DESKTOP_CAMERA_VIDEO_MODE&&command<=DESKTOP_CAMERA_VIDEO_STOP)
    {
      int ret=qpk_runtime_camera_video_action(command-DESKTOP_CAMERA_VIDEO_MODE);
      printf("desktop: camera video action=%u result=%d\n",command-DESKTOP_CAMERA_VIDEO_MODE,ret);
    }

  if (g_camera_launch_pending && !qpk_runtime_camera_busy())
    {
      launch_builtin_camera_qapp(NULL);
    }

  desktop_camera_album_service();
  if (!qpk_runtime_camera_active())
    {
      idle = lv_timer_handler();
    }

  /* Bound NSH command latency even when LVGL has no timer ready. */
  return idle > 100 ? 100 : (idle ? idle : 1);
}

#ifndef GLASS_HOST_TEST

#include "glass_test_storage.inc"

int main(int argc, FAR char *argv[])
{
  glass_board_music_register();
  glass_board_assistant_register();
#ifdef CONFIG_SYSTEM_BILIBILI_LIVE
  if(argc==2&&!strcmp(argv[1],"cpu-status")){bili_cpu_report();return 0;}
  if(argc==3&&!strcmp(argv[1],"media-fps")){
    char *end;unsigned long fps=strtoul(argv[2],&end,10);
    if(!*argv[2]||*end||fps>20)return -EINVAL;
    return bili_cpu_set_target_fps((unsigned)fps);
  }
  if(argc==2&&!strcmp(argv[1],"ppa-test"))return bili_ppa_test()<0?1:0;
  if(argc==3&&!strcmp(argv[1],"ppa-mode")&&(!strcmp(argv[2],"0")||!strcmp(argv[2],"1"))){bili_ppa_enable(argv[2][0]=='1');return 0;}
  if(argc==4&&!strcmp(argv[1],"decode-bench")){
    char *end;unsigned long features=strtoul(argv[3],&end,10);
    if(!*argv[3]||*end||features>3)return -EINVAL;
    return local_video_decode_benchmark(argv[2],(unsigned)features)<0?1:0;
  }
#endif
#if defined(CONFIG_BOARDCTL_MKRD) && defined(CONFIG_FS_FAT)
  if (argc == 2 && !strcmp(argv[1], "test-disk"))
    return desktop_test_disk();
#endif
  if (argc == 4 && !strcmp(argv[1], "receive"))
    return desktop_receive(argv[2], argv[3]);
  if (argc > 1 && !strcmp(argv[1], "hardware-test")) {
    extern int qpk_hw_probe(bool);
    return qpk_hw_probe(argc > 2 && !strcmp(argv[2], "audio"));
  }
  if(argc>=4&&!strcmp(argv[1],"hardware-call")){
    extern int qpk_hw_call(int,char **);
    return qpk_hw_call(argc-2,argv+2);
  }
#ifdef GLASS_LOCAL_SPEECH
  if(argc>1&&!strcmp(argv[1],"speech")){
    /* A temporary NSH task must not own the recognition worker. */
    if(argc==3&&!strcmp(argv[2],"listen"))return desktop_ui_probe_request("speech-listen");
    if(argc==3&&!strcmp(argv[2],"probe"))return desktop_ui_probe_request("speech-probe");
    if(argc==3&&!strcmp(argv[2],"nano-scalar"))return desktop_ui_probe_request("speech-nano-scalar");
    if(argc==3&&!strcmp(argv[2],"nano-pie"))return desktop_ui_probe_request("speech-nano-pie");
    if(argc==3&&!strcmp(argv[2],"nano-play"))return desktop_ui_probe_request("speech-nano-play");
    if(argc==3&&!strcmp(argv[2],"esp-tts"))return desktop_ui_probe_request("speech-esp-tts");
    if(argc==3&&!strcmp(argv[2],"esp-tts-play"))return desktop_ui_probe_request("speech-esp-tts-play");
    if(argc==3&&!strcmp(argv[2],"esp-tts-export"))return desktop_ui_probe_request("speech-esp-tts-export");
    if(argc==3&&!strcmp(argv[2],"p4tts-scalar"))return desktop_ui_probe_request("speech-p4tts-scalar");
    if(argc==3&&!strcmp(argv[2],"p4tts-pie"))return desktop_ui_probe_request("speech-p4tts-pie");
    if(argc==3&&!strcmp(argv[2],"p4tts-dual"))return desktop_ui_probe_request("speech-p4tts-dual");
    if(argc==3&&!strcmp(argv[2],"zh-q8"))return desktop_ui_probe_request("speech-zh-q8");
    if(argc==3&&!strcmp(argv[2],"zh-q8-play"))return desktop_ui_probe_request("speech-zh-q8-play");
    if(argc==3&&!strcmp(argv[2],"zh-q8-pie"))return desktop_ui_probe_request("speech-zh-q8-pie");
    if(argc==3&&!strcmp(argv[2],"zh-q8-dual"))return desktop_ui_probe_request("speech-zh-q8-dual");
    if(argc==4&&!strcmp(argv[2],"say")){
      char request[80];int n=snprintf(request,sizeof(request),"speech-say:%s",argv[3]);
      if(n<0||n>=(int)sizeof(request))return -ENAMETOOLONG;
      return desktop_ui_probe_request(request);
    }
    return glass_speech_cli(argc,argv);
  }
#endif
  if (argc>1 && !strcmp(argv[1],"espdl")) {
    extern int qpk_dl_probe(int,char **);
    return qpk_dl_probe(argc,argv);
  }
  if(argc==2&&!strcmp(argv[1],"interfaces")){
    char *text=qpk_hw_capabilities();if(!text)return -ENOMEM;puts(text);free(text);return 0;
  }
  if(argc==3&&!strcmp(argv[1],"camera-agent")){
    atomic_bool cancel=false;cJSON *args=glass_agent_json(argv[2]);if(!args)return -EINVAL;
    const cJSON *operation=cJSON_GetObjectItemCaseSensitive(args,"operation");
    bool capture=cJSON_IsString(operation)&&!strcmp(operation->valuestring,"capture");
    bool recognize=cJSON_IsString(operation)&&!strcmp(operation->valuestring,"recognize");
    char *text=glass_assistant_platform("assistant_camera",args,&cancel);cJSON_Delete(args);
    if(!text)return -ENOMEM;
    puts(text);cJSON *reply=glass_agent_json(text);free(text);
    const cJSON *ticket=reply?cJSON_GetObjectItemCaseSensitive(reply,"request_id"):NULL;
    unsigned request=cJSON_IsNumber(ticket)?ticket->valueint:0;
    bool accepted=reply&&cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply,"accepted"));cJSON_Delete(reply);
    /* NuttX kills a CLI task's pthreads on exit. Retain the diagnostic owner
     * until the USB worker has closed its stream, even after cancellation. */
    if(accepted&&request&&(capture||recognize)){
      struct qpk_dl_result status;qpk_dl_status(&status);
      unsigned worker=status.request,ticks=0;
      do{usleep(100000);qpk_dl_status(&status);
        if(++ticks==200)qpk_dl_cancel_request(worker);
      }while(status.busy&&status.request==worker);
      args=cJSON_CreateObject();if(!args)return -ENOMEM;
      cJSON_AddStringToObject(args,"operation",recognize?"recognition_status":"photo_status");
      cJSON_AddNumberToObject(args,"request_id",request);
      text=glass_assistant_platform("assistant_camera",args,&cancel);cJSON_Delete(args);
      if(!text)return -ENOMEM;
      puts(text);free(text);
    }
    return 0;
  }
  if (argc > 1 && strcmp(argv[1], "camera-sample") == 0)
    {
      /* Acquisition-only diagnostic; never submits camera data to a model. */
      struct qpk_camera_clip clip={0};atomic_bool abort=false;
      int ret=qpk_camera_clip_capture(&clip,&abort);
      printf("[camera-sample] result=%d frames=%u duration_ms=%lu camera_closed=1\n",ret,clip.count,(unsigned long)clip.duration_ms);
      for(unsigned i=0;i<clip.count;i++)printf("[camera-sample] frame=%u offset_ms=%lu jpeg_bytes=%u\n",i+1,(unsigned long)clip.frames[i].offset_ms,(unsigned)clip.frames[i].size);
      qpk_camera_clip_free(&clip);return ret?1:0;
    }
#ifdef CONFIG_ESP32P4_DSI_FLASH_RELOAD
  if (argc > 1 && strcmp(argv[1], "scanout-hold") == 0)
    {
      extern int esp_mipi_dsi_flash_probe(void);
      return esp_mipi_dsi_flash_probe();
    }
#endif
#ifdef CONFIG_ESP32P4_PSRAM_XIP
  if (argc == 2 && strcmp(argv[1], "xip") == 0)
    {
      extern void esp32p4_psram_xip_status(void);
      esp32p4_psram_xip_status();
      return 0;
    }
#endif
  if (argc > 1 && strcmp(argv[1], "cameras") == 0)
    {
      extern int qpk_camera_probe(int, char **);
      return qpk_camera_probe(argc, argv);
    }
  lv_nuttx_dsc_t info;
  lv_nuttx_result_t result;
  bool launch_camera;
  bool stop_camera;
#ifdef CONFIG_SYSTEM_NSH
  extern int nsh_main(int argc, FAR char *argv[]);
#endif
#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_TOUCHSCREEN
  extern int board_touch_initialize(void);
#endif
#ifdef CONFIG_SYSTEM_HASS
  /* Register the in-process smart home MCP tools before any AI tool call. */
  glass_board_home_register();
#endif

  if (argc > 1 && strcmp(argv[1], "storage-test") == 0)
    {
      return qpk_storage_selftest() < 0 ? 1 : 0;
    }

  /* Physical USB diagnostics: return the same short-lived code shown by the
   * portal page. The installer consumes it privately; debug logs never do. */
  if (argc == 2 && strcmp(argv[1], "portal-pair") == 0)
    {
      /* A temporary NSH task must not own a long-lived portal pthread.
       * Start it through the persistent desktop task, just like the page. */
      char code[PORTAL_CODE_LENGTH+1];int ret=desktop_ui_probe_request("portal");
      if(!ret)ret=portal_session_open(code,sizeof(code));
      if(!ret)printf("PORTAL_PAIR=%s\n",code);memset(code,0,sizeof(code));return ret;
    }
  if (argc == 3 && strcmp(argv[1], "grok-verify") == 0)
    {
      extern int qpk_dl_sha256_file(const char *,char[65]);
      const char *names[]={"home-white.gbs","home-black.gbs","mini-white.gbs","app-white.gbs","app-black.gbs"};
      bool allowed=false;for(unsigned i=0;i<5;i++)if(!strcmp(argv[2],names[i]))allowed=true;
      if(!allowed)return 1;char path[96],digest[65];snprintf(path,sizeof(path),"/sdcard/grok/%s",argv[2]);
      int ret=qpk_dl_sha256_file(path,digest);if(ret)printf("GROK_VERIFY error=%d\n",ret);else printf("GROK_VERIFY %s %s\n",argv[2],digest);return ret;
    }
  if ((argc == 2 && !strcmp(argv[1],"frame-export")) ||
      (argc == 3 && !strcmp(argv[1],"frame-chunk")))
    {
      /* The immutable snapshot is produced by the UI thread first. Export
       * never touches live LVGL objects or stops the animation renderer. */
      size_t size=g_ui_probe_frame_bytes;const uint8_t *data=(const uint8_t *)g_ui_probe_frame;
      if(!data||!size)return 1;
      size_t start=0,end=size;if(argc==3){char *tail;start=strtoul(argv[2],&tail,10);if(*tail||start>=size||start%64)return 1;end=start+64;if(end>size)end=size;}
      printf("FRAME_DATA %lu\n",(unsigned long)size);
      for(size_t off=start;off<end;off+=64){char line[150];int pos=snprintf(line,sizeof(line),"F%08lx:",(unsigned long)off);size_t n=end-off<64?end-off:64;
        for(size_t i=0;i<n;i++){static const char hex[]="0123456789abcdef";line[pos++]=hex[data[off+i]>>4];line[pos++]=hex[data[off+i]&15];}
        line[pos]=0;puts(line);fflush(stdout);usleep(1500);
      }
      puts("FRAME_END");return 0;
    }
  if (argc == 3 && strcmp(argv[1], "ui") == 0)
    return desktop_ui_probe_request(argv[2]);

  launch_camera = argc > 1 && strcmp(argv[1], "camera") == 0;
  stop_camera = argc > 1 && strcmp(argv[1], "camera-stop") == 0;
  if (lv_is_initialized())
    {
      if(argc==2&&(!strcmp(argv[1],"camera-video-mode")||!strcmp(argv[1],"camera-video-start")||!strcmp(argv[1],"camera-video-stop")))
        {
          desktop_camera_request(!strcmp(argv[1],"camera-video-mode")?DESKTOP_CAMERA_VIDEO_MODE:
            !strcmp(argv[1],"camera-video-start")?DESKTOP_CAMERA_VIDEO_START:DESKTOP_CAMERA_VIDEO_STOP);
          return 0;
        }
      if (launch_camera)
        {
          desktop_camera_request(DESKTOP_CAMERA_LAUNCH);
          printf("desktop: camera diagnostic requested\n");
          return 0;
        }

      if (stop_camera)
        {
          desktop_camera_request(DESKTOP_CAMERA_STOP);
          printf("desktop: camera diagnostic stop requested\n");
          return 0;
        }

      return -1;
    }

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_TOUCHSCREEN
  if (board_touch_initialize() < 0)
    {
      fprintf(stderr, "desktop: touch init failed\n");
    }
#endif

  lv_init();
  lv_nuttx_dsc_init(&info);
  info.fb_path = "/dev/fb0";
#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_TOUCHSCREEN
  info.input_path = "/dev/input0";
#else
  info.input_path = NULL;
#endif

#ifdef CONFIG_SYSTEM_BILIBILI_LIVE
  bili_cpu_register_ui();
#endif
  lv_nuttx_init(&info, &result);
  if (result.disp == NULL)
    {
      fprintf(stderr, "desktop: display init failed\n");
      return 1;
  }
  glass_ui_metrics_attach(result.disp);

#if defined(CONFIG_FS_SMARTFS) && !defined(CONFIG_DISABLE_MOUNTPOINT)
  /* Restore the existing user volume before any app loads preferences.
   * Never format or erase storage automatically. */
  if (mkdir("/data", 0777) < 0 && errno != EEXIST)
    fprintf(stderr, "desktop: data directory unavailable: %d\n", errno);
  else if (mount("/dev/smart0", "/data", "smartfs", 0, NULL) < 0 && errno != EBUSY)
    fprintf(stderr, "desktop: data volume not mounted: %d\n", errno);
#endif

  extern int qpk_photo_store_initialize(void);
  int photo_init=qpk_photo_store_initialize();
  if(photo_init<0)fprintf(stderr,"desktop: photo writer unavailable: %d\n",photo_init);
  desktop_ui_create();
#ifdef GLASS_LOCAL_SPEECH
  glass_speech_prepare();
#endif
  lv_obj_invalidate(lv_screen_active());
  lv_refr_now(result.disp);

#if defined(CONFIG_NETUTILS_NETINIT) && !defined(CONFIG_SYSTEM_NSH)
  /* netinit would run before the companion radio has registered wlan0, so the
   * boot task below waits for the station and then requests the lease. */
#endif

#ifdef CONFIG_SYSTEM_C6_DESKTOP
  glass_net_autostart();
  glass_weather_start();
  (void)glass_assistant_start();
#endif

#if defined(CONFIG_ESP32P4_SELECTS_REV_LESS_V3) && \
    !defined(CONFIG_LV_NUTTX_FBDEV_PARTIAL)
  {
    extern int esp32p4_lcd_prime_pageflip(void);

    /* Publish a complete desktop frame to both pages before input starts. */
    lv_refr_now(result.disp);
    (void)esp32p4_lcd_prime_pageflip();
  }
#endif

#ifdef CONFIG_SYSTEM_NSH
  if (task_create("nsh", 100, 4096, nsh_main, NULL) < 0)
    {
      fprintf(stderr, "desktop: failed to start NSH\n");
    }
#endif

  if (launch_camera)
    {
      printf("desktop: launching camera diagnostic\n");
      launch_builtin_camera_qapp(NULL);
    }

  while (1)
    {
      usleep(desktop_loop_once() * 1000);
    }

  return 0;
}
#endif /* GLASS_HOST_TEST */
