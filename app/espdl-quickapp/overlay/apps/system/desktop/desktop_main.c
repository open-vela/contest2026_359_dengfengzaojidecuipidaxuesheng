/****************************************************************************
 * apps/system/desktop/desktop_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Chinese phone-style launcher for ESP32-P4 Function-EV-Board.
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
#include <time.h>
#include <unistd.h>
#include <cJSON.h>

#include <nuttx/sched.h>
#include <lvgl/lvgl.h>
#ifdef CONFIG_NETUTILS_NETINIT
#  include <netutils/netinit.h>
#endif

#include "qpk_runtime.h"
#include "qpk_security.h"
#include "qpk_storage.h"
#include "qpk_limits.h"
#include "glass_qpk_builder.h"
#include "glass_portal.h"
#ifdef CONFIG_SYSTEM_HASS
#include "hass_portal.h"
#include "hass_service.h"
#endif
#include "glass_dashboard.h"
#include "pet_engine.h"
#include "pet_lvgl.h"

#define QPK_DIR       CONFIG_SYSTEM_DESKTOP_QPK_DIR
#define NAME_MAXLEN   48
#define PANEL_WIDTH   944
#define PANEL_HEIGHT  512
#define QAPP_HEADER_HEIGHT  64
/* Long Chinese chat pages exceed 64 unique glyphs even within one viewport.
 * Keep glyph metrics and rasterized bitmaps across scrolling in PSRAM. */
#define DESKTOP_FONT_CACHE_GLYPHS 2048

static bool chat_qpk_preview_active, chat_qpk_preview_origin;
static uint32_t chat_qpk_preview_revision;
static void chat_qpk_preview_back(void);

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

static const struct builtin_qpk_s g_builtin_ouo_qpk =
{
  .manifest =
    {
      .name = "OuO",
      .package = "ouo",
      .version = "1.0.0",
      .entry = "builtin:/ouo/app.js",
    },
  .kind = "互动表情",
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
};

static pthread_mutex_t g_camera_command_lock = PTHREAD_MUTEX_INITIALIZER;
static enum desktop_camera_command_e g_camera_command;
static bool g_camera_launch_pending; /* UI thread only. */

enum builtin_id_e
{
  APP_SETTINGS = 0,
  APP_HOME_ASSISTANT,
  APP_QPK,
  APP_ABOUT
};

static void settings_rebuild_async(void *data);
static void glass_save_preferences(void);

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

static uint32_t theme_primary(void)
{
  return g_desktop.light_theme ? 0x252d46 : 0xf4f2ff;
}

static uint32_t theme_secondary(void)
{
  return g_desktop.light_theme ? 0x616981 : 0xabb3cd;
}

static uint32_t theme_card(void)
{
  return g_desktop.light_theme ? 0xf5f6fd : 0x202641;
}

static uint32_t theme_surface(void)
{
  return g_desktop.light_theme ? 0xe9eafa : 0x303752;
}

static uint32_t theme_accent(void)
{
  return g_desktop.light_theme ? 0x5860bf : 0xb4a6ff;
}

static void style_button(lv_obj_t *button, uint32_t color)
{
  lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
  lv_obj_set_style_border_width(button, 1, 0);
  lv_obj_set_style_border_color(button, lv_color_hex(theme_secondary()), 0);
  lv_obj_set_style_border_opa(button, 45, 0);
  lv_obj_set_style_radius(button, 12, 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_set_style_transform_width(button, 0, LV_STATE_PRESSED);
  lv_obj_set_style_transform_height(button, 0, LV_STATE_PRESSED);
}

static void style_panel(lv_obj_t *panel, uint32_t color)
{
  lv_obj_set_style_bg_color(panel, lv_color_hex(color), 0);
  lv_obj_set_style_border_width(panel, 1, 0);
  lv_obj_set_style_border_color(panel, lv_color_hex(0x344144), 0);
  lv_obj_set_style_radius(panel, 24, 0);
  lv_obj_set_style_shadow_width(panel, 0, 0);
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
  dp = opendir(QPK_DIR);
  if (dp == NULL)
    {
      return;
    }

  while ((ent = readdir(dp)) != NULL)
    {
      struct qpk_entry_s candidate;
      struct qpk_entry_s *qpk = &candidate;

      if (ent->d_name[0] == '.' ||
          snprintf(path, sizeof(path), "%s/%s", QPK_DIR, ent->d_name) >=
          sizeof(path) || stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
        {
          continue;
        }

      memset(qpk, 0, sizeof(*qpk));
      if (!qpk_probe(path, qpk))
        {
          continue;
        }

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
  if (dialog != NULL)
    {
      lv_obj_delete(dialog);
    }
}

static void desktop_transients_reset(void)
{
  toast_cancel();
  dialog_cancel();

  if (g_desktop.settings_rebuild_pending)
    {
      lv_async_call_cancel(settings_rebuild_async, NULL);
      g_desktop.settings_rebuild_pending = false;
    }
}

static void panel_hide(lv_event_t *e)
{
  LV_UNUSED(e);
  chat_qpk_preview_finish();
  g_camera_launch_pending = false;
  qpk_runtime_stop();
  desktop_transients_reset();
  lv_obj_add_flag(g_desktop.panel, LV_OBJ_FLAG_HIDDEN);
  g_desktop.current_card = NULL;
  lv_obj_clean(g_desktop.panel);
  g_desktop.apps_page = false;
  g_desktop.chat_page = false;
  g_desktop.chat_return_apps = false;
  g_desktop.qapp_page = false;
  g_desktop.qapp_return_apps = false;
}

static lv_obj_t *panel_card(const char *title)
{
  lv_obj_t *card;
  lv_obj_t *close;
  lv_obj_t *label;

  chat_qpk_preview_finish();
  g_camera_launch_pending = false;
  qpk_runtime_stop();
  desktop_transients_reset();
  g_desktop.apps_page = false;
  g_desktop.chat_page = false;
  g_desktop.chat_return_apps = false;
  g_desktop.qapp_page = false;
  g_desktop.qapp_return_apps = false;
  lv_obj_remove_flag(g_desktop.panel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clean(g_desktop.panel);
  lv_obj_set_style_bg_color(g_desktop.panel, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(g_desktop.panel, LV_OPA_80, 0);
  lv_obj_set_style_pad_all(g_desktop.panel, 0, 0);

  card = lv_obj_create(g_desktop.panel);
  if (card == NULL)
    {
      return NULL;
    }

  g_desktop.current_card = card;
  lv_obj_set_size(card, PANEL_WIDTH, PANEL_HEIGHT);
  lv_obj_center(card);
  style_panel(card, theme_card());
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

  lv_obj_set_width(label, PANEL_WIDTH - 120);
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
  lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(theme_primary()), 0);
  lv_obj_center(label);
  return card;
}

static void toast_delete_cb(lv_timer_t *timer)
{
  if (g_desktop.toast_timer == timer)
    {
      lv_obj_t *toast = g_desktop.toast;

      g_desktop.toast_timer = NULL;
      g_desktop.toast = NULL;
      if (toast != NULL)
        {
          lv_obj_delete(toast);
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
  "ui.text(info.packageName + '  v' + info.versionName, left, 104, 16, ui.secondary);\n"
  "ui.text(info.name, left, 52, 28, ui.primary);\n"
  "ui.panel(left, 150, 400, 96, ui.surface, 16, 255);\n"
  "ui.number(String(launches), left + 20, 164, 80, 64, ui.primary);\n"
  "ui.text('累计启动次数', left + 120, 180, 20, ui.secondary);\n"
  "ui.button('Toast 提示', left, 274, 400, 54, () => {\n"
  "  prompt.showToast({message: '来自真正 QuickJS QPK 的问候'});\n"
  "}, 0x6677f5);\n"
  "ui.button('对话框', left, 344, 400, 54, () => {\n"
  "  prompt.dialog({title: 'QPK 运行时',\n"
  "    message: '界面由 JavaScript 创建，事件由 QuickJS 执行。'});\n"
  "}, ui.surface);\n"
  "let secs = 0;\n"
  "const tick = ui.text('已运行 0 秒', left, 426, 16, ui.secondary);\n"
  "setInterval(() => { secs++; ui.setText(tick, '已运行 ' + secs + ' 秒'); }, 1000);\n"
  "console.log('hello QPK initialized');\n";

static const char g_2048_qpk_js[] =
  "'use strict';\n"
  "const viewport = ui.getSize();\n"
  "const W = viewport.width, H = viewport.height;\n"
  "const controlsY = H - 48;\n"
  "const tileGap = 8;\n"
  "const boardSize = Math.min(W - 40, controlsY - 54);\n"
  "const tileSize = Math.floor((boardSize - 16 - tileGap * 3) / 4);\n"
  "const actualBoard = tileSize * 4 + tileGap * 3 + 16;\n"
  "const boardX = Math.floor((W - actualBoard) / 2);\n"
  "const boardY = 46;\n"
  "const scoreLabel = ui.text('分数 0', 20, 10, 20, 0x776e65);\n"
  "const bestLabel = ui.text('最高 0', 270, 10, 20, 0x776e65);\n"
  "ui.setSize(scoreLabel, 230, 32);\n"
  "ui.setSize(bestLabel, 360, 32);\n"
  "const statusLabel = ui.text('准备开始', W - 170, 12, 16, 0x776e65);\n"
  "ui.background(0xfaf8ef);\n"
  "const boardPanel = ui.panel(boardX, boardY, actualBoard, actualBoard, 0xbbada0, 10, 255);\n"
  "const tilePanels = [], tileLabels = [];\n"
  "for (let i = 0; i < 16; i++) {\n"
  "  const x = boardX + 8 + (i % 4) * (tileSize + tileGap);\n"
  "  const y = boardY + 8 + Math.floor(i / 4) * (tileSize + tileGap);\n"
  "  tilePanels.push(ui.panel(x, y, tileSize, tileSize, 0xcdc1b4, 7, 255));\n"
  "  tileLabels.push(ui.number('', x, y, tileSize, tileSize, 0x776e65));\n"
  "}\n"
  "const board = [];\n"
  "let score = 0;\n"
  "let best = 0;\n"
  "let gameOver = false;\n"
  "let won = false;\n"
  "try { const value = Number(system.storage.get('best') || 0); if (isFinite(value) && value > 0) best = Math.floor(value); } catch (e) {}\n"
  "function setText(handle, value) { ui.setText(handle, String(value)); }\n"
  "function tileColor(value) {\n"
  "  if (value === 2) return 0xeee4da;\n"
  "  if (value === 4) return 0xede0c8;\n"
  "  if (value === 8) return 0xf2b179;\n"
  "  if (value === 16) return 0xf59563;\n"
  "  if (value === 32) return 0xf67c5f;\n"
  "  if (value === 64) return 0xf65e3b;\n"
  "  if (value === 128) return 0xedcf72;\n"
  "  if (value === 256) return 0xedcc61;\n"
  "  if (value === 512) return 0xedc850;\n"
  "  if (value === 1024) return 0xedc53f;\n"
  "  if (value >= 2048) return 0xedc22e;\n"
  "  return 0xcdc1b4;\n"
  "}\n"
  "function textColor(value) { return value <= 4 ? 0x776e65 : 0xffffff; }\n"
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
  "const restart = ui.button('重新开始', 20, controlsY, 132, 42, reset, 0xf0a04b);\n"
  "const left = ui.button('左', W - 292, controlsY, 62, 42, function () { move('left'); }, 0x776e65);\n"
  "const up = ui.button('上', W - 224, controlsY, 62, 42, function () { move('up'); }, 0x776e65);\n"
  "const down = ui.button('下', W - 156, controlsY, 62, 42, function () { move('down'); }, 0x776e65);\n"
  "const right = ui.button('右', W - 88, controlsY, 62, 42, function () { move('right'); }, 0x776e65);\n"
  "ui.onSwipe(function (direction) { move(direction); });\n"
  "reset();\n";

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

  for (i = 0; i < 3; i++)
    {
      if (snprintf(filename, filename_size, patterns[i], QPK_DIR, qpk->dir,
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
  if (chat_qpk_preview_active) { chat_qpk_preview_back(); return; }
  if (g_desktop.chat_page) { glass_chat_back(NULL); return; }
  if (g_desktop.qapp_page && g_desktop.qapp_return_apps)
    qpk_clicked(NULL);
  else
    panel_hide(NULL);
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
  desktop_transients_reset();
  lv_obj_remove_flag(g_desktop.panel, LV_OBJ_FLAG_HIDDEN);

  page = lv_obj_create(g_desktop.panel);
  if (page == NULL)
    {
      return NULL;
    }

  lv_obj_set_style_opa(page, LV_OPA_TRANSP, 0);
  lv_obj_set_size(page, lv_pct(100), lv_pct(100));
  style_panel(page, theme_card());
  lv_obj_set_style_pad_all(page, 0, 0);
  lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);

  back = lv_button_create(page);
  if (back == NULL)
    {
      lv_obj_delete(page);
      return NULL;
    }

  lv_obj_set_size(back, 52, 42);
  lv_obj_set_pos(back, 12, 10);
  style_button(back, theme_surface());
  lv_obj_add_event_cb(back, qapp_back, LV_EVENT_CLICKED, NULL);
  label = lv_label_create(back);
  if (label == NULL)
    {
      lv_obj_delete(page);
      return NULL;
    }

  lv_label_set_text(label, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(theme_primary()), 0);
  lv_obj_center(label);

  label = make_label(page, title, theme_primary(), 28);
  if (label == NULL)
    {
      lv_obj_delete(page);
      return NULL;
    }

  lv_obj_set_pos(label, 78, 15);

  content = lv_obj_create(page);
  if (content == NULL)
    {
      lv_obj_delete(page);
      return NULL;
    }

  lv_obj_update_layout(page);
  height = lv_obj_get_height(page) - QAPP_HEADER_HEIGHT;
  lv_obj_set_size(content, lv_pct(100), height);
  lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(content, lv_color_hex(theme_card()), 0);
  lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(content, 0, 0);
  lv_obj_set_style_radius(content, 8, 0);
  lv_obj_set_style_pad_all(content, 0, 0);
  lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

  return content;
}

static void qapp_page_commit(lv_obj_t *content)
{
  lv_obj_t *page = lv_obj_get_parent(content);
  lv_obj_t *previous = g_desktop.current_card;

  /* Commit the origin only after launch succeeds, before retiring its page.
   * A replacement app inherits the original entry point. */
  if (!g_desktop.qapp_page)
    g_desktop.qapp_return_apps = g_desktop.apps_page;
  g_desktop.apps_page = false;
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
                           zh_font, show_toast, qpk_show_dialog);
  if (ret == 0)
    {
      qapp_page_commit(card);
    }
  else
    {
      lv_obj_delete(lv_obj_get_parent(card));
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
                           zh_font, show_toast, qpk_show_dialog);
  if (ret == 0)
    {
      qapp_page_commit(card);
    }
  else
    {
      lv_obj_delete(lv_obj_get_parent(card));
    }
}

static void launch_builtin_ouo_qapp(lv_event_t *e)
{
  lv_obj_t *card;
  const struct qpk_entry_s *manifest = &g_builtin_ouo_qpk.manifest;
  extern const char *ouo_get_app_js(unsigned int *len);
  unsigned int source_size;
  const char *source;
  int ret;

  LV_UNUSED(e);
  source = ouo_get_app_js(&source_size);
  card = qapp_page_prepare(manifest->name);
  if (card == NULL || source == NULL || source_size == 0)
    {
      show_toast("页面内存不足");
      return;
    }

  ret = qpk_runtime_launch(card, manifest->name, manifest->package,
                           manifest->version, manifest->entry,
                           source, source_size, zh_font,
                           show_toast, qpk_show_dialog);
  if (ret == 0)
    {
      qapp_page_commit(card);
    }
  else
    {
      lv_obj_delete(lv_obj_get_parent(card));
    }
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
                             "app.js",source,length,zh_font,show_toast,qpk_show_dialog);
  if (!ret) qapp_page_commit(card);
  else lv_obj_delete(lv_obj_get_parent(card));
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
      g_camera_launch_pending = true;
      return;
    }

  g_camera_launch_pending = false;
  source = camera_get_app_js(&source_size);
  card = qapp_page_prepare(manifest->name);
  if (card == NULL || source == NULL || source_size == 0)
    {
      show_toast("页面内存不足");
      return;
    }

  ret = qpk_runtime_launch(card, manifest->name, manifest->package,
                           manifest->version, manifest->entry,
                           source, source_size, zh_font,
                           show_toast, qpk_show_dialog);
  if (ret == 0)
    {
      qapp_page_commit(card);
    }
  else
    {
      lv_obj_delete(lv_obj_get_parent(card));
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
                               zh_font, show_toast, qpk_show_dialog);
  if (!ret) qapp_page_commit(card);
  else lv_obj_delete(lv_obj_get_parent(card));
}

static void launch_builtin_dafeiyu_qapp(lv_event_t *e)
{
  LV_UNUSED(e);
  extern const char *dafeiyu_get_app_js(unsigned int *len);
  unsigned int length;
  const char *source = dafeiyu_get_app_js(&length);
  lv_obj_t *card = qapp_page_prepare("大肥鱼桌宠");
  if (!card || source == NULL || length == 0)
    {
      show_toast("页面内存不足");
      return;
    }

  int ret = qpk_runtime_launch(card, "大肥鱼桌宠", "org.flash.dafeiyu", "1.0.1",
                               "builtin:/dafeiyu/app.js", source, length,
                               zh_font, show_toast, qpk_show_dialog);
  if (!ret) qapp_page_commit(card);
  else lv_obj_delete(lv_obj_get_parent(card));
}

static void glass_homeassistant_enter(lv_obj_t *card);
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
  card = qapp_page_prepare(manifest->name);
  if (card == NULL || source == NULL || source_size == 0)
    {
      show_toast("页面内存不足");
      return;
    }

  ret = qpk_runtime_launch(card, manifest->name, manifest->package,
                           manifest->version, manifest->entry,
                           source, source_size, zh_font,
                           show_toast, qpk_show_dialog);
  if (ret == 0)
    {
      qapp_page_commit(card);
      glass_homeassistant_enter(card);
    }
  else
    {
      lv_obj_delete(lv_obj_get_parent(card));
    }
}

static void external_qapp_clicked(lv_event_t *e)
{
  struct qpk_draft_info operation; glass_qpk_get(&operation);
  if (operation.removing) { show_toast("正在删除应用，请稍候"); return; }
  intptr_t index = (intptr_t)lv_event_get_user_data(e);
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
                                   qpk_show_dialog);
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
           qpk->entry[0] ? qpk->entry : "未声明", QPK_DIR, qpk->dir);
  label = make_label(card, text, theme_secondary(), 20);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(label, 690);
  lv_obj_align(label, LV_ALIGN_TOP_LEFT, 24, 76);
  qapp_page_commit(card);
}

static lv_obj_t *list_button(lv_obj_t *parent, const char *title,
                             const char *subtitle, int y,
                             lv_event_cb_t cb, void *user)
{
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_t *label;

  lv_obj_set_size(button, 690, 72);
  lv_obj_align(button, LV_ALIGN_TOP_MID, 0, y);
  style_button(button, theme_surface());
  lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, user);
  label = make_label(button, title, theme_primary(), 20);
  lv_obj_align(label, LV_ALIGN_TOP_LEFT, 12, 2);
  label = make_label(button, subtitle, theme_secondary(), 16);
  lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 12, -2);
  return button;
}

static void qpk_clicked(lv_event_t *e)
{
  glass_apps(e);
}

static void settings_clicked(lv_event_t *e);

static void settings_rebuild_async(void *data)
{
  LV_UNUSED(data);
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

static void about_clicked(lv_event_t *e)
{
  struct timespec ts;
  lv_obj_t *card;
  lv_obj_t *label;
  char text[320];

  LV_UNUSED(e);
  clock_gettime(CLOCK_MONOTONIC, &ts);
  card = panel_card("关于");
  snprintf(text, sizeof(text),
           "ESP32-P4 中文触摸桌面\n\n"
           "作者：电子科技大学 闻家贤\n\n"
           "系统：Apache NuttX RTOS\n"
           "处理器：双核 RISC-V\n"
           "内存：内部 RAM + PSRAM\n"
           "运行时间：%lu 秒\n"
           "中文字体：阿里巴巴普惠体 3.0 55 Regular",
           (unsigned long)ts.tv_sec);
  label = make_label(card, text, theme_secondary(), 20);
  lv_obj_set_pos(label, 24, 82);
}

#include "glass_ui.inc"
#include "glass_ui_probe.inc"

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

  desktop_ui_probe_service();

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

  if (g_camera_launch_pending && !qpk_runtime_camera_busy())
    {
      launch_builtin_camera_qapp(NULL);
    }

  if (!qpk_runtime_camera_active())
    {
      idle = lv_timer_handler();
    }

  /* Bound NSH command latency even when LVGL has no timer ready. */
  return idle > 100 ? 100 : (idle ? idle : 1);
}

#ifndef GLASS_HOST_TEST

int main(int argc, FAR char *argv[])
{
  if (argc > 1 && !strcmp(argv[1], "hardware-test")) {
    extern int qpk_hw_probe(bool);
    return qpk_hw_probe(argc > 2 && !strcmp(argv[2], "audio"));
  }
  if (argc>1 && !strcmp(argv[1],"espdl")) {
    extern int qpk_dl_probe(int,char **);
    return qpk_dl_probe(argc,argv);
  }
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

  if (argc > 1 && strcmp(argv[1], "storage-test") == 0)
    {
      return qpk_storage_selftest() < 0 ? 1 : 0;
    }

  if (argc == 3 && strcmp(argv[1], "ui") == 0)
    return desktop_ui_probe_request(argv[2]);

  launch_camera = argc > 1 && strcmp(argv[1], "camera") == 0;
  stop_camera = argc > 1 && strcmp(argv[1], "camera-stop") == 0;
  if (lv_is_initialized())
    {
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

  lv_nuttx_init(&info, &result);
  if (result.disp == NULL)
    {
      fprintf(stderr, "desktop: display init failed\n");
      return 1;
  }

  desktop_ui_create();
  lv_obj_invalidate(lv_screen_active());
  lv_refr_now(result.disp);

#if defined(CONFIG_NETUTILS_NETINIT) && !defined(CONFIG_SYSTEM_NSH)
  /* netinit would run before the companion radio has registered eth0, so the
   * boot task below waits for the station and then requests the lease. */
#endif

#ifdef CONFIG_SYSTEM_C6_DESKTOP
  glass_net_autostart();
  glass_weather_start();
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
