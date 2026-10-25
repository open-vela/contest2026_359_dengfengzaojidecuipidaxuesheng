/* Shared desktop UI. Only the LVGL thread owns these objects. */
#ifdef CONFIG_SYSTEM_C6_DESKTOP
#include "desktop_backend.h"
static struct c6_desktop_worker g_wifi_worker;
static bool g_wifi_started;
static lv_timer_t *g_wifi_timer;
static lv_obj_t *g_wifi_status;
static lv_obj_t *g_wifi_scan;
static lv_obj_t *g_wifi_check;
static lv_obj_t *g_wifi_disconnect;
static lv_obj_t *g_wifi_connect;
static lv_obj_t *g_wifi_ssid;
static lv_obj_t *g_wifi_password;
static lv_obj_t *g_wifi_keyboard;
static lv_obj_t *g_wifi_list;
static lv_obj_t *g_wifi_form;
static struct c6_desktop_result g_wifi_result;
static unsigned long g_wifi_shown;
/* UI-owned tracking; the radio and DHCP workers outlive this page. */
static enum c6_desktop_operation g_wifi_pending;
static uint32_t g_wifi_started_at;
static uint32_t g_wifi_polled_at;
static bool g_wifi_monitor;
static bool g_wifi_waiting_ip;

#define GLASS_WIFI_POLL_MS 1000
#define GLASS_WIFI_JOIN_MS 45000
#define GLASS_WIFI_IP_MS 60000
#define GLASS_WIFI_DISCONNECT_MS 15000

enum glass_wifi_status_e
{
  GLASS_WIFI_IDLE = 1,
  GLASS_WIFI_SCANNING,
  GLASS_WIFI_CONNECTING,
  GLASS_WIFI_OPERATION_ERROR,
  GLASS_WIFI_CONNECTED,
  GLASS_WIFI_COUNT,
  GLASS_WIFI_REQUEST_ERROR,
  GLASS_WIFI_TIMER_ERROR,
  GLASS_WIFI_LINK,
  GLASS_WIFI_CHECKING,
  GLASS_WIFI_IPV4,
  GLASS_WIFI_DISCONNECTING,
  GLASS_WIFI_DISCONNECTED,
  GLASS_WIFI_WAIT_IP,
  GLASS_WIFI_JOIN_TIMEOUT,
  GLASS_WIFI_IP_TIMEOUT,
  GLASS_WIFI_DISCONNECT_TIMEOUT,
};

/* Cache display semantics rather than LVGL's mutable label buffer. */
static struct
{
  int value;
  uint8_t status;
} g_wifi_display;

static void glass_wifi_status_set(enum glass_wifi_status_e status, int value)
{
  if (g_wifi_display.status == status && g_wifi_display.value == value) return;
  const char *text;
  char buffer[112];
  switch (status)
    {
      case GLASS_WIFI_IDLE: text = "尚未扫描"; break;
      case GLASS_WIFI_SCANNING: text = "扫描中"; break;
      case GLASS_WIFI_CONNECTING: text = "正在连接 Wi-Fi…"; break;
      case GLASS_WIFI_CONNECTED: text = "正在连接热点，等待关联…"; break;
      case GLASS_WIFI_WAIT_IP: text = "已连接热点，正在获取 IP 地址…"; break;
      case GLASS_WIFI_JOIN_TIMEOUT: text = "连接超时，请检查密码和热点后重试"; break;
      case GLASS_WIFI_IP_TIMEOUT: text = "获取 IP 超时，请检查路由器 DHCP 后重试"; break;
      case GLASS_WIFI_DISCONNECT_TIMEOUT: text = "断开超时，请查询状态后重试"; break;
      case GLASS_WIFI_TIMER_ERROR: text = "状态刷新启动失败"; break;
      case GLASS_WIFI_CHECKING: text = "正在读取链路状态"; break;
      case GLASS_WIFI_DISCONNECTING: text = "正在断开"; break;
      case GLASS_WIFI_DISCONNECTED: text = "Wi-Fi 已断开"; break;
      case GLASS_WIFI_IPV4:
        snprintf(buffer, sizeof(buffer), "连接成功 · IP: %u.%u.%u.%u",
                 (unsigned int)((uint32_t)value >> 24),
                 (unsigned int)(((uint32_t)value >> 16) & 255),
                 (unsigned int)(((uint32_t)value >> 8) & 255),
                 (unsigned int)((uint32_t)value & 255));
        text = buffer;
        break;
      case GLASS_WIFI_LINK:
        text = value == 3 ? "正在获取 IP 地址…" :
               value == 2 ? "已关联，等待链路就绪" :
               value == 1 ? "未连接热点" : "无线设备未初始化";
        break;
      case GLASS_WIFI_OPERATION_ERROR:
        snprintf(buffer, sizeof(buffer), "操作失败 (%d)", value);
        text = buffer;
        break;
      case GLASS_WIFI_REQUEST_ERROR:
        snprintf(buffer, sizeof(buffer), "请求失败 (%d)", value);
        text = buffer;
        break;
      case GLASS_WIFI_COUNT:
        if (!value) snprintf(buffer, sizeof(buffer), "未找到网络，请重新扫描或手动添加");
        else snprintf(buffer, sizeof(buffer), "找到 %u 个热点，请选择网络", (unsigned int)value);
        text = buffer;
        break;
      default:
        return;
    }
  lv_label_set_text(g_wifi_status, text);
  g_wifi_display.status = status;
  g_wifi_display.value = value;
}
static lv_obj_t *g_wifi_back;
static lv_obj_t *g_wifi_manual;

static size_t glass_wifi_name_length(const struct c6_scan_ap *ap)
{
  size_t length = ap->ssid_length;
  if (length > 32) return 0;
  while (length && ap->ssid[length - 1] == 0) length--;
  return length;
}

static bool glass_wifi_utf8(const uint8_t *s, size_t length)
{
  for (size_t i = 0; i < length; )
    {
      uint32_t value = s[i++];
      unsigned extra;
      uint32_t minimum;
      if (value < 0x80)
        {
          if (value < 32 || value == 127) return false;
          continue;
        }
      if (value >= 0xc2 && value <= 0xdf) { extra = 1; minimum = 0x80; value &= 31; }
      else if (value >= 0xe0 && value <= 0xef) { extra = 2; minimum = 0x800; value &= 15; }
      else if (value >= 0xf0 && value <= 0xf4) { extra = 3; minimum = 0x10000; value &= 7; }
      else return false;
      if (extra > length - i) return false;
      while (extra--)
        {
          if ((s[i] & 0xc0) != 0x80) return false;
          value = (value << 6) | (s[i++] & 63);
        }
      if (value < minimum || value > 0x10ffff ||
          (value >= 0xd800 && value <= 0xdfff)) return false;
    }
  return length > 0;
}

static void glass_wifi_list_view(lv_event_t *e)
{
  LV_UNUSED(e);
  lv_textarea_set_text(g_wifi_password, "");
  lv_obj_remove_flag(g_wifi_list, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(g_wifi_scan, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(g_wifi_manual, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(g_wifi_form, LV_OBJ_FLAG_HIDDEN);
}

static void glass_wifi_edit_view(lv_event_t *e)
{
  if (e) lv_textarea_set_text(g_wifi_ssid, "");
  lv_textarea_set_text(g_wifi_password, "");
  lv_obj_add_flag(g_wifi_list, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(g_wifi_scan, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(g_wifi_manual, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(g_wifi_form, LV_OBJ_FLAG_HIDDEN);
  glass_ime_set_target(g_wifi_keyboard, e ? g_wifi_ssid : g_wifi_password);
}

static void glass_wifi_deleted(lv_event_t *e)
{
  LV_UNUSED(e);
  if (g_wifi_password) lv_textarea_set_text(g_wifi_password, "");
  if (g_wifi_timer) lv_timer_delete(g_wifi_timer);
  g_wifi_timer = NULL;
  memset(&g_wifi_display, 0, sizeof(g_wifi_display));
  g_wifi_monitor = false;
  g_wifi_status = g_wifi_scan = g_wifi_connect = NULL;
  g_wifi_check = NULL;
  g_wifi_disconnect = NULL;
  g_wifi_ssid = g_wifi_password = g_wifi_keyboard = g_wifi_list = NULL;
  g_wifi_back = g_wifi_manual = g_wifi_form = NULL;
}

static void glass_wifi_focus(lv_event_t *e)
{
  glass_ime_set_target(g_wifi_keyboard, lv_event_get_target(e));
}

static void glass_wifi_pick(lv_event_t *e)
{
  size_t index = (uintptr_t)lv_event_get_user_data(e);
  struct c6_desktop_result current;
  if (c6_desktop_worker_read(&g_wifi_worker, &current) < 0 || current.busy ||
      g_wifi_pending) return;
  if (index >= g_wifi_result.count) return;
  struct c6_scan_ap *ap = &g_wifi_result.records[index];
  size_t length = glass_wifi_name_length(ap);
  if (!glass_wifi_utf8(ap->ssid, length)) return;
  char selected[33];
  memcpy(selected, ap->ssid, length);
  selected[length] = 0;
  lv_textarea_set_text(g_wifi_ssid, selected);
  lv_textarea_set_text(g_wifi_password, "");
  glass_ime_set_target(g_wifi_keyboard, g_wifi_password);
  glass_wifi_edit_view(NULL);
}

static void glass_wifi_render_list(void)
{
  lv_obj_clean(g_wifi_list);
  if (g_wifi_result.count == 0)
    detail_text(g_wifi_list, "未找到网络，可重新扫描或手动添加", 20, 24, 680, 16, detail_muted());
  for (size_t i = 0; i < g_wifi_result.count; i++)
    {
      char name[33];
      const struct c6_scan_ap *ap = &g_wifi_result.records[i];
      size_t length = glass_wifi_name_length(ap);
      bool supported = glass_wifi_utf8(ap->ssid, length);
      if (supported) memcpy(name, ap->ssid, length);
      else memset(name, '?', length);
      name[length] = 0;
      lv_obj_t *row = detail_button(g_wifi_list, "", 16, 12 + i * 56, 752,
                                   glass_wifi_pick,
                                   (void *)(uintptr_t)i);
      if (!supported) lv_obj_add_state(row, LV_STATE_DISABLED);
      home_icon(row, LV_SYMBOL_WIFI, 16, 12, 20, detail_accent());
      detail_text(row, name[0] ? name : "隐藏网络", 52, 12, 490, 14, detail_ink());
      char signal[24];
      snprintf(signal, sizeof(signal), "%ld dBm", (long)ap->rssi);
      lv_obj_t *text = detail_text(row, signal, 552, 12, 140, 14, detail_muted());
      lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_RIGHT, 0);
      home_icon(row, LV_SYMBOL_RIGHT, 716, 14, 16, detail_muted());
    }
}

static void glass_wifi_controls(bool busy)
{
  lv_obj_t *controls[] = {g_wifi_scan, g_wifi_manual, g_wifi_connect,
                         g_wifi_ssid, g_wifi_password, g_wifi_keyboard};
  bool locked = busy || g_wifi_pending;
  for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); i++)
    {
      if (locked) lv_obj_add_state(controls[i], LV_STATE_DISABLED);
      else lv_obj_remove_state(controls[i], LV_STATE_DISABLED);
    }
  /* Disconnect remains available between polls to cancel a pending join. */
  lv_obj_t *actions[] = {g_wifi_check, g_wifi_disconnect};
  for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); i++)
    {
      if (busy) lv_obj_add_state(actions[i], LV_STATE_DISABLED);
      else lv_obj_remove_state(actions[i], LV_STATE_DISABLED);
    }
}

static void glass_wifi_update(lv_timer_t *timer)
{
  LV_UNUSED(timer);
  if (g_wifi_status == NULL || g_wifi_list == NULL) return;
  if (!glass_ui_can_paint(g_wifi_status)) return;
  struct c6_desktop_result result;
  if (c6_desktop_worker_read(&g_wifi_worker, &result) < 0) return;
  glass_wifi_controls(result.busy);
  if (result.busy)
    {
      /* Passive status polling must not flash a checking message each second. */
      if (result.operation != C6_DESKTOP_STATUS)
        glass_wifi_status_set(result.operation == C6_DESKTOP_SCAN ?
                            GLASS_WIFI_SCANNING :
                            result.operation == C6_DESKTOP_STATUS ?
                            GLASS_WIFI_CHECKING :
                            result.operation == C6_DESKTOP_DISCONNECT ?
                            GLASS_WIFI_DISCONNECTING : GLASS_WIFI_CONNECTING, 0);
      return;
    }
  if (!result.sequence || result.sequence == g_wifi_shown) goto poll;
  g_wifi_shown = result.sequence;
  if (result.error)
    {
      /* A link transition can invalidate a status snapshot; retry that read. */
      if (result.operation == C6_DESKTOP_STATUS && result.error == -EAGAIN)
        goto poll;
      g_wifi_pending = 0;
      g_wifi_monitor = false;
      glass_wifi_status_set(GLASS_WIFI_OPERATION_ERROR, result.error);
    }
  else if (result.operation == C6_DESKTOP_CONNECT)
    {
      g_wifi_pending = C6_DESKTOP_CONNECT;
      glass_wifi_status_set(GLASS_WIFI_CONNECTED, 0);
      glass_net_lease(true);
      g_wifi_polled_at = lv_tick_get() - GLASS_WIFI_POLL_MS;
    }
  else if (result.operation == C6_DESKTOP_DISCONNECT)
    {
      g_wifi_pending = C6_DESKTOP_DISCONNECT;
      glass_wifi_status_set(GLASS_WIFI_DISCONNECTING, 0);
      g_wifi_polled_at = lv_tick_get() - GLASS_WIFI_POLL_MS;
    }
  else if (result.operation == C6_DESKTOP_STATUS)
    {
      bool usable = result.link.ipv4_ready &&
                    glass_net_wifi_ready(result.link.generation);
      if (g_wifi_pending == C6_DESKTOP_DISCONNECT)
        {
          if (!result.link.associated && !result.link.carrier_ready)
            {
              g_wifi_pending = 0;
              glass_wifi_status_set(GLASS_WIFI_DISCONNECTED, 0);
            }
        }
      else if (usable)
        {
          bool joined = g_wifi_pending == C6_DESKTOP_CONNECT;
          g_wifi_pending = 0;
          const uint8_t *ip = result.link.ipv4;
          uint32_t value = (uint32_t)ip[0] << 24 | (uint32_t)ip[1] << 16 |
                           (uint32_t)ip[2] << 8 | ip[3];
          glass_wifi_status_set(GLASS_WIFI_IPV4, (int)value);
          if (joined) glass_wifi_list_view(NULL);
        }
      else if (g_wifi_pending == C6_DESKTOP_CONNECT)
        {
          if (result.link.associated && !g_wifi_waiting_ip)
            {
              g_wifi_waiting_ip = true;
              g_wifi_started_at = lv_tick_get();
            }
          glass_wifi_status_set(result.link.associated ? GLASS_WIFI_WAIT_IP :
                                GLASS_WIFI_CONNECTED, 0);
        }
      else glass_wifi_status_set(GLASS_WIFI_LINK, result.link.carrier_ready ? 3 :
                          result.link.associated ? 2 :
                          result.link.initialized ? 1 : 0);
    }
  else
    {
      g_wifi_result = result;
      glass_wifi_render_list();
      glass_wifi_status_set(GLASS_WIFI_COUNT, (int)result.count);
    }
poll:
  if (g_wifi_pending)
    {
      uint32_t limit = g_wifi_pending == C6_DESKTOP_DISCONNECT ?
                       GLASS_WIFI_DISCONNECT_MS : g_wifi_waiting_ip ?
                       GLASS_WIFI_IP_MS : GLASS_WIFI_JOIN_MS;
      if (lv_tick_elaps(g_wifi_started_at) >= limit)
        {
          glass_wifi_status_set(g_wifi_pending == C6_DESKTOP_DISCONNECT ?
                                GLASS_WIFI_DISCONNECT_TIMEOUT :
                                g_wifi_waiting_ip ? GLASS_WIFI_IP_TIMEOUT :
                                GLASS_WIFI_JOIN_TIMEOUT, 0);
          g_wifi_pending = 0;
          g_wifi_monitor = false;
        }
    }
  glass_wifi_controls(false);
  if (g_wifi_monitor && lv_tick_elaps(g_wifi_polled_at) >= GLASS_WIFI_POLL_MS)
    {
      int ret = c6_desktop_worker_submit(&g_wifi_worker, C6_DESKTOP_STATUS,
                                         NULL, NULL);
      if (ret == 0) g_wifi_polled_at = lv_tick_get();
      else if (ret != -EBUSY)
        {
          g_wifi_pending = 0;
          g_wifi_monitor = false;
          glass_wifi_status_set(GLASS_WIFI_REQUEST_ERROR, ret);
          glass_wifi_controls(false);
        }
    }
}

static void glass_wifi_request(lv_event_t *e)
{
  uintptr_t action = (uintptr_t)lv_event_get_user_data(e);
  bool connect = action == 1;
  if (connect) glass_ime_commit(g_wifi_keyboard);
  if (connect && (!lv_textarea_get_text(g_wifi_ssid)[0] ||
      strlen(lv_textarea_get_text(g_wifi_ssid)) > 32))
    {
      glass_wifi_status_set(GLASS_WIFI_REQUEST_ERROR, -EINVAL);
      return;
    }
  int ret = c6_desktop_worker_submit(&g_wifi_worker,
      connect ? C6_DESKTOP_CONNECT : action == 2 ? C6_DESKTOP_STATUS :
      action == 3 ? C6_DESKTOP_DISCONNECT : C6_DESKTOP_SCAN,
      connect ? lv_textarea_get_text(g_wifi_ssid) : NULL,
      connect ? lv_textarea_get_text(g_wifi_password) : NULL);
  if (ret == 0)
    {
      g_wifi_monitor = true;
      g_wifi_polled_at = lv_tick_get();
      if (connect || action == 3)
        {
          g_wifi_pending = connect ? C6_DESKTOP_CONNECT : C6_DESKTOP_DISCONNECT;
          g_wifi_started_at = lv_tick_get();
          g_wifi_waiting_ip = false;
        }
      if (connect) lv_textarea_set_text(g_wifi_password, "");
    }
  if (ret < 0) glass_wifi_status_set(GLASS_WIFI_REQUEST_ERROR, ret);
  else glass_wifi_update(NULL);
}
#endif

static void glass_wifi_page(void *unused)
{
  LV_UNUSED(unused);
  lv_obj_t *card = panel_card("Wi-Fi");
  if (!card) return;
  detail_page(card);
  detail_sidebar(card, "网络", LV_SYMBOL_WIFI, panel_hide);
  lv_obj_t *selected = detail_nav(card, "Wi-Fi 网络", LV_SYMBOL_WIFI,
                                  104, 140, NULL, NULL);
  glass_sidebar_item(selected, true);
  detail_text(card, "Wi-Fi 网络", 216, 26, 500, 28, detail_ink());
  detail_text(card, "连接附近网络，或手动添加隐藏网络", 216, 66, 720, 14, detail_muted());
#ifdef CONFIG_SYSTEM_C6_DESKTOP
  if (!g_wifi_started)
    {
      int ret = c6_desktop_worker_start(&g_wifi_worker, &g_c6_desktop_backend);
      if (ret < 0)
        {
          detail_text(card, "网络任务启动失败", 240, 128, 720, 20, detail_ink());
          return;
        }
      g_wifi_started = true;
    }
  lv_obj_add_event_cb(card, glass_wifi_deleted, LV_EVENT_DELETE, NULL);
  g_wifi_shown = 0;
  memset(&g_wifi_display, 0, sizeof(g_wifi_display));
  lv_obj_t *status = detail_tile(card, 216, 104, 784, 84);
  home_icon(status, LV_SYMBOL_WIFI, 20, 28, 24, detail_accent());
  g_wifi_status = detail_text(status, "", 58, 30, 420, 16, detail_ink());
  lv_label_set_long_mode(g_wifi_status, LV_LABEL_LONG_WRAP);
  glass_wifi_status_set(GLASS_WIFI_IDLE, 0);
  g_wifi_check = detail_button(status, "查询状态", 492, 20, 124,
                                glass_wifi_request, (void *)2);
  g_wifi_disconnect = detail_button(status, "断开连接", 632, 20, 132,
                                     glass_wifi_request, (void *)3);
  detail_text(card, "附近网络", 216, 212, 280, 20, detail_ink());
  g_wifi_manual = detail_button(card, "手动添加", 724, 200, 132,
                                 glass_wifi_edit_view, NULL);
  g_wifi_scan = detail_button(card, "扫描网络", 868, 200, 132,
                               glass_wifi_request, NULL);
  g_wifi_list = detail_tile(card, 216, 256, 784, 294);
  lv_obj_add_flag(g_wifi_list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(g_wifi_list, LV_DIR_VER);
  g_wifi_form = detail_tile(card, 216, 200, 784, 350);
  detail_text(g_wifi_form, "网络名称", 20, 26, 88, 14, detail_muted());
  g_wifi_ssid = lv_textarea_create(g_wifi_form);
  glass_input_style(g_wifi_ssid);
  lv_obj_set_pos(g_wifi_ssid, 116, 16);
  lv_obj_set_size(g_wifi_ssid, 648, 44);
  lv_obj_set_style_text_font(g_wifi_ssid, zh_font(20), 0);
  lv_textarea_set_one_line(g_wifi_ssid, true);
  lv_textarea_set_max_length(g_wifi_ssid, 32);
  lv_textarea_set_placeholder_text(g_wifi_ssid, "网络名称（SSID）");
  g_wifi_password = lv_textarea_create(g_wifi_form);
  glass_input_style(g_wifi_password);
  lv_obj_set_pos(g_wifi_password, 20, 74);
  lv_obj_set_size(g_wifi_password, 444, 44);
  lv_textarea_set_one_line(g_wifi_password, true);
  lv_textarea_set_max_length(g_wifi_password, 64);
  lv_textarea_set_password_mode(g_wifi_password, true);
  lv_textarea_set_placeholder_text(g_wifi_password, "密码（开放网络可留空）");
  g_wifi_connect = detail_button(g_wifi_form, "连接网络", 480, 74, 136,
                                  glass_wifi_request, (void *)1);
  g_wifi_back = detail_button(g_wifi_form, "返回列表", 632, 74, 132,
                               glass_wifi_list_view, NULL);
  g_wifi_keyboard = lv_keyboard_create(g_wifi_form);
  glass_keyboard_style(g_wifi_keyboard);
  /* The IME owns a 44 px sibling above the keyboard. Reserve its space. */
  lv_obj_set_size(g_wifi_keyboard, 744, 154);
  lv_obj_align(g_wifi_keyboard, LV_ALIGN_TOP_LEFT, 20, 180);
  lv_obj_add_event_cb(g_wifi_ssid, glass_wifi_focus, LV_EVENT_FOCUSED, NULL);
  lv_obj_add_event_cb(g_wifi_password, glass_wifi_focus, LV_EVENT_FOCUSED, NULL);
  glass_ime_attach(g_wifi_keyboard, zh_font(20), true,
                   detail_ink(), detail_soft(), detail_accent());
  glass_wifi_list_view(NULL);
  glass_wifi_render_list();
  g_wifi_timer = lv_timer_create(glass_wifi_update, 100, NULL);
  if (!g_wifi_timer)
    {
      lv_obj_add_state(g_wifi_scan, LV_STATE_DISABLED);
      lv_obj_add_state(g_wifi_manual, LV_STATE_DISABLED);
      lv_obj_add_state(g_wifi_check, LV_STATE_DISABLED);
      lv_obj_add_state(g_wifi_disconnect, LV_STATE_DISABLED);
      lv_obj_add_state(g_wifi_connect, LV_STATE_DISABLED);
      glass_wifi_status_set(GLASS_WIFI_TIMER_ERROR, 0);
      return;
    }
  glass_wifi_update(NULL);
  /* Opening Settings starts discovery; an in-flight join keeps its owner. */
  struct c6_desktop_result current;
  if (c6_desktop_worker_read(&g_wifi_worker, &current) == 0 &&
      !current.busy && !g_wifi_pending)
    {
      int ret = c6_desktop_worker_submit(&g_wifi_worker, C6_DESKTOP_SCAN,
                                         NULL, NULL);
      if (ret == 0)
        {
          g_wifi_monitor = true;
          g_wifi_polled_at = lv_tick_get();
        }
      else glass_wifi_status_set(GLASS_WIFI_REQUEST_ERROR, ret);
    }
  else g_wifi_monitor = true;
#else
  lv_obj_t *status = detail_tile(card, 216, 104, 784, 160);
  detail_text(status, "桌面网络管理未接入", 24, 28, 720, 20, detail_ink());
  detail_text(status, "连接状态：未知", 24, 80, 720, 16, detail_muted());
#endif
}
