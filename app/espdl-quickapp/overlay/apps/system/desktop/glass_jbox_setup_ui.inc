/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_jbox_setup.h"
struct jbox_setup_ui {
  lv_obj_t *root, *status, *detail, *list, *search, *cancel;
  lv_obj_t *rows[GLASS_JBOX_SETUP_LIMIT];
  lv_timer_t *timer;
  unsigned rendered, rendered_count;
  struct glass_jbox_setup_snapshot state;
};
static struct jbox_setup_ui *g_jbox_setup_ui;
static void jbox_setup_refresh(lv_timer_t *timer);
static void jbox_setup_enabled(lv_obj_t *object, bool enabled) {
  if (enabled)
    lv_obj_remove_state(object, LV_STATE_DISABLED);
  else
    lv_obj_add_state(object, LV_STATE_DISABLED);
}
static void jbox_setup_row(lv_event_t *event) {
  struct jbox_setup_ui *v = g_jbox_setup_ui;
  if (!v || v->state.busy)
    return;
  unsigned index = (unsigned)(uintptr_t)lv_event_get_user_data(event);
  int r = glass_jbox_setup_start(v->state.generation, index);
  if (r)
    show_toast(glass_tr("设备状态已变化，请重新搜索"));
  jbox_setup_refresh(NULL);
}
static void jbox_setup_search(lv_event_t *event) {
  LV_UNUSED(event);
  int r = glass_jbox_setup_scan();
  if (r)
    show_toast(glass_tr("正在处理，请稍后重试"));
  jbox_setup_refresh(NULL);
}
static void jbox_setup_cancel_clicked(lv_event_t *event) {
  LV_UNUSED(event);
  glass_jbox_setup_cancel();
  jbox_setup_refresh(NULL);
}
static void jbox_setup_deleted(lv_event_t *event) {
  struct jbox_setup_ui *v = lv_event_get_user_data(event);
  if (g_jbox_setup_ui == v)
    g_jbox_setup_ui = NULL;
  if (v->timer)
    lv_timer_delete(v->timer);
  /* A delete event may arrive after children were destroyed. Never dereference
   * child pointers here; only the persistent service sees cancel.
   */
  glass_jbox_setup_cancel();
  free(v);
}
static void jbox_setup_refresh(lv_timer_t *timer) {
  LV_UNUSED(timer);
  struct jbox_setup_ui *v = g_jbox_setup_ui;
  if (!v)
    return;
  int r = glass_jbox_setup_read(&v->state);
  const char *texts[] = {"准备添加 JBOX",
                         "正在搜索局域网与蓝牙",
                         "选择扩展板，一键配网并绑定",
                         "正在连接所选扩展板",
                         "正在共享主板当前 Wi-Fi 并加密配网",
                         "等待联网与按键授权窗口",
                         "正在保存绑定",
                         "正在校验连接和设备状态",
                         "配网完成，助手与应用可直接使用",
                         "已取消，设备动作不会自动重试",
                         "未完成，请检查网络或重新按设备配对键"};
  const char *text = r                     ? "服务尚未就绪，请稍后打开"
                     : v->state.cancelling ? "正在取消并清理连接"
                                           : texts[v->state.phase];
  lv_label_set_text(v->status, glass_tr(text));
  char detail[160];
  if (v->state.error == -ENETDOWN)
    snprintf(detail, sizeof(detail), "%s",
             glass_tr("请先让主板连接 Wi-Fi，再点击一键配网"));
  else if (v->state.error)
    snprintf(
        detail, sizeof(detail), "%s %d%s", glass_tr("错误"), v->state.error,
        v->state.binding_saved ? glass_tr(" · 绑定已保存，可重新搜索后校验")
                               : "");
  else
    snprintf(
        detail, sizeof(detail), "%s",
        glass_tr(
            "短按 JBOX 的 GPIO10 按键，点击目标即可加入主板当前 Wi-Fi 并绑定"));
  lv_label_set_text(v->detail, detail);
  jbox_setup_enabled(v->search, !r && !v->state.busy);
  jbox_setup_enabled(v->cancel, v->state.busy && !v->state.cancelling);
  if (v->rendered != v->state.generation ||
      v->rendered_count != v->state.count) {
    lv_obj_clean(v->list);
    v->rendered = v->state.generation;
    v->rendered_count = v->state.count;
    for (unsigned i = 0; i < v->state.count && i < GLASS_JBOX_SETUP_LIMIT;
         i++) {
      struct glass_jbox_setup_device *d = &v->state.devices[i];
      lv_obj_t *row = glass_box(v->list, 0, i * 72, 756, 64, detail_soft(), 12);
      v->rows[i] = row;
      detail_text(row, d->name, 16, 9, 420, 18, detail_ink());
      detail_text(row, d->bluetooth ? glass_tr("蓝牙 · 首次配网") : d->host, 16,
                  36, 420, 12, detail_muted());
      detail_button(row, glass_tr(d->bluetooth ? "一键配网" : "绑定并校验"),
                    548, 10, 188, jbox_setup_row, (void *)(uintptr_t)i);
    }
  }
}
static void glass_jbox_setup_page(lv_event_t *event) {
  LV_UNUSED(event);
  lv_obj_t *card = panel_card(glass_tr("JBOX 扩展板"));
  if (!card)
    return;
  struct jbox_setup_ui *v = calloc(1, sizeof(*v));
  if (!v) {
    show_toast(glass_tr("内存不足"));
    return;
  }
  g_jbox_setup_ui = v;
  v->root = card;
  v->rendered = UINT_MAX;
  lv_obj_add_event_cb(card, jbox_setup_deleted, LV_EVENT_DELETE, v);
  detail_page(card);
  glass_settings_sidebar(card, SETTINGS_JBOX);
  detail_text(card, glass_tr("JBOX 扩展板"), 216, 26, 500, 28, detail_ink());
  detail_text(
      card, glass_tr("自动使用主板当前连接的 Wi-Fi，无需选择网络或填写密码。"),
      216, 66, 776, 14, detail_muted());
  v->status = detail_text(card, "", 216, 104, 776, 18, detail_ink());
  lv_label_set_long_mode(v->status, LV_LABEL_LONG_DOT);
  v->detail = detail_text(card, "", 216, 140, 776, 13, detail_muted());
  lv_label_set_long_mode(v->detail, LV_LABEL_LONG_DOT);
  v->search = detail_button(card, glass_tr("搜索扩展板"), 216, 188, 180,
                            jbox_setup_search, NULL);
  v->cancel = detail_button(card, glass_tr("取消"), 408, 188, 120,
                            jbox_setup_cancel_clicked, NULL);
  detail_text(card,
              glass_tr("多块设备时请选择目标；密码只发给所选设备，不交给 AI。"),
              542, 200, 454, 12, detail_muted());
  v->list = glass_box(card, 216, 248, 784, 300, detail_paper(), 0);
  lv_obj_add_flag(v->list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(v->list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(v->list, LV_SCROLLBAR_MODE_AUTO);
  v->timer = lv_timer_create(jbox_setup_refresh, 200, NULL);
  jbox_setup_refresh(NULL);
  if (!v->timer) {
    jbox_setup_enabled(v->search, false);
    show_toast(glass_tr("状态刷新失败，请重新打开"));
  } else if (!v->state.busy && glass_jbox_setup_scan() == 0) {
    jbox_setup_refresh(NULL);
  }
}
