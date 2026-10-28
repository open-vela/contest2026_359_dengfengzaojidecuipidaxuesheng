/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_ble.h"
static bool g_mouse_return_apps;
static void glass_mouse_back(lv_event_t *e)
{if(g_mouse_return_apps)glass_apps(e);else panel_hide(e);}

enum glass_mouse_command
{
  GLASS_MOUSE_START = 1,
  GLASS_MOUSE_STOP,
  GLASS_MOUSE_MOVE,
  GLASS_MOUSE_SCROLL,
  GLASS_MOUSE_CLICK_LEFT,
  GLASS_MOUSE_CLICK_MIDDLE,
  GLASS_MOUSE_CLICK_RIGHT
};

struct glass_mouse_job
{
  int command;
  int x;
  int y;
  int wheel;
};

static pthread_mutex_t g_mouse_ui_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_mouse_ui_wake = PTHREAD_COND_INITIALIZER;
static struct
{
  bool busy;
  bool page_open;
  bool pending;
  bool worker_started;
  int command;
  int result;
  unsigned revision;
  struct glass_mouse_job job;
} g_mouse_ui;

static lv_obj_t *g_mouse_status;
static lv_obj_t *g_mouse_detail;
static lv_obj_t *g_mouse_values[4];
static lv_obj_t *g_mouse_start;
static lv_obj_t *g_mouse_stop;
static lv_obj_t *g_mouse_controls[5];
static lv_timer_t *g_mouse_timer;
static bool g_mouse_input_ready;
static bool g_mouse_touching;
static bool g_mouse_touch_moved;
static lv_point_t g_mouse_touch_start;
static lv_point_t g_mouse_touch_last;
static bool g_mouse_view_valid;
static uint32_t g_mouse_input_session,g_mouse_input_epoch;
static bool g_mouse_dragging;
static void glass_mouse_input_cancel(void)
{
  if(g_mouse_input_session)(void)glass_ble_hid_input_end(g_mouse_input_session);
  g_mouse_input_session=g_mouse_input_epoch=0;
  g_mouse_dragging=g_mouse_touching=false;
}
static int glass_mouse_input_ensure(void)
{
  struct glass_ble_hid_state state;glass_ble_hid_read(&state);
  if(g_mouse_input_session&&g_mouse_input_epoch!=state.epoch)glass_mouse_input_cancel();
  if(g_mouse_input_session)return 0;
  int ret=glass_ble_hid_input_begin(state.epoch,&g_mouse_input_session);
  if(!ret)g_mouse_input_epoch=state.epoch;
  return ret;
}
struct glass_mouse_view
{
  bool enabled, advertising, connected, encrypted, mouse_notify;
  bool suspended, boot_mode, host_ready, busy;
  int command, result, error, ble_error, store_error;
};
static struct glass_mouse_view g_mouse_view;
static void glass_mouse_refresh(lv_timer_t *timer);

static int glass_mouse_execute(const struct glass_mouse_job *job)
{
  switch (job->command)
    {
      case GLASS_MOUSE_START: return glass_ble_hid_start();
      case GLASS_MOUSE_STOP: return glass_ble_hid_stop();
      case GLASS_MOUSE_MOVE:
        return glass_ble_hid_mouse(0, job->x, job->y, 0);
      case GLASS_MOUSE_SCROLL:
        return glass_ble_hid_mouse(0, 0, 0, job->wheel);
      case GLASS_MOUSE_CLICK_LEFT: return glass_ble_hid_click(1);
      case GLASS_MOUSE_CLICK_MIDDLE: return glass_ble_hid_click(4);
      case GLASS_MOUSE_CLICK_RIGHT: return glass_ble_hid_click(2);
      default: return -EINVAL;
    }
}

static void *glass_mouse_worker(void *arg)
{
  LV_UNUSED(arg);
  for (;;)
    {
      struct glass_mouse_job job;
      pthread_mutex_lock(&g_mouse_ui_lock);
      while (!g_mouse_ui.pending)
        pthread_cond_wait(&g_mouse_ui_wake, &g_mouse_ui_lock);
      job = g_mouse_ui.job;
      g_mouse_ui.pending = false;
      pthread_mutex_unlock(&g_mouse_ui_lock);

      int result = glass_mouse_execute(&job);
      pthread_mutex_lock(&g_mouse_ui_lock);
      /* A page can close while START is waiting for the old BLE link.
       * Finish its stop on this worker, after START returns, never on LVGL. */
      if (!g_mouse_ui.page_open && job.command != GLASS_MOUSE_STOP)
        {
          pthread_mutex_unlock(&g_mouse_ui_lock);
          int stopped = glass_ble_hid_stop();
          if (!result) result = stopped;
          pthread_mutex_lock(&g_mouse_ui_lock);
        }
      /* Keep the worker busy while a newer motion delta is queued. */
      g_mouse_ui.busy = g_mouse_ui.pending;
      g_mouse_ui.result = result;
      g_mouse_ui.revision++;
      pthread_mutex_unlock(&g_mouse_ui_lock);
    }
  return NULL;
}

static int glass_mouse_submit(int command, int x, int y, int wheel)
{
  pthread_attr_t attr;
  pthread_t thread;
  int ret;
  if(command!=GLASS_MOUSE_START&&command!=GLASS_MOUSE_STOP){
    ret=glass_mouse_input_ensure();
    if(!ret){
      if(command==GLASS_MOUSE_MOVE||command==GLASS_MOUSE_SCROLL)
        ret=glass_ble_hid_input_report(g_mouse_input_session,g_mouse_dragging?1:0,x,y,wheel);
      else{
        unsigned button=command==GLASS_MOUSE_CLICK_LEFT?1:
                        command==GLASS_MOUSE_CLICK_RIGHT?2:4;
        ret=glass_ble_hid_input_click(g_mouse_input_session,button);
      }
    }
    pthread_mutex_lock(&g_mouse_ui_lock);g_mouse_ui.result=ret;
    g_mouse_ui.command=command;g_mouse_ui.revision++;pthread_mutex_unlock(&g_mouse_ui_lock);
    if(ret==-ESTALE||ret==-ENOTCONN)glass_mouse_input_cancel();
    return ret; /* Accepted into a bounded queue, not peer-confirmed input. */
  }
  if(command==GLASS_MOUSE_STOP)glass_mouse_input_cancel();
  pthread_mutex_lock(&g_mouse_ui_lock);
  if (g_mouse_ui.busy)
    {
      pthread_mutex_unlock(&g_mouse_ui_lock);
      return -EBUSY;
    }
  g_mouse_ui.job = (struct glass_mouse_job){command, x, y, wheel};
  g_mouse_ui.busy = true;
  g_mouse_ui.pending = true;
  g_mouse_ui.command = command;
  g_mouse_ui.result = 0;
  g_mouse_ui.revision++;
  if (!g_mouse_ui.worker_started)
    {
      ret = pthread_attr_init(&attr);
      if (!ret)
        {
          ret = pthread_attr_setstacksize(&attr, 8192);
          if (!ret) ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
          if (!ret) ret = pthread_create(&thread, &attr, glass_mouse_worker, NULL);
          pthread_attr_destroy(&attr);
        }
      if (!ret) g_mouse_ui.worker_started = true;
    }
  else
    ret = 0;
  if (ret)
    {
      g_mouse_ui.busy = false;
      g_mouse_ui.pending = false;
      g_mouse_ui.result = -ret;
      g_mouse_ui.revision++;
      pthread_mutex_unlock(&g_mouse_ui_lock);
      return -ret;
    }
  pthread_cond_signal(&g_mouse_ui_wake);
  pthread_mutex_unlock(&g_mouse_ui_lock);
  return 0;
}

static void glass_mouse_action(lv_event_t *event)
{
  int command = (int)(intptr_t)lv_event_get_user_data(event);
  if (glass_mouse_submit(command, 0, 0, 0) == -EBUSY)
    show_toast(glass_tr("正在处理上一项操作"));
}

static int glass_mouse_clamp(int value)
{
  if (value < -127) return -127;
  if (value > 127) return 127;
  return value;
}

static void glass_mouse_touch(lv_event_t *event)
{
  lv_event_code_t code = lv_event_get_code(event);
  if(code==LV_EVENT_PRESS_LOST){glass_mouse_input_cancel();glass_mouse_refresh(NULL);return;}
  lv_indev_t *input = lv_event_get_indev(event);
  bool scroll = (intptr_t)lv_event_get_user_data(event) != 0;
  lv_point_t point;
  if (!input) return;
  lv_indev_get_point(input, &point);
  if (code == LV_EVENT_PRESSED)
    {
      g_mouse_touching = g_mouse_input_ready;
      g_mouse_touch_moved = false;
      g_mouse_touch_start = point;
      g_mouse_touch_last = point;
      return;
    }
  if (!g_mouse_touching) return;
  if(code==LV_EVENT_LONG_PRESSED&&!scroll&&!g_mouse_touch_moved){
    if(!glass_mouse_input_ensure()&&
       !glass_ble_hid_input_report(g_mouse_input_session,1,0,0,0)){
      g_mouse_dragging=true;g_mouse_touch_moved=true;
    }
    return;
  }
  if (code == LV_EVENT_PRESSING)
    {
      int total_x = point.x - g_mouse_touch_start.x;
      int total_y = point.y - g_mouse_touch_start.y;
      if (abs(total_x) > 6 || abs(total_y) > 6) g_mouse_touch_moved = true;
      if (scroll)
        {
          int delta = point.y - g_mouse_touch_last.y;
          if (abs(delta) >= 10)
            {
              int wheel = glass_mouse_clamp(-(delta / 10));
              if (!glass_mouse_submit(GLASS_MOUSE_SCROLL, 0, 0, wheel))
                g_mouse_touch_last = point;
            }
        }
      else
        {
          int delta_x = point.x - g_mouse_touch_last.x;
          int delta_y = point.y - g_mouse_touch_last.y;
          if (abs(delta_x) >= 2 || abs(delta_y) >= 2)
            {
              delta_x = glass_mouse_clamp(delta_x * 2);
              delta_y = glass_mouse_clamp(delta_y * 2);
              if (!glass_mouse_submit(GLASS_MOUSE_MOVE, delta_x, delta_y, 0))
                g_mouse_touch_last = point;
            }
        }
      return;
    }
  if (code == LV_EVENT_RELEASED)
    {
      if(g_mouse_dragging){
        if(glass_ble_hid_input_report(g_mouse_input_session,0,0,0,0))
          glass_mouse_input_cancel();
        g_mouse_dragging=false;
      }else if (!scroll && !g_mouse_touch_moved)
        (void)glass_mouse_submit(GLASS_MOUSE_CLICK_LEFT, 0, 0, 0);
      g_mouse_touching = false;
    }
  else if (code == LV_EVENT_PRESS_LOST)
    {
      g_mouse_touching = false;
      glass_mouse_refresh(NULL);
    }
}

static void glass_mouse_button_enabled(lv_obj_t *button, bool enabled)
{
  if (!button) return;
  if (enabled) lv_obj_remove_state(button, LV_STATE_DISABLED);
  else lv_obj_add_state(button, LV_STATE_DISABLED);
  lv_obj_set_style_opa(button, enabled ? LV_OPA_COVER : LV_OPA_40, 0);
}

static void glass_mouse_status_value(unsigned index, bool ready,
                                     const char *yes, const char *no)
{
  if (!g_mouse_values[index]) return;
  lv_label_set_text(g_mouse_values[index], ready ? yes : no);
  lv_obj_set_style_text_color(g_mouse_values[index],
                              lv_color_hex(ready ? 0x4f9b77 : detail_muted()), 0);
}

static void glass_mouse_refresh(lv_timer_t *timer)
{
  struct glass_ble_hid_state state;
  bool busy;
  int command, result;
  unsigned revision;
  char text[112];
  LV_UNUSED(timer);
  if (!g_mouse_status) return;
  if (!glass_ui_can_paint(g_mouse_status)) return;
  if(g_mouse_touching&&g_mouse_dragging&&g_mouse_input_session&&
     glass_ble_hid_input_keepalive(g_mouse_input_session))glass_mouse_input_cancel();
  /* A touch stream must not compete with status repaint work on the LVGL
   * thread. The next timer tick refreshes the connection state after release. */
  if (g_mouse_touching) return;
  glass_ble_hid_read(&state);
  pthread_mutex_lock(&g_mouse_ui_lock);
  busy = g_mouse_ui.busy;
  command = g_mouse_ui.command;
  result = g_mouse_ui.result;
  revision = g_mouse_ui.revision;
  pthread_mutex_unlock(&g_mouse_ui_lock);

  struct glass_mouse_view view = {0};
  view = (struct glass_mouse_view){
    .enabled = state.enabled,
    .advertising = state.advertising,
    .connected = state.connected,
    .encrypted = state.encrypted,
    .mouse_notify = state.mouse_notify,
    .suspended = state.suspended,
    .boot_mode = state.boot_mode,
    .host_ready = state.host_ready,
    .busy = busy,
    .command = command,
    .result = result,
    .error = state.error,
    .ble_error = state.ble_error,
    .store_error = state.store_error
  };
  bool view_changed = !g_mouse_view_valid ||
                      view.enabled != g_mouse_view.enabled ||
                      view.advertising != g_mouse_view.advertising ||
                      view.connected != g_mouse_view.connected ||
                      view.encrypted != g_mouse_view.encrypted ||
                      view.mouse_notify != g_mouse_view.mouse_notify ||
                      view.suspended != g_mouse_view.suspended ||
                      view.boot_mode != g_mouse_view.boot_mode ||
                      view.host_ready != g_mouse_view.host_ready ||
                      view.busy != g_mouse_view.busy ||
                      view.command != g_mouse_view.command ||
                      view.result != g_mouse_view.result ||
                      view.error != g_mouse_view.error ||
                      view.ble_error != g_mouse_view.ble_error ||
                      view.store_error != g_mouse_view.store_error;
  g_mouse_input_ready = state.enabled && state.connected && state.encrypted &&
                        state.mouse_notify && !state.suspended;
  if (!view_changed) return;
  g_mouse_view = view;
  g_mouse_view_valid = true;

  bool input_ready = state.enabled && state.connected && state.encrypted &&
                     state.mouse_notify && !state.suspended;
  if (busy)
    {
      const char *operation = command == GLASS_MOUSE_START ? glass_tr("正在启动蓝牙") :
                              command == GLASS_MOUSE_STOP ? glass_tr("正在停止并断开") :
                              glass_tr("正在发送操作");
      lv_label_set_text(g_mouse_status, operation);
    }
  else if (state.connected && input_ready)
    lv_label_set_text(g_mouse_status, glass_tr("已连接，可以控制"));
  else if (state.connected)
    lv_label_set_text(g_mouse_status, glass_tr("正在完成安全连接"));
  else if (state.advertising)
    lv_label_set_text(g_mouse_status, glass_tr("等待电脑配对"));
  else if (state.host_ready)
    lv_label_set_text(g_mouse_status, glass_tr("已就绪，等待开始"));
  else
    lv_label_set_text(g_mouse_status, glass_tr("未启动"));

  glass_mouse_status_value(0, state.host_ready, glass_tr("就绪"), glass_tr("未启动"));
  glass_mouse_status_value(1, state.connected, glass_tr("已连接"), glass_tr("未连接"));
  glass_mouse_status_value(2, state.encrypted, glass_tr("已加密"), glass_tr("等待加密"));
  glass_mouse_status_value(3, state.mouse_notify, glass_tr("已订阅"), glass_tr("等待订阅"));

  if (state.store_error)
    snprintf(text, sizeof(text), glass_tr("配对保存失败 (%d)，请检查存储并重启后重试"), state.store_error);
  else if (!busy && result)
    snprintf(text, sizeof(text), result == -ENOSPC ?
             glass_tr("蓝牙连接名额已满 (%d)，请先断开一个连接") : glass_tr("操作失败 (%d)"), result);
  else if (state.error)
    snprintf(text, sizeof(text), glass_tr("鼠标服务错误 (%d)，BLE %d"), state.error,
             state.ble_error);
  else if (state.connected)
    snprintf(text, sizeof(text), glass_tr("%s模式%s"), state.boot_mode ? "Boot " : "Report ",
             state.suspended ? glass_tr(" · 已挂起") : "");
  else if (state.advertising)
    snprintf(text, sizeof(text), glass_tr("请在电脑蓝牙中选择 %s"), glass_ble_identity_name());
  else
    snprintf(text, sizeof(text), glass_tr("开始后暂停普通设备连接，退出鼠标后自动恢复"));
  lv_label_set_text(g_mouse_detail, text);
  LV_UNUSED(revision);
  g_mouse_input_ready = input_ready;

  glass_mouse_button_enabled(g_mouse_start,
                             !busy && !state.advertising && !state.connected);
  glass_mouse_button_enabled(g_mouse_stop,
                             !busy && state.host_ready);
  for (unsigned i = 0; i < 5; i++)
    glass_mouse_button_enabled(g_mouse_controls[i],
                               input_ready && (i < 2 || !busy) &&
                               (!state.boot_mode || i != 1));
}

static void glass_mouse_deleted(lv_event_t *event)
{
  LV_UNUSED(event);
  glass_mouse_input_cancel();
  if (g_mouse_timer) lv_timer_delete(g_mouse_timer);
  g_mouse_timer = NULL;
  g_mouse_input_ready = false;
  g_mouse_touching = false;
  g_mouse_view_valid = false;
  g_mouse_status = g_mouse_detail = NULL;
  g_mouse_start = g_mouse_stop = NULL;
  memset(g_mouse_values, 0, sizeof(g_mouse_values));
  memset(g_mouse_controls, 0, sizeof(g_mouse_controls));
  pthread_mutex_lock(&g_mouse_ui_lock);
  g_mouse_ui.page_open = false;
  bool stopping = g_mouse_ui.busy;
  pthread_mutex_unlock(&g_mouse_ui_lock);
  if (!stopping) (void)glass_mouse_submit(GLASS_MOUSE_STOP, 0, 0, 0);
}

static void glass_mouse_page(lv_obj_t *card)
{
  pthread_mutex_lock(&g_mouse_ui_lock);
  g_mouse_ui.page_open = true;
  pthread_mutex_unlock(&g_mouse_ui_lock);
  lv_obj_add_event_cb(card, glass_mouse_deleted, LV_EVENT_DELETE, NULL);

  detail_page(card);
  lv_obj_t *back=detail_sidebar(card,glass_tr("触摸板"),LV_SYMBOL_GPS,glass_mouse_back);
  lv_label_set_text(lv_obj_get_child(back,0),g_mouse_return_apps?glass_tr("返回应用"):glass_tr("返回桌面"));
  lv_obj_t *selected=detail_nav(card,glass_tr("触摸操作"),LV_SYMBOL_GPS,104,140,NULL,NULL);
  glass_sidebar_item(selected,true);g_desktop.page_back=glass_mouse_back;
  detail_text(card, glass_tr("无线触摸板"), 216, 26, 500, 28, detail_ink());
  detail_text(card, glass_tr("与电脑配对，将屏幕用作无线触摸板"), 216, 66, 720, 14, detail_muted());

  lv_obj_t *status = detail_tile(card, 216, 104, 268, 446);
  home_icon(status, LV_SYMBOL_BLUETOOTH, 20, 22, 24, detail_accent());
  detail_text(status, glass_ble_identity_name(), 52, 24, 200, 18, detail_ink());
  g_mouse_status = detail_text(status, glass_tr("未启动"), 20, 68, 228, 18, detail_ink());
  lv_label_set_long_mode(g_mouse_status, LV_LABEL_LONG_WRAP);
  const char *labels[] = {glass_tr("蓝牙主机"), glass_tr("设备连接"), glass_tr("安全加密"), glass_tr("输入通知")};
  for (unsigned i = 0; i < 4; i++)
    {
      detail_text(status, labels[i], 20, 124 + i * 40, 104, 14, detail_muted());
      g_mouse_values[i] = detail_text(status, "--", 128, 124 + i * 40,
                                      120, 14, detail_muted());
      lv_obj_set_style_text_align(g_mouse_values[i], LV_TEXT_ALIGN_RIGHT, 0);
    }
  g_mouse_start = detail_button(status, glass_tr("开始配对"), 20, 300, 108,
                                 glass_mouse_action,
                                 (void *)(intptr_t)GLASS_MOUSE_START);
  g_mouse_stop = detail_button(status, glass_tr("停止断开"), 140, 300, 108,
                                glass_mouse_action,
                                (void *)(intptr_t)GLASS_MOUSE_STOP);
  lv_obj_t *hint = detail_text(status, glass_tr("配对后保存设备并保持连接；安全连接就绪后可使用触摸板。"),
                                20, 370, 228, 14, detail_muted());
  lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);

  lv_obj_t *control = detail_tile(card, 500, 104, 500, 446);
  detail_text(control, glass_tr("鼠标控制"), 20, 22, 440, 20, detail_ink());
  g_mouse_detail = detail_text(control, glass_tr("点击开始配对后开放 120 秒配对窗口"),
                                20, 60, 460, 14, detail_muted());
  lv_label_set_long_mode(g_mouse_detail, LV_LABEL_LONG_WRAP);

  lv_obj_t *touchpad = glass_box(control, 20, 114, 372, 234,
                                  detail_soft(), 14);
  lv_obj_add_flag(touchpad, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(touchpad, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_border_width(touchpad, 1, 0);
  lv_obj_set_style_border_color(touchpad, lv_color_hex(detail_line()), 0);
  lv_obj_set_style_bg_opa(touchpad, 220, LV_STATE_PRESSED);
  home_icon(touchpad, LV_SYMBOL_GPS, 174, 78, 24, detail_accent());
  lv_obj_t *caption = detail_text(touchpad, glass_tr("轻点左键 · 长按拖拽"), 20, 132, 332, 14, detail_muted());
  lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_add_event_cb(touchpad, glass_mouse_touch, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(touchpad, glass_mouse_touch, LV_EVENT_PRESSING, NULL);
  lv_obj_add_event_cb(touchpad, glass_mouse_touch, LV_EVENT_LONG_PRESSED, NULL);
  lv_obj_add_event_cb(touchpad, glass_mouse_touch, LV_EVENT_RELEASED, NULL);
  lv_obj_add_event_cb(touchpad, glass_mouse_touch, LV_EVENT_PRESS_LOST, NULL);
  g_mouse_controls[0] = touchpad;

  lv_obj_t *scroll = glass_box(control, 408, 114, 72, 234,
                                detail_soft(), 14);
  lv_obj_add_flag(scroll, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(scroll, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_border_width(scroll, 1, 0);
  lv_obj_set_style_border_color(scroll, lv_color_hex(detail_line()), 0);
  lv_obj_set_style_bg_opa(scroll, 220, LV_STATE_PRESSED);
  home_icon(scroll, LV_SYMBOL_UP, 27, 30, 18, detail_muted());
  detail_text(scroll, glass_tr("滚轮"), 20, 108, 48, 14, detail_muted());
  home_icon(scroll, LV_SYMBOL_DOWN, 27, 186, 18, detail_muted());
  lv_obj_add_event_cb(scroll, glass_mouse_touch, LV_EVENT_PRESSED, (void *)1);
  lv_obj_add_event_cb(scroll, glass_mouse_touch, LV_EVENT_PRESSING, (void *)1);
  lv_obj_add_event_cb(scroll, glass_mouse_touch, LV_EVENT_RELEASED, (void *)1);
  lv_obj_add_event_cb(scroll, glass_mouse_touch, LV_EVENT_PRESS_LOST, (void *)1);
  g_mouse_controls[1] = scroll;

  g_mouse_controls[2] = detail_button(control, glass_tr("左键"), 20, 374, 144,
      glass_mouse_action, (void *)(intptr_t)GLASS_MOUSE_CLICK_LEFT);
  g_mouse_controls[3] = detail_button(control, glass_tr("中键"), 178, 374, 144,
      glass_mouse_action, (void *)(intptr_t)GLASS_MOUSE_CLICK_MIDDLE);
  g_mouse_controls[4] = detail_button(control, glass_tr("右键"), 336, 374, 144,
      glass_mouse_action, (void *)(intptr_t)GLASS_MOUSE_CLICK_RIGHT);

  g_mouse_timer = lv_timer_create(glass_mouse_refresh, 200, NULL);
  glass_mouse_refresh(NULL);
  struct glass_ble_hid_state state;
  glass_ble_hid_read(&state);
  struct glass_ble_state central;glass_ble_read(&central);
  if (!state.enabled && !state.connected &&
      (glass_ble_capacity() >= 2 || (!central.scanning && !central.connecting &&
       !(central.connected && central.peer_valid) && !central.disconnecting)))
    (void)glass_mouse_submit(GLASS_MOUSE_START, 0, 0, 0);
}

static void glass_mouse_open(lv_event_t *event)
{
  LV_UNUSED(event);
  g_mouse_return_apps=g_desktop.apps_page;
  lv_obj_t *card = panel_card(glass_tr("无线触摸板"));
  if (card) glass_mouse_page(card);
  if(card)glass_navigation_register(glass_mouse_open,g_mouse_return_apps,0);
}
