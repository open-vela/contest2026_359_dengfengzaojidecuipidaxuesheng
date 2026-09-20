/* SPDX-License-Identifier: Apache-2.0
 * NSH diagnostics enqueue work; only the desktop thread calls LVGL.
 */
#include <stdarg.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <nuttx/video/fb.h>

static pthread_mutex_t g_ui_probe_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_ui_probe_command[64];
static bool g_ui_probe_pending;
static bool g_ui_probe_done;
static char g_ui_probe_report[32768];
static size_t g_ui_probe_length;
static unsigned g_ui_probe_id;
uint16_t *g_ui_probe_frame;
volatile size_t g_ui_probe_frame_bytes;

/* Lossless run-length capture makes USB JTAG readback practical. The UI
 * thread reads the current displayed page; GDB only reads the saved buffer.
 */
static int desktop_ui_probe_frame(void)
{
  struct fb_planeinfo_s plane;
  memset(&plane, 0, sizeof(plane));
  int fd = open("/dev/fb0", O_RDONLY);
  if (fd < 0) return -errno;
  int ret = ioctl(fd, FBIOGET_PLANEINFO, (unsigned long)&plane);
  int error = errno;
  close(fd);
  if (ret < 0) return -error;
  if (plane.bpp != 16 || plane.stride < 2048 ||
      (size_t)(plane.yoffset + 600) * plane.stride > plane.fblen) return -EINVAL;
  free(g_ui_probe_frame);
  g_ui_probe_frame_bytes = 0;
  g_ui_probe_frame = malloc(1024 * 600 * 4);
  if (!g_ui_probe_frame) return -ENOMEM;
  size_t pairs = 0;
  uint16_t value = 0;
  uint16_t count = 0;
  for (unsigned y = 0; y < 600; y++)
    {
      const uint16_t *row = (const uint16_t *)((const uint8_t *)plane.fbmem +
                            (size_t)(plane.yoffset + y) * plane.stride);
      for (unsigned x = 0; x < 1024; x++)
        {
          if (count && (row[x] != value || count == UINT16_MAX))
            {
              g_ui_probe_frame[pairs++] = count;
              g_ui_probe_frame[pairs++] = value;
              count = 0;
            }
          value = row[x];
          count++;
        }
    }
  if (count)
    { g_ui_probe_frame[pairs++] = count; g_ui_probe_frame[pairs++] = value; }
  g_ui_probe_frame_bytes = pairs * sizeof(uint16_t);
  return 0;
}

static void desktop_ui_probe_print(const char *format, ...)
{
  size_t available = sizeof(g_ui_probe_report) - g_ui_probe_length;
  va_list args;
  va_start(args, format);
  int size = vsnprintf(g_ui_probe_report + g_ui_probe_length, available, format, args);
  va_end(args);
  if (size > 0) g_ui_probe_length += (size_t)size < available ? (size_t)size : available - 1;
}

static void desktop_ui_probe_string(const char *text)
{
  desktop_ui_probe_print("\"");
  for (size_t i = 0; text && text[i] && i < 192; i++)
    {
      unsigned char c = text[i];
      if (c == '"' || c == '\\') desktop_ui_probe_print("\\%c", c);
      else if (c < 32) desktop_ui_probe_print("\\u%04x", c);
      else desktop_ui_probe_print("%c", c);
    }
  desktop_ui_probe_print("\"");
}

static void desktop_ui_probe_tree(lv_obj_t *object, unsigned parent)
{
  if (!lv_obj_is_visible(object)) return;
  unsigned id = ++g_ui_probe_id;
  const char *type = "container";
  const char *text = "";
  bool input = lv_obj_check_type(object, &lv_textarea_class);
  if (lv_obj_check_type(object, &lv_label_class))
    { type = "label"; text = object == portal_code ? "[pairing code hidden]" : lv_label_get_text(object); }
  else if (input)
    { type = "input"; text = lv_textarea_get_placeholder_text(object); }
  else if (lv_obj_check_type(object, &lv_button_class)) type = "button";
  else if (lv_obj_check_type(object, &lv_switch_class)) type = "switch";
  else if (lv_obj_check_type(object, &lv_slider_class)) type = "slider";
  else if (lv_obj_check_type(object, &lv_dropdown_class)) type = "dropdown";
  else if (lv_obj_check_type(object, &lv_keyboard_class)) type = "keyboard";
  else if (lv_obj_check_type(object, &lv_image_class)) type = "image";
  lv_area_t area;
  lv_obj_get_coords(object, &area);
  desktop_ui_probe_print("%s{\"id\":%u,\"parent\":%u,\"type\":\"%s\",\"x\":%ld,\"y\":%ld,\"w\":%ld,\"h\":%ld,\"scroll\":%s,\"text\":",
      id > 1 ? ",\n" : "", id, parent, type, (long)area.x1, (long)area.y1,
      (long)lv_area_get_width(&area), (long)lv_area_get_height(&area),
      lv_obj_has_flag(object, LV_OBJ_FLAG_SCROLLABLE) ? "true" : "false");
  desktop_ui_probe_string(text);
  desktop_ui_probe_print("}");
  /* Never expose input values, including passwords, through diagnostics. */
  if (input) return;
  for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++)
    desktop_ui_probe_tree(lv_obj_get_child(object, i), id);
}

static bool desktop_ui_probe_click(lv_obj_t *object, const char *text)
{
  if (!object || !lv_obj_is_visible(object)) return false;
  if (lv_obj_has_flag(object, LV_OBJ_FLAG_CLICKABLE) && !lv_obj_has_state(object, LV_STATE_DISABLED))
    {
      for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++) {
        lv_obj_t *label = lv_obj_get_child(object, i);
        if (label && lv_obj_check_type(label, &lv_label_class) &&
          (text[0] == '*' ? strstr(lv_label_get_text(label), text + 1) != NULL :
                           !strcmp(lv_label_get_text(label), text)))
          { lv_obj_send_event(object, LV_EVENT_CLICKED, NULL); return true; }
      }
    }
  for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++)
    if (desktop_ui_probe_click(lv_obj_get_child(object, i), text)) return true;
  return false;
}

#include "qpk_capacity_test.inc"
#include "qpk_yield_test.inc"

static lv_obj_t *desktop_ui_probe_form(lv_obj_t *object)
{
  if (!object || !lv_obj_is_visible(object)) return NULL;
  if (lv_obj_check_type(object, &lv_keyboard_class))
    return lv_obj_get_parent(object);
  for (uint32_t i = lv_obj_get_child_count(object); i > 0; i--)
    {
      lv_obj_t *form = desktop_ui_probe_form(lv_obj_get_child(object, i - 1));
      if (form) return form;
    }
  return NULL;
}

static void desktop_ui_probe_service(void)
{
  char command[sizeof(g_ui_probe_command)];
  pthread_mutex_lock(&g_ui_probe_lock);
  if (!g_ui_probe_pending || g_ui_probe_done)
    { pthread_mutex_unlock(&g_ui_probe_lock); return; }
  memcpy(command, g_ui_probe_command, sizeof(command));
  pthread_mutex_unlock(&g_ui_probe_lock);

  g_ui_probe_length = 0;
  g_ui_probe_report[0] = '\0';
  if (!strcmp(command, "frame"))
    {
      int ret = desktop_ui_probe_frame();
      desktop_ui_probe_print("desktop: frame result=%d bytes=%lu\n", ret,
                             (unsigned long)g_ui_probe_frame_bytes);
    }
  else if (!strcmp(command, "frame-free"))
    {
      free(g_ui_probe_frame);
      g_ui_probe_frame = NULL;
      g_ui_probe_frame_bytes = 0;
      desktop_ui_probe_print("desktop: frame released\n");
    }
  else if (!strcmp(command, "weather-status"))
    {
      struct glass_weather_snapshot weather;
      glass_weather_get(&weather);
      desktop_ui_probe_print("weather state=%d valid=%d error=%d revision=%u AQI=%d temp=%d humidity=%d city=%s\n",
        weather.state, weather.valid, weather.error, weather.revision,
        weather.data.aqi, weather.data.temperature, weather.data.humidity, weather.data.location);
    }
  else if (!strcmp(command, "weather-refresh")) glass_weather_refresh();
  else if (!strcmp(command, "pet-status"))
    desktop_ui_probe_print("pet source=%s bytes=%lu open=%d\n",
      pet_lvgl_uses_sd_sprites() ? "sd" : "fallback",
      (unsigned long)pet_lvgl_sprite_bytes(), pet_lvgl_is_open());
  else if (!strcmp(command, "music-status"))
    {
      struct music_snapshot *s = malloc(sizeof(*s));
      if (s) {
        glass_music_get(s);
        desktop_ui_probe_print("music state=%d error=%d search=%d search_error=%d result=%d favorites=%u storage=%d position_ms=%u volume=%u title=%s artist=%s\n",
          s->state,s->error,s->searching,s->search_error,s->has_result,s->favorite_count,s->storage_error,s->position_ms,s->volume,s->current.name,s->current.artist);
        free(s);
      }
    }
  else if (!strcmp(command, "music-play-result"))
    { if (g_music_ui.snapshot.has_result) glass_music_play(&g_music_ui.snapshot.result); }
  else if (!strcmp(command, "navigation-status"))
    desktop_ui_probe_print("navigation page=%s return=%s\n",
      lv_obj_has_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN)?"home":g_desktop.apps_page?"apps":g_desktop.chat_page?"chat":g_desktop.qapp_page?"qapp":"panel",
      (g_desktop.qapp_page && g_desktop.qapp_return_apps) || (g_desktop.chat_page && g_desktop.chat_return_apps)?"apps":"home");
  else if (!strcmp(command, "chat-status")) {
    struct chat_snapshot *s = calloc(1, sizeof(*s));
    if (s) {
      glass_chat_get(s);
      desktop_ui_probe_print("chat loaded=%d configured=%d phase=%d turns=%u storage=%d revision=%u last_state=%d last_error=%d editor=%d\n",
        s->loaded, s->configured, s->phase, s->count, s->storage_error, s->revision,
        s->count ? (int)s->turns[s->count - 1].state : -1,
        s->count ? (int)s->turns[s->count - 1].error : 0, g_chat_ui.editor != NULL);
      const struct chat_turn *t = s->count ? &s->turns[s->count - 1] : NULL;
      desktop_ui_probe_print("chat-stream text_bytes=%lu thinking_bytes=%lu thinking_tokens=%lu tokens_known=%d rounds=%u rebuilds=%u updates=%u refresh_max_ms=%lu draft_bytes=%u\n",
        (unsigned long)(t ? t->text_bytes : 0), (unsigned long)(t ? t->reasoning_bytes : 0),
        (unsigned long)(t ? t->reasoning_tokens : 0), t ? t->has_reasoning_tokens : 0, t ? t->stream_round : 0,
        g_chat_ui.rebuilds, g_chat_ui.live_updates, (unsigned long)g_chat_ui.refresh_max_ms, (unsigned)strlen(chat_draft));
      free(s);
    }
  }
  else if (!strcmp(command, "qpk-draft-status")) {
    struct qpk_draft_info draft; glass_qpk_get(&draft);
    desktop_ui_probe_print("qpk-draft loaded=%d busy=%d revision=%lu previewed=%lu saved=%lu error=%d preview_active=%d name=%s package=%s\n",
      draft.loaded, draft.busy, (unsigned long)draft.revision, (unsigned long)draft.previewed_revision,
      (unsigned long)draft.saved_revision, draft.error, chat_qpk_preview_active, draft.name, draft.installed_package);
    desktop_ui_probe_print("qpk-save stage=%u elapsed_ms=%lu\n", draft.save_stage, (unsigned long)draft.save_elapsed_ms);
    extern volatile uint32_t g_flash_external_stack_ops;
    extern volatile uintptr_t g_flash_caller_sp, g_flash_worker_sp;
    desktop_ui_probe_print("flash-stack ops=%lu caller=%lx worker=%lx\n", (unsigned long)g_flash_external_stack_ops,
      (unsigned long)g_flash_caller_sp, (unsigned long)g_flash_worker_sp);
  }
  else if (!strcmp(command, "chat-scroll-up") || !strcmp(command, "chat-scroll-down")) {
    if (g_chat_ui.messages) {
      uint32_t began = lv_tick_get();
      lv_obj_scroll_by(g_chat_ui.messages, 0, !strcmp(command, "chat-scroll-up") ? 240 : -240, LV_ANIM_OFF);
      lv_refr_now(NULL);
      desktop_ui_probe_print("chat-scroll render_ms=%lu scroll_y=%ld\n", (unsigned long)(lv_tick_get() - began), (long)lv_obj_get_scroll_y(g_chat_ui.messages));
    }
  }
  else if (!strcmp(command, "app-back")) qapp_back(NULL);
  else if (!strcmp(command, "edge-back"))
    { g_edge_page=g_desktop.current_card; glass_edge_back_async(NULL); }
  else if (!strcmp(command, "music-toggle")) glass_music_toggle();
  else if (!strcmp(command, "music-stop")) glass_music_stop();
  else if (!strncmp(command, "music-search:", 13)) glass_music_search(command+13);
  else if (!strncmp(command, "music-key:", 10) || !strncmp(command, "chat-key:", 9))
    {
      bool chat_key = !strncmp(command, "chat-key:", 9);
      lv_obj_t *kb = chat_key ? g_chat_ui.keyboard : g_music_ui.keyboard;
      if (kb) {
        const char **map = lv_buttonmatrix_get_map(kb);
        unsigned id = 0;
        for (unsigned i=0; map[i] && map[i][0]; i++) {
          if (!strcmp(map[i],"\n")) continue;
          if (!strcmp(map[i],command+(chat_key?9:10))) {
            lv_buttonmatrix_set_selected_button(kb,id);
            lv_obj_send_event(kb,LV_EVENT_VALUE_CHANGED,NULL);
            break;
          }
          id++;
        }
      }
    }
  else if (!strncmp(command, "chat-type:", 10)) {
    if (g_chat_ui.input) lv_textarea_set_text(g_chat_ui.input, command + 10);
  }
  else if (!strcmp(command, "weather")) glass_weather_open(NULL);
  else if (strcmp(command, "audit") == 0)
    {
      lv_obj_update_layout(g_desktop.screen);
      g_ui_probe_id = 0;
      desktop_ui_probe_print("UI_LAYOUT_BEGIN\n[");
      lv_obj_t *root = g_desktop.toast ? g_desktop.toast :
                       g_desktop.dialog ? g_desktop.dialog :
                       lv_obj_has_flag(g_desktop.panel, LV_OBJ_FLAG_HIDDEN) ?
                       g_glass.home : g_desktop.current_card;
      /* Input shades cover the app below; audit the visible form itself. */
      lv_obj_t *form = desktop_ui_probe_form(root);
      if (form && root == g_desktop.current_card &&
          lv_obj_get_width(root) == 1024) root = form;
      if (root) desktop_ui_probe_tree(root, 0);
      desktop_ui_probe_print("\n]\nUI_LAYOUT_END\n");
    }
  else
    {
      if (!strncmp(command, "click:", 6))
        desktop_ui_probe_print("click %s -> %d\n", command + 6,
          desktop_ui_probe_click(g_chat_ui.confirm ? g_chat_ui.confirm : g_desktop.current_card, command + 6));
      else if (!strncmp(command, "touch:", 6))
        {
          /* touch:<state>:<x>:<y> replays one pointer update with fractions
           * of the app content, which drives the ui.onTouch() path. */
          char text[sizeof(g_ui_probe_command)];
          strlcpy(text, command + 6, sizeof(text));
          char *state = text;
          char *x = strchr(text, ':');
          char *y = x ? strchr(x + 1, ':') : NULL;
          if (y == NULL)
            desktop_ui_probe_print("touch: expected state:x:y\n");
          else
            {
              *x++ = '\0';
              *y++ = '\0';
              desktop_ui_probe_print("touch %s %s %s -> %d\n", state, x, y,
                qpk_runtime_touch_test(state, strtod(x, NULL), strtod(y, NULL)));
            }
        }
      else if (!strcmp(command, "camera-start"))
        desktop_ui_probe_print("camera start clicked=%d\n",
          desktop_ui_probe_click(g_desktop.current_card, "开始预览"));
      else if (!strcmp(command, "camera-mode"))
        desktop_ui_probe_print("camera mode clicked=%d\n",
          desktop_ui_probe_click(g_desktop.current_card, "*MJPEG"));
      else if (!strcmp(command, "camera-photo"))
        desktop_ui_probe_print("camera capture accepted=%d\n", qpk_runtime_camera_capture());
      else if (!strcmp(command, "espdl")) launch_builtin_espdl_qapp(NULL);
      else if (!strcmp(command, "espdl-start"))
        desktop_ui_probe_print("espdl start clicked=%d\n",desktop_ui_probe_click(g_desktop.current_card,"开始跟随"));
      else if (!strcmp(command, "espdl-stop"))
        desktop_ui_probe_print("espdl stop clicked=%d\n",desktop_ui_probe_click(g_desktop.current_card,"停止"));
      else if (!strcmp(command, "espdl-classify"))
        desktop_ui_probe_print("espdl mode clicked=%d\n",desktop_ui_probe_click(g_desktop.current_card,"物体分类"));
      else if (!strcmp(command, "espdl-photo"))
        desktop_ui_probe_print("espdl classify clicked=%d\n",desktop_ui_probe_click(g_desktop.current_card,"拍摄并分类"));
      else if (!strcmp(command, "home")) panel_hide(NULL);
      else if (!strcmp(command, "settings")) glass_settings(NULL);
      else if (!strcmp(command, "portal")) glass_portal_open(NULL);
      else if (!strcmp(command, "portal-status")) portal_debug_status();
      else if (!strcmp(command, "chat")) glass_chat_open(NULL);
      else if (!strcmp(command, "chat-input")) { if (g_chat_ui.root) chat_ui_editor_open(NULL); }
      else if (!strcmp(command, "chat-send")) { if (g_chat_ui.root) chat_ui_send(NULL); }
      else if (!strcmp(command, "chat-cancel")) glass_chat_cancel();
      else if (!strcmp(command, "chat-new")) { if (g_chat_ui.root) chat_ui_new(NULL); }
      else if (!strcmp(command, "chat-settings")) { if (g_chat_ui.root) chat_ui_config(NULL); }
      else if (!strcmp(command, "qpk-preview")) { if (g_chat_ui.root) chat_ui_preview_app(NULL); }
      else if (!strcmp(command, "qpk-save")) { if (g_chat_ui.root) chat_ui_save_app(NULL); }
      else if (!strcmp(command, "qpk-capacity")) qpk_capacity_test_start();
      else if (!strcmp(command, "qpk-yield")) qpk_yield_test_start();
      else if (!strcmp(command, "qpk-capacity-status")) desktop_ui_probe_print("qpk-resources widgets=%u error=%s\n", qpk_runtime_widget_count(), qpk_runtime_last_error());
      else if (!strcmp(command, "wifi")) glass_wifi_page(NULL);
      else if (!strcmp(command, "bluetooth")) glass_bluetooth_page(NULL);
      else if (!strcmp(command, "ble-scan"))
        { glass_bluetooth_page(NULL); (void)glass_ble_scan(); }
      else if (!strcmp(command, "wifi-scan"))
        { glass_wifi_page(NULL); lv_obj_send_event(g_wifi_scan, LV_EVENT_CLICKED, NULL); }
      else if (!strcmp(command, "files")) glass_storage_page(NULL);
      else if (!strcmp(command, "resources")) glass_resources_page(NULL);
      else if (!strcmp(command, "music")) glass_music(NULL);
      else if (!strcmp(command, "apps")) glass_apps(NULL);
      else if (!strcmp(command, "about")) about_clicked(NULL);
      else if (!strcmp(command, "environment")) glass_environment(NULL);
      else if (!strcmp(command, "hello")) launch_builtin_qapp(NULL);
      else if (!strcmp(command, "toast"))
        { launch_builtin_qapp(NULL); show_toast("操作完成，中文提示清晰可读"); }
      else if (!strcmp(command, "dialog"))
        qpk_show_dialog("提示\n中文内容应清晰显示，正文与确认按钮保持独立间距。\n较长的内容可以在正文区域滚动查看。");
      else if (!strcmp(command, "2048")) launch_builtin_2048_qapp(NULL);
      else if (!strcmp(command, "ouo")) launch_builtin_ouo_qapp(NULL);
      else if (!strcmp(command, "dafeiyu") || !strcmp(command, "pet"))
        launch_builtin_dafeiyu_qapp(NULL);
      else if (!strcmp(command, "recorder")) launch_builtin_recorder_qapp(NULL);
      else if (!strcmp(command, "ha")) glass_homeassistant(NULL);
      else if (!strcmp(command, "ha-settings") || !strcmp(command, "ha-address"))
        {
          glass_portal_show(glass_homeassistant);
        }
      else if (!strcmp(command, "dropdown"))
        { glass_settings(NULL); lv_dropdown_open(g_glass.widget_dropdown); }
      else if (!strcmp(command, "files-new"))
        { glass_storage_page(NULL); g_files_operation = 1; glass_files_dialog_async(NULL); }
      else if (!strcmp(command, "light") || !strcmp(command, "dark"))
        { panel_hide(NULL); g_desktop.light_theme = !strcmp(command, "light"); apply_theme(); }
      desktop_ui_probe_print("desktop: ui %s ready\n", command);
    }
  pthread_mutex_lock(&g_ui_probe_lock);
  g_ui_probe_done = true;
  pthread_mutex_unlock(&g_ui_probe_lock);
}

static int desktop_ui_probe_request(const char *command)
{
  const char *commands[] = {"weather", "weather-status", "weather-refresh", "home", "settings", "wifi", "bluetooth", "files", "resources",
    "music", "music-status", "music-play-result", "music-toggle", "music-stop", "current", "apps", "about", "environment", "hello", "2048", "ouo", "ha", "dropdown",
    "files-new", "light", "dark", "audit", "frame", "frame-free", "wifi-scan", "ble-scan", "dialog",
    "ha-settings", "ha-address", "toast", "camera-start", "camera-photo", "camera-mode",
    "espdl", "espdl-start", "espdl-stop", "espdl-classify", "espdl-photo",
    "navigation-status", "app-back", "edge-back", "portal", "portal-status",
    "chat", "chat-input", "chat-send", "chat-cancel", "chat-new", "chat-settings", "chat-status",
    "qpk-draft-status", "qpk-preview", "qpk-save", "qpk-capacity", "qpk-capacity-status", "chat-scroll-up", "chat-scroll-down", "recorder", "dafeiyu", "pet", "pet-status"};
  /* click:<label> presses one visible control by its label, which is how the
   * file manager's list rows are verified. */
  bool found = (!strncmp(command, "click:", 6) && command[6] != '\0') ||
               (!strncmp(command, "music-key:", 10) && command[10] != '\0') ||
               (!strncmp(command, "chat-key:", 9) && command[9] != '\0') ||
               (!strncmp(command, "chat-type:", 10) && command[10] != '\0') ||
               (!strncmp(command, "music-search:", 13) && command[13] != '\0') ||
               (!strncmp(command, "touch:", 6) && command[6] != '\0');
  for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
    if (!strcmp(command, commands[i])) found = true;
  if (!found || !lv_is_initialized())
    { puts("desktop: unknown UI page or desktop not running"); return 1; }
  pthread_mutex_lock(&g_ui_probe_lock);
  if (g_ui_probe_pending)
    { pthread_mutex_unlock(&g_ui_probe_lock); puts("desktop: UI request busy"); return 1; }
  strlcpy(g_ui_probe_command, command, sizeof(g_ui_probe_command));
  g_ui_probe_pending = true;
  g_ui_probe_done = false;
  pthread_mutex_unlock(&g_ui_probe_lock);
  for (unsigned i = 0; i < 250; i++)
    {
      usleep(20000);
      pthread_mutex_lock(&g_ui_probe_lock);
      if (g_ui_probe_done)
        {
          char *report = strdup(g_ui_probe_report);
          g_ui_probe_pending = false;
          pthread_mutex_unlock(&g_ui_probe_lock);
          if (!report) return 1;
          fputs(report, stdout);
          free(report);
          return 0;
        }
      pthread_mutex_unlock(&g_ui_probe_lock);
    }
  puts("desktop: UI request timed out");
  return 1;
}
