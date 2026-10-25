/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_chat.h"
#include "glass_ai_config.h"
#include "glass_ime.h"
#include "glass_qpk_builder.h"
#include "glass_grok.h"
#include "qpk_espdl.h"
#include "glass_chat_markup.h"
#define CHAT_OUTPUT_PARTS 24
#define CHAT_CONTENT_HEIGHT (lv_display_get_vertical_resolution(NULL))

/* Keep the app edge-to-edge and restore desktop geometry on every exit. */
static struct {
  bool active,status_hidden;
  int screen_x,screen_y,screen_w,screen_h,panel_x,panel_y,panel_w,panel_h;
} chat_display;

static void chat_display_restore(void)
{
  if(!chat_display.active)return;
  chat_display.active=false;
  lv_obj_set_pos(g_desktop.screen,chat_display.screen_x,chat_display.screen_y);
  lv_obj_set_size(g_desktop.screen,chat_display.screen_w,chat_display.screen_h);
  lv_obj_set_pos(g_desktop.panel,chat_display.panel_x,chat_display.panel_y);
  lv_obj_set_size(g_desktop.panel,chat_display.panel_w,chat_display.panel_h);
  if(g_desktop.statusbar&&!chat_display.status_hidden)
    lv_obj_remove_flag(g_desktop.statusbar,LV_OBJ_FLAG_HIDDEN);
}

static void chat_display_expand(void)
{
  lv_obj_update_layout(g_desktop.screen);
  chat_display.screen_x=lv_obj_get_x(g_desktop.screen);chat_display.screen_y=lv_obj_get_y(g_desktop.screen);
  chat_display.screen_w=lv_obj_get_width(g_desktop.screen);chat_display.screen_h=lv_obj_get_height(g_desktop.screen);
  chat_display.panel_x=lv_obj_get_x(g_desktop.panel);chat_display.panel_y=lv_obj_get_y(g_desktop.panel);
  chat_display.panel_w=lv_obj_get_width(g_desktop.panel);chat_display.panel_h=lv_obj_get_height(g_desktop.panel);
  chat_display.status_hidden=!g_desktop.statusbar||lv_obj_has_flag(g_desktop.statusbar,LV_OBJ_FLAG_HIDDEN);
  chat_display.active=true;
  lv_obj_set_pos(g_desktop.screen,0,0);
  lv_obj_set_size(g_desktop.screen,lv_display_get_horizontal_resolution(NULL),CHAT_CONTENT_HEIGHT);
  lv_obj_set_pos(g_desktop.panel,0,0);
  lv_obj_set_size(g_desktop.panel,lv_display_get_horizontal_resolution(NULL),CHAT_CONTENT_HEIGHT);
  if(g_desktop.statusbar)lv_obj_add_flag(g_desktop.statusbar,LV_OBJ_FLAG_HIDDEN);
}
struct chat_output_part {lv_obj_t *root,*text,*caption,*numbers,*viewport;bool code,rich;uint32_t signature;};

enum chat_ball_phase {BALL_IDLE,BALL_LISTEN,BALL_REPLY,BALL_SPEAK,BALL_DRAIN};
enum chat_ball_wave {BALL_WAVE_OFF,BALL_WAVE_LISTEN,BALL_WAVE_THINK,BALL_WAVE_SPEAK};
enum {CHAT_WORKSPACE_LEFT=196,CHAT_WORKSPACE_WIDTH=808,
      CHAT_WORKSPACE_CENTER=CHAT_WORKSPACE_LEFT+CHAT_WORKSPACE_WIDTH/2};
static struct {
  bool voice_session,voice_owned,speech_owned,voice_heard,voice_wave_reduced;
  bool voice_from_wake;
  uint32_t voice_listen_at;
  enum chat_ball_phase voice_phase;
  uint32_t voice_request,voice_speech_request,voice_revision,voice_started,voice_last_sound,voice_ready_at,voice_turn;
  size_t speech_cursor;
  lv_obj_t *voice_hint,*voice_caption,*voice_end;
  lv_obj_t *voice_result;uint32_t voice_result_signature,external_result_turn;
  lv_obj_t *voice_waveform;
  lv_obj_t *voice_face_label;
  lv_obj_t *voice_camera_label;
  bool voice_camera_pending,camera_result_handoff,voice_camera_armed;
  uint32_t voice_camera_wait,voice_camera_turn;
  uint32_t voice_face_request,voice_face_frame,voice_face_seen;
  int voice_gaze_x,voice_gaze_y;
  uint8_t voice_wave_heights[25];
  lv_timer_t *voice_wave_timer;
  enum chat_ball_wave voice_wave;
  uint32_t voice_wave_started;
  unsigned voice_wave_level;
  lv_obj_t *root, *messages, *status, *model, *count, *storage, *draft_label, *action, *action_label;
  lv_obj_t *editor, *input, *keyboard, *counter, *confirm, *waiting;
  lv_obj_t *app_status, *preview_button, *save_button, *discard_button;
  bool discard_requested,ball_mode;
  enum glass_agent_mode mode;
  lv_obj_t *mode_panel;
  struct glass_grok *ball;
  lv_obj_t *ball_strip, *ball_mount, *ball_anchor;
  lv_obj_t *live_bubble, *live_reasoning;
  struct chat_output_part live_parts[CHAT_OUTPUT_PARTS];
  lv_obj_t *trace_root,*trace_button,*trace_details;
  bool trace_expanded;
  lv_obj_t *cards_root;
  int cards_h;
  unsigned drawn_cards;
  uint32_t drawn_cards_signature;
  unsigned live_count;
  int live_y,trace_y;
  unsigned drawn_revision;
  uint32_t drawn_id;
  unsigned drawn_count;
  enum chat_turn_state drawn_state;
  unsigned rebuilds, live_updates;
  uint32_t refresh_max_ms;
  lv_obj_t *archive_panel, *archive_content, *archive_status;
  lv_obj_t *archive_pager;
  struct chat_page *archive_page;
  char archive_name[48];
  struct qpk_draft_info app;
  lv_timer_t *timer;
  struct chat_snapshot *snapshot;
  unsigned ticks;
  uint32_t began;
  lv_obj_t *task_title, *activity_dot, *review_shade, *artifact_name, *sidebar_task;
  bool workspace_ball_compact;
  uint32_t background, surface, sidebar, primary, secondary, accent, on_accent;
} g_chat_ui;
/* A failed staged launch keeps the native chat page usable, including its
 * original full-screen geometry. Never resume a page already retired. */
static void chat_display_resume(void)
{
  if (!chat_display.active && g_desktop.chat_page &&
      g_chat_ui.root && g_chat_ui.root == g_desktop.current_card)
    chat_display_expand();
}

static char chat_drafts[AGENT_MODE_COUNT][CHAT_PROMPT_BYTES + 1];
#define chat_draft chat_drafts[g_chat_ui.mode]
static enum glass_agent_mode chat_interface_modes[2]={AGENT_MODE_CREATE,AGENT_MODE_ASSISTANT};
static bool chat_portal_return_apps,chat_portal_return_ball;
static void glass_chat_open(lv_event_t *e);
static void glass_chat_show(bool from_apps);
static void glass_chat_show_mode(bool from_apps,bool ball_mode);
static void glass_chat_back(lv_event_t *e);
static void chat_ui_refresh(lv_timer_t *timer);
static void chat_workspace_refresh(void);
static void chat_workspace_close(lv_event_t *event);
static void chat_ball_stop(void);
static void chat_ball_refresh(void);
static void chat_ball_create(lv_obj_t *card);
static void chat_ui_editor_open(lv_event_t *e);
static void chat_ui_config(lv_event_t *e);
static void chat_ui_discard(lv_event_t *e);
static void chat_ui_modes(lv_event_t *e);
static void chat_ui_mode_close(lv_event_t *e);
static lv_obj_t *chat_ui_text(lv_obj_t *, const char *, int, int, int, int, uint32_t, bool);
static void chat_ui_ball_position(lv_event_t *e)
{
  LV_UNUSED(e);
  if(!g_chat_ui.ball_mount)return;
  if(g_chat_ui.ball_mode)return;
  if(g_chat_ui.task_title){
    if(g_chat_ui.editor||g_chat_ui.archive_panel){lv_obj_add_flag(g_chat_ui.ball_mount,LV_OBJ_FLAG_HIDDEN);return;}
    lv_obj_remove_flag(g_chat_ui.ball_mount,LV_OBJ_FLAG_HIDDEN);
    if(!g_chat_ui.snapshot->count||!g_chat_ui.ball_anchor)return;
    lv_obj_update_layout(g_chat_ui.messages);
    lv_area_t anchor,view,root;
    lv_obj_get_coords(g_chat_ui.ball_anchor,&anchor);lv_obj_get_coords(g_chat_ui.messages,&view);lv_obj_get_coords(g_chat_ui.root,&root);
    int y=anchor.y1-root.y1-24;
    int top=view.y1-root.y1+4,bottom=view.y2-root.y1-63;
    if(y<top)y=top;if(y>bottom)y=bottom;
    lv_obj_set_pos(g_chat_ui.ball_mount,view.x1-root.x1+8,y);
    return;
  }
  if(!g_chat_ui.ball_anchor||g_chat_ui.editor||g_chat_ui.archive_panel){lv_obj_add_flag(g_chat_ui.ball_mount,LV_OBJ_FLAG_HIDDEN);return;}
  lv_obj_update_layout(g_chat_ui.messages);
  lv_area_t anchor,strip;lv_obj_get_coords(g_chat_ui.ball_anchor,&anchor);lv_obj_get_coords(g_chat_ui.ball_strip,&strip);
  int y=anchor.y1-strip.y1;
  lv_obj_set_pos(g_chat_ui.ball_mount,0,y);
  if(y+64<=0||y>=382)lv_obj_add_flag(g_chat_ui.ball_mount,LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(g_chat_ui.ball_mount,LV_OBJ_FLAG_HIDDEN);
}

static void chat_ui_label(lv_obj_t *label, const char *text)
{ if (label && strcmp(lv_label_get_text(label), text)) lv_label_set_text(label, text); }

static const char *chat_ui_placeholder(void)
{
  return g_chat_ui.mode==AGENT_MODE_CREATE?"描述你想做的应用…":
    g_chat_ui.mode==AGENT_MODE_ASSISTANT?"告诉我需要完成的任务…":"聊点什么…";
}

#include "glass_chat_output.inc"

static void chat_qpk_preview_back(void)
{
  bool origin = chat_qpk_preview_origin;
  chat_qpk_preview_finish();
  glass_chat_show(origin);
}

static void chat_ui_preview_app(lv_event_t *e)
{
  LV_UNUSED(e);
  if (!g_chat_ui.root || g_chat_ui.snapshot->phase != CHAT_IDLE) return;
  struct qpk_draft_info draft;
  char *source = glass_qpk_preview_source(&draft);
  if (!source) { show_toast("请先让 AI 生成草稿，或等待保存结束"); return; }
  bool origin = g_desktop.chat_return_apps;
  char title[96], package[48], filename[192];
  snprintf(title, sizeof(title), "预览 · %s", draft.name);
  snprintf(package, sizeof(package), "preview.%s", draft.slug);
  /* Runtime storage follows the native source volume. A bare "app.js"
   * defaults to internal Flash even when this draft lives on SD. */
  snprintf(filename, sizeof(filename), "%s/.draft/%s.js",
           qpk_volume_root(draft.draft_volume), draft.slug);
  lv_obj_t *card = qapp_page_prepare(title);
  if (!card) { free(source); show_toast("内存不足，无法预览"); return; }
  int ret = qpk_runtime_launch(card, draft.name, package, "draft", filename, source, strlen(source),
                               zh_font, show_toast, qpk_show_dialog, qpk_show_unresponsive);
  free(source);
  const char *error = qpk_runtime_last_error();
  if (ret || error[0] || !qpk_runtime_widget_count()) {
    glass_qpk_preview_result(draft.revision, error[0] ? error : ret ? "预览启动失败，请检查脚本或可用内存。" : "脚本未创建任何可见控件。" );
    qapp_page_discard(lv_obj_get_parent(card));
    show_toast("预览失败，返回对话让 AI 根据错误修改"); return;
  }
  glass_qpk_preview_result(draft.revision, "");
  qapp_page_commit(card);
  chat_qpk_preview_active = true; chat_qpk_preview_origin = origin;
  chat_qpk_preview_revision = draft.revision;
  show_toast("试用后点左上角返回，可以继续修改或保存应用");
}

static void chat_ui_install_choice(lv_event_t *e)
{
  uintptr_t choice=(uintptr_t)lv_event_get_user_data(e);
  if(g_chat_ui.confirm){lv_obj_delete(g_chat_ui.confirm);g_chat_ui.confirm=NULL;}
  if(choice>1)return;
  int ret=glass_qpk_save_async_to(g_chat_ui.app.revision,(enum qpk_volume)choice);
  show_toast(ret?glass_qpk_error_text(ret):"正在安装应用…");chat_ui_refresh(NULL);
}
static void chat_ui_save_app(lv_event_t *e)
{
  LV_UNUSED(e);
  if (!g_chat_ui.root || g_chat_ui.snapshot->phase != CHAT_IDLE || g_chat_ui.confirm) return;
  lv_obj_t *shade=glass_box(g_chat_ui.root,0,0,1024,CHAT_CONTENT_HEIGHT,0x000000,0);
  lv_obj_set_style_bg_opa(shade,LV_OPA_60,0);g_chat_ui.confirm=shade;
  lv_obj_t *card=glass_box(shade,252,150,520,300,g_chat_ui.background,20);
  chat_ui_text(card,"选择安装位置",28,26,464,28,g_chat_ui.primary,false);
  chat_ui_text(card,"内置存储与系统数据共享 2 MiB。\n安装到 SD 卡的应用需要插卡运行。",28,78,464,20,g_chat_ui.secondary,true);
  glass_button(card,"内置存储",28,172,210,g_chat_ui.surface,chat_ui_install_choice,(void *)0);
  bool sd_ready=qpk_volume_mounted(QPK_VOLUME_SD)==0;
  lv_obj_t *sd=glass_button(card,sd_ready?"SD 卡":"SD 卡未就绪",256,172,236,g_chat_ui.surface,chat_ui_install_choice,(void *)1);
  if(!sd_ready){lv_obj_add_state(sd,LV_STATE_DISABLED);lv_obj_set_style_opa(sd,LV_OPA_40,LV_STATE_DISABLED);}
  glass_button(card,"取消",176,238,168,g_chat_ui.surface,chat_ui_install_choice,(void *)2);
}

static lv_obj_t *chat_ui_text(lv_obj_t *parent, const char *text, int x, int y,
                              int width, int size, uint32_t color, bool wrap)
{
  lv_obj_t *label = lv_label_create(parent);
  lv_obj_set_pos(label, x, y); lv_obj_set_width(label, width);
  lv_obj_set_style_text_font(label, zh_font(size), 0);
  lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
  lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_label_set_long_mode(label, wrap ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
  if (wrap) {
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(label, LV_SIZE_CONTENT);
    lv_obj_set_style_text_line_space(label, 6, 0);
  }
  lv_label_set_text(label, text);
  return label;
}

static lv_obj_t *chat_ui_button(lv_obj_t *parent, const char *text, int x, int y,
                                int width, int height, bool filled, lv_event_cb_t cb, void *data)
{
  lv_obj_t *button = glass_box(parent, x, y, width, height,
                               filled ? g_chat_ui.accent : g_chat_ui.surface, 12);
  glass_clickable(button, cb, data);
  lv_obj_set_style_bg_opa(button, LV_OPA_70, LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(button, LV_OPA_40, LV_STATE_DISABLED);
  lv_obj_t *label = chat_ui_text(button, text, 0, 0, width - 20, 16,
                                filled ? g_chat_ui.on_accent : g_chat_ui.primary, false);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0); lv_obj_center(label);
  return button;
}

#include "glass_chat_trace.inc"
#include "glass_chat_cards.inc"
#include "glass_chat_result_ui.inc"

static bool chat_ui_busy(void)
{ return g_chat_ui.snapshot->phase != CHAT_IDLE; }

static void chat_ui_editor_close(lv_event_t *e)
{
  LV_UNUSED(e);
  if (!g_chat_ui.editor) return;
  glass_ime_commit(g_chat_ui.keyboard);
  glass_chat_copy_utf8(chat_draft, sizeof(chat_draft), lv_textarea_get_text(g_chat_ui.input));
  lv_obj_delete(g_chat_ui.editor);
  g_chat_ui.editor = g_chat_ui.input = g_chat_ui.keyboard = g_chat_ui.counter = NULL;
  chat_ui_label(g_chat_ui.draft_label, chat_draft[0] ? chat_draft : chat_ui_placeholder());
  chat_ui_ball_position(NULL);
}

static void chat_ui_send(lv_event_t *e)
{
  LV_UNUSED(e);
  struct qpk_draft_info app; glass_qpk_get(&app);
  if (g_chat_ui.mode==AGENT_MODE_CREATE&&(app.discarding || g_chat_ui.discard_requested)) { show_toast("正在放弃创意，请稍候"); return; }
  const char *text = chat_draft;
  if (g_chat_ui.editor) { glass_ime_commit(g_chat_ui.keyboard); text = lv_textarea_get_text(g_chat_ui.input); }
  if (strlen(text) > CHAT_PROMPT_BYTES) { show_toast("消息太长，请删减少量文字后再试"); return; }
  bool camera=g_chat_ui.ball_mode&&g_chat_ui.voice_camera_armed;
  int ret = camera?glass_chat_send_bot_camera(text,g_chat_ui.mode):
    g_chat_ui.ball_mode?glass_chat_send_bot(text,g_chat_ui.mode):glass_chat_send_mode(text,g_chat_ui.mode);
  if (ret) {
    show_toast(ret == -ENOKEY ? "先扫码完成 AI 配置" : ret == -EBUSY ? "请等待当前回复结束" : "请输入有效的消息");
    return;
  }
  g_chat_ui.voice_camera_armed=false;
  if(camera&&g_chat_ui.mode!=AGENT_MODE_ASSISTANT){glass_chat_get_mode(g_chat_ui.snapshot,g_chat_ui.mode);
    g_chat_ui.voice_camera_turn=g_chat_ui.snapshot->turns[g_chat_ui.snapshot->count-1].id;}
  if (g_chat_ui.editor) {
    lv_obj_delete(g_chat_ui.editor);
    g_chat_ui.editor = g_chat_ui.input = g_chat_ui.keyboard = g_chat_ui.counter = NULL;
  }
  chat_draft[0] = 0; g_chat_ui.began = lv_tick_get();
  /* The Bot shares this editor but has no workbench composer label. */
  chat_ui_label(g_chat_ui.draft_label, chat_ui_placeholder()); chat_ui_refresh(NULL);
}

static void chat_ui_action(lv_event_t *e)
{
  LV_UNUSED(e);
  if (g_chat_ui.snapshot->phase == CHAT_REQUESTING || g_chat_ui.snapshot->phase == CHAT_QUEUED) {
    if(g_chat_ui.snapshot->active_mode!=g_chat_ui.mode){show_toast("其他模式正在回复，请稍候");return;}
    glass_chat_cancel(); chat_ui_refresh(NULL);
  } else if (chat_ui_busy()) return;
  else if (!g_chat_ui.snapshot->configured) chat_ui_config(NULL);
  else if (chat_draft[0]) chat_ui_send(NULL);
  else chat_ui_editor_open(NULL);
}

static void chat_ui_counter(lv_event_t *e)
{
  LV_UNUSED(e);
  if (!g_chat_ui.counter || !g_chat_ui.input) return;
  unsigned length = 0;
  for (const unsigned char *p = (const unsigned char *)lv_textarea_get_text(g_chat_ui.input); *p; p++)
    if ((*p & 0xc0) != 0x80) length++;
  lv_label_set_text_fmt(g_chat_ui.counter, "%u / 512", length);
}

static void chat_ui_editor_open(lv_event_t *e)
{
  LV_UNUSED(e);
  if (g_chat_ui.editor) return;
  lv_obj_t *editor = glass_box(g_chat_ui.root, 168, 0, 856, CHAT_CONTENT_HEIGHT, g_chat_ui.background, 0);
  g_chat_ui.editor = editor;
  chat_ui_text(editor, g_chat_ui.mode==AGENT_MODE_CHAT?"写下消息":"描述任务", 26, 28, 430, 28, g_chat_ui.primary, false);
  chat_ui_button(editor, "收起", 624, 23, 88, 46, false, chat_ui_editor_close, NULL);
  chat_ui_button(editor, "发送", 726, 23, 102, 46, true, chat_ui_send, NULL);
  g_chat_ui.input = lv_textarea_create(editor);
  glass_input_style(g_chat_ui.input);
  lv_obj_set_pos(g_chat_ui.input, 26, 92); lv_obj_set_size(g_chat_ui.input, 804, 116);
  lv_obj_set_style_bg_color(g_chat_ui.input, lv_color_hex(g_chat_ui.surface), 0);
  lv_obj_set_style_text_color(g_chat_ui.input, lv_color_hex(g_chat_ui.primary), 0);
  lv_obj_set_style_text_color(g_chat_ui.input, lv_color_hex(g_chat_ui.secondary), LV_PART_TEXTAREA_PLACEHOLDER);
  lv_obj_set_style_border_color(g_chat_ui.input, lv_color_hex(g_chat_ui.secondary), 0);
  lv_obj_set_style_border_color(g_chat_ui.input, lv_color_hex(g_chat_ui.accent), LV_STATE_FOCUSED);
  lv_obj_set_style_text_line_space(g_chat_ui.input, 5, 0);
  lv_textarea_set_max_length(g_chat_ui.input, 512);
  lv_textarea_set_placeholder_text(g_chat_ui.input, chat_ui_placeholder());
  lv_textarea_set_text(g_chat_ui.input, chat_draft);
  lv_textarea_set_cursor_pos(g_chat_ui.input, LV_TEXTAREA_CURSOR_LAST);
  chat_ui_text(editor, "中文 / English · 连续拼音选词 · 空格确认", 28, 222, 556, 16, g_chat_ui.secondary, false);
  g_chat_ui.counter = chat_ui_text(editor, "", 640, 222, 190, 16, g_chat_ui.secondary, false);
  lv_obj_set_style_text_align(g_chat_ui.counter, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_add_event_cb(g_chat_ui.input, chat_ui_counter, LV_EVENT_VALUE_CHANGED, NULL);
  g_chat_ui.keyboard = lv_keyboard_create(editor);
  glass_keyboard_style(g_chat_ui.keyboard);
  lv_obj_set_style_bg_color(g_chat_ui.keyboard,lv_color_hex(g_chat_ui.sidebar),LV_PART_MAIN);
  lv_obj_set_style_bg_color(g_chat_ui.keyboard,lv_color_hex(g_chat_ui.surface),LV_PART_ITEMS);
  lv_obj_set_style_text_color(g_chat_ui.keyboard,lv_color_hex(g_chat_ui.primary),LV_PART_ITEMS);
  lv_obj_set_style_bg_color(g_chat_ui.keyboard,lv_color_hex(g_chat_ui.accent),LV_PART_ITEMS|LV_STATE_CHECKED);
  lv_obj_set_style_text_color(g_chat_ui.keyboard,lv_color_hex(g_chat_ui.on_accent),LV_PART_ITEMS|LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(g_chat_ui.keyboard,lv_color_hex(g_chat_ui.accent),LV_PART_ITEMS|LV_STATE_PRESSED);
  lv_obj_set_style_text_color(g_chat_ui.keyboard,lv_color_hex(g_chat_ui.on_accent),LV_PART_ITEMS|LV_STATE_PRESSED);
  lv_obj_set_size(g_chat_ui.keyboard, 824, 244);
  /* lv_keyboard starts bottom-aligned; coordinates alone would move it
   * below the screen. Fix the alignment before attaching the candidate bar. */
  lv_obj_align(g_chat_ui.keyboard, LV_ALIGN_TOP_LEFT, 16, CHAT_CONTENT_HEIGHT-260);
  lv_keyboard_set_mode(g_chat_ui.keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_keyboard_set_textarea(g_chat_ui.keyboard, g_chat_ui.input);
  glass_ime_attach(g_chat_ui.keyboard, zh_font(20), true,
                    g_chat_ui.primary, g_chat_ui.surface, g_chat_ui.accent);
  /* The keyboard's check mark closes the keyboard. Sending is explicit. */
  lv_obj_add_event_cb(g_chat_ui.keyboard, chat_ui_editor_close, LV_EVENT_READY, NULL);
  lv_obj_add_event_cb(g_chat_ui.keyboard, chat_ui_editor_close, LV_EVENT_CANCEL, NULL);
  chat_ui_counter(NULL);
  chat_ui_ball_position(NULL);
}

static void chat_ui_suggestion(lv_event_t *e)
{
  snprintf(chat_draft, sizeof(chat_draft), "%s", (const char *)lv_event_get_user_data(e));
  chat_ui_label(g_chat_ui.draft_label, chat_draft); chat_ui_editor_open(NULL);
}

static void chat_ui_retry(lv_event_t *e)
{
  LV_UNUSED(e);
  int ret = glass_chat_retry_mode(g_chat_ui.mode);
  if (ret) show_toast(ret == -ENOKEY ? "先扫码完成 AI 配置" : "请等待当前操作结束");
  else g_chat_ui.began = lv_tick_get();
  chat_ui_refresh(NULL);
}

static void chat_ui_archive_close(lv_event_t *e)
{
  LV_UNUSED(e);
  if (g_chat_ui.archive_panel) lv_obj_delete(g_chat_ui.archive_panel);
  g_chat_ui.archive_panel = g_chat_ui.archive_content = g_chat_ui.archive_status = NULL;
  g_chat_ui.archive_pager = NULL;
  free(g_chat_ui.archive_page); g_chat_ui.archive_page = NULL;
  chat_ui_ball_position(NULL);
}
static void chat_ui_archive_step(void *context,unsigned target)
{
  struct chat_page *p = g_chat_ui.archive_page;
  if (!p || p->busy) return;
  (void)context;int page=(int)target;
  if (page < 0 || page >= (int)p->pages) return;
  if (glass_chat_page_request(g_chat_ui.archive_name, page)) show_toast("正在读取，请稍候");
  else {
    p->busy = true;
    glass_pager_set_enabled(g_chat_ui.archive_pager,false);
  }
}
static void chat_ui_archive_open(lv_event_t *e)
{
  const char *name = lv_event_get_user_data(e);
  if (!name || g_chat_ui.archive_panel) return;
  struct chat_page *p = calloc(1, sizeof(*p));
  if (!p) { show_toast("内存不足"); return; }
  int error = glass_chat_page_request(name, 0);
  if (error) { free(p); show_toast("暂时无法读取完整回复"); return; }
  g_chat_ui.archive_page = p; snprintf(g_chat_ui.archive_name, sizeof(g_chat_ui.archive_name), "%s", name);
  int panel_height=CHAT_CONTENT_HEIGHT-88;
  lv_obj_t *panel = glass_box(g_chat_ui.root, 244, 64, 760, panel_height, g_chat_ui.background, 16);
  g_chat_ui.archive_panel = panel;
  g_chat_ui.archive_status = chat_ui_text(panel, "正在读取完整回复…", 20, 14, 680, 18, g_chat_ui.primary, false);
  g_chat_ui.archive_content = glass_box(panel, 8, 48, 744, panel_height-116, g_chat_ui.surface, 0);
  lv_obj_add_flag(g_chat_ui.archive_content, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(g_chat_ui.archive_content, LV_DIR_VER);
  g_chat_ui.archive_pager=glass_pager_create(panel,g_chat_ui.surface,g_chat_ui.primary,g_chat_ui.secondary,chat_ui_archive_step,NULL);
  if(g_chat_ui.archive_pager)lv_obj_align(g_chat_ui.archive_pager,LV_ALIGN_BOTTOM_MID,0,-16);
  chat_ui_button(panel, "返回对话", 574, panel_height-56, 168, 42, true, chat_ui_archive_close, NULL);
  chat_ui_ball_position(NULL);
}

static int chat_ui_bubble(const char *text, int y, bool user)
{
  int width = user ? 580 : 700, x = user ? 164 : 96;
  if (!user) chat_ui_text(g_chat_ui.messages, "ESPClaw", 114, y + 4, 680, 16, g_chat_ui.secondary, false);
  lv_obj_t *bubble = glass_box(g_chat_ui.messages, x, y + (user ? 0 : 30), width, 60,
                               user ? g_chat_ui.surface : g_chat_ui.background, user ? 18 : 0);
  lv_obj_remove_flag(bubble, LV_OBJ_FLAG_CLICKABLE);
  if(!user)g_chat_ui.ball_anchor=bubble;
  int height = chat_ui_chunks(bubble, text, width, g_chat_ui.primary, false);
  return y + (user ? 0 : 30) + height + 20;
}

static void chat_ui_messages(void)
{
  g_chat_ui.rebuilds++;
  struct chat_snapshot *s = g_chat_ui.snapshot;
  int scroll = lv_obj_get_scroll_y(g_chat_ui.messages);
  bool follow = lv_obj_get_scroll_bottom(g_chat_ui.messages) < 48;
  g_chat_ui.ball_anchor=NULL;
  lv_obj_clean(g_chat_ui.messages); g_chat_ui.waiting = g_chat_ui.live_bubble = g_chat_ui.live_reasoning = NULL;
  g_chat_ui.live_count = 0;memset(g_chat_ui.live_parts,0,sizeof(g_chat_ui.live_parts));
  g_chat_ui.trace_root=g_chat_ui.trace_button=g_chat_ui.trace_details=NULL;
  g_chat_ui.cards_root=NULL;g_chat_ui.cards_h=0;
  if(!s->count||g_chat_ui.drawn_id!=s->turns[s->count-1].id)g_chat_ui.trace_expanded=false;
  if (!s->count) {
    chat_ui_text(g_chat_ui.messages, g_chat_ui.mode==AGENT_MODE_CREATE?"让想法开始运行。":g_chat_ui.mode==AGENT_MODE_ASSISTANT?"有什么任务交给我？":"想聊些什么？", 16, 76, CHAT_WORKSPACE_WIDTH, 28, g_chat_ui.primary, false);
    chat_ui_text(g_chat_ui.messages, s->configured ? (g_chat_ui.mode==AGENT_MODE_CREATE?"描述想做的功能，先生成草稿，再预览和保存。":g_chat_ui.mode==AGENT_MODE_ASSISTANT?"查询信息、完成任务，使用你已授权的工具。":"聊想法、写文字，一起把问题想明白。") : "先连接你的 AI 服务，让灵感从这里开始。",
                  16, 122, CHAT_WORKSPACE_WIDTH, 16, g_chat_ui.secondary, false);
    if (!s->configured) chat_ui_button(g_chat_ui.messages, "扫码配置 AI", 310, 166, 178, 42, true, chat_ui_config, NULL);
    const char *titles[] = {g_chat_ui.mode!=AGENT_MODE_CREATE?"聊聊今天":"制作计数器",g_chat_ui.mode!=AGENT_MODE_CREATE?"帮我想想":"制作计时器","自由聊聊"};
    const char *prompts[] = {g_chat_ui.mode!=AGENT_MODE_CREATE?"我想聊聊今天发生的事。":"做一个大数字计数器快应用，有加一、减一和清零，退出后保留计数。请先生成草稿让我预览。",g_chat_ui.mode!=AGENT_MODE_CREATE?"帮我一起理清现在的想法。":"做一个可以开始、暂停和重置的倒计时快应用，先生成草稿让我预览。", "帮我安排今天的学习计划。"};
    if(g_chat_ui.mode==AGENT_MODE_ASSISTANT){
      titles[0]="查看可用工具";prompts[0]="请列出当前助手模式可用的 MCP 工具及其用途。";
      titles[1]="查询信息";prompts[1]="我想查询一些信息，请先告诉我现在有哪些可用的信息查询工具。";
      titles[2]="梳理任务";prompts[2]="请帮我整理待办事项，需要执行外部操作时先确认具体目标。";
    }else if(g_chat_ui.mode==AGENT_MODE_CREATE){
      titles[2]="完善界面";prompts[2]="我想完善快应用的界面，请先和我确认要修改哪些功能和布局。";
    }
    lv_obj_set_style_text_align(lv_obj_get_child(g_chat_ui.messages,0),LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_style_text_align(lv_obj_get_child(g_chat_ui.messages,1),LV_TEXT_ALIGN_CENTER,0);
    int y = s->configured ? 184 : 218;
    const char *notes[]={"从一个小工具开始","让时间更有条理","继续打磨你的想法"};
    if(g_chat_ui.mode==AGENT_MODE_CHAT){notes[0]="分享日常与心情";notes[1]="一起把问题想明白";notes[2]="从任何话题开始";}
    if(g_chat_ui.mode==AGENT_MODE_ASSISTANT){notes[0]="了解已授权的能力";notes[1]="依据真实工具结果";notes[2]="把事情一件件理清";}
    for (unsigned i = 0; i < 3; i++) {
      lv_obj_t *tile = chat_ui_button(g_chat_ui.messages, titles[i], 56 + i * 248, y, 232, 76,
                                      false, chat_ui_suggestion, (void *)prompts[i]);
      lv_obj_t *label=lv_obj_get_child(tile,0);lv_obj_set_align(label,LV_ALIGN_TOP_LEFT);lv_obj_set_pos(label,14,12);lv_obj_set_width(label,204);
      lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_LEFT,0);
      chat_ui_text(tile,notes[i],14,44,204,16,g_chat_ui.secondary,false);
      lv_obj_set_style_border_width(tile, 1, 0);
      lv_obj_set_style_border_color(tile, lv_color_hex(g_chat_ui.secondary), 0);
      lv_obj_set_style_border_opa(tile, 24, 0);
    }
  } else {
    int y = 4;
    for (unsigned i = 0; i < s->count; i++) {
      const struct chat_turn *t = &s->turns[i];
      y = chat_ui_bubble(t->prompt, y, true);
      if (t->state == CHAT_DONE) {
        if(i==s->count-1&&t->tool_count){
          chat_ui_trace_create(t,y);
          chat_ui_cards_attach(t);
          g_chat_ui.live_bubble=glass_box(g_chat_ui.messages,96,g_chat_ui.live_y,700,0,g_chat_ui.background,0);
          lv_obj_remove_flag(g_chat_ui.live_bubble,LV_OBJ_FLAG_CLICKABLE);
          y=g_chat_ui.live_y+chat_ui_chunks(g_chat_ui.live_bubble,t->reply,700,g_chat_ui.primary,false)+22;
          g_chat_ui.ball_anchor=g_chat_ui.live_bubble;
        }else {
          y = chat_ui_bubble(t->reply, y, false);
          y = chat_ui_cards_inline(t, y);
        }
        if (t->clipped) {
          if (t->archive[0]) {
            chat_ui_button(g_chat_ui.messages, "阅读完整回复", 8, y, 188, 42, false, chat_ui_archive_open, (void *)t->archive);
            y += 52;
          } else {
            chat_ui_text(g_chat_ui.messages, "完整回复未能保存；当前为已保留的片段，请检查存储空间。", 12, y, 704, 16, g_chat_ui.secondary, true);
            y += 58;
          }
        }
        if (t->output_limited) { chat_ui_text(g_chat_ui.messages, "已达到本次输出 Token 上限，可调整设置后继续。", 12, y, 704, 16, g_chat_ui.secondary, true); y += 38; }
      } else if (t->state == CHAT_WAITING) {
        chat_ui_trace_create(t,y);
        chat_ui_cards_attach(t);
        g_chat_ui.live_bubble=glass_box(g_chat_ui.messages,96,g_chat_ui.live_y,700,0,g_chat_ui.background,0);
        lv_obj_remove_flag(g_chat_ui.live_bubble,LV_OBJ_FLAG_CLICKABLE);
        y=g_chat_ui.live_y+chat_ui_chunks(g_chat_ui.live_bubble,t->reply,700,g_chat_ui.primary,true)+22;
        chat_ui_trace_update(t);
      } else {
        if (t->reply[0]) y = chat_ui_bubble(t->reply, y, false);
        if (t->archive[0]) { chat_ui_button(g_chat_ui.messages, "阅读已收到的内容", 8, y, 220, 42, false, chat_ui_archive_open, (void *)t->archive); y += 52; }
        const char *text = t->state == CHAT_CANCELLED ? "已停止这次回复。" : glass_chat_error_text(t->error);
        y = chat_ui_bubble(text, y, false);
        y = chat_ui_cards_inline(t, y);
        if (i == s->count - 1) {
          chat_ui_button(g_chat_ui.messages, "重新发送", 8, y - 6, 128, 42, false, chat_ui_retry, NULL);
          y += 52;
        }
      }
    }
  }
  lv_obj_update_layout(g_chat_ui.messages);
  if (follow) lv_obj_scroll_to_y(g_chat_ui.messages, LV_COORD_MAX, LV_ANIM_OFF);
  else lv_obj_scroll_to_y(g_chat_ui.messages, scroll, LV_ANIM_OFF);
  chat_ui_ball_position(NULL);
  g_chat_ui.drawn_revision = s->revision; g_chat_ui.drawn_count = s->count;
  g_chat_ui.drawn_id = s->count ? s->turns[s->count - 1].id : 0;
  g_chat_ui.drawn_state = s->count ? s->turns[s->count - 1].state : CHAT_DONE;
  g_chat_ui.drawn_cards = s->count ? s->turns[s->count - 1].card_count : 0;
  g_chat_ui.drawn_cards_signature = s->count ? chat_ui_cards_signature(&s->turns[s->count - 1]) : 0;
}

static void chat_ui_live(void)
{
  g_chat_ui.live_updates++;
  struct chat_snapshot *s = g_chat_ui.snapshot;
  if (!s->count || !g_chat_ui.live_bubble) return;
  const struct chat_turn *t = &s->turns[s->count - 1];
  bool follow = lv_obj_get_scroll_bottom(g_chat_ui.messages) < 48;
  chat_ui_chunks(g_chat_ui.live_bubble, t->reply, 700, g_chat_ui.primary, true);
  chat_ui_trace_update(t);
  lv_obj_update_layout(g_chat_ui.messages);
  if (follow) lv_obj_scroll_to_y(g_chat_ui.messages, LV_COORD_MAX, LV_ANIM_OFF);
  chat_ui_ball_position(NULL);
  g_chat_ui.drawn_revision = s->revision;
}

static void chat_ui_confirm_close(lv_event_t *e)
{
  LV_UNUSED(e);
  if (g_chat_ui.confirm) lv_obj_delete(g_chat_ui.confirm);
  g_chat_ui.confirm = NULL;
}

static void chat_ui_new_confirm(lv_event_t *e)
{
  LV_UNUSED(e);
  int ret = glass_chat_clear_mode(g_chat_ui.mode);
  if (ret) { show_toast("请等待对话载入后再试"); return; }
  if (g_chat_ui.editor) chat_ui_editor_close(NULL);
  chat_draft[0] = 0;
  chat_ui_label(g_chat_ui.draft_label, chat_ui_placeholder());
  chat_ui_confirm_close(NULL); chat_ui_refresh(NULL);
}

static void chat_ui_discard_confirm(lv_event_t *e)
{
  LV_UNUSED(e);
  glass_chat_cancel();
  int ret = glass_qpk_discard_async();
  if (ret) { show_toast(glass_qpk_error_text(ret)); return; }
  g_chat_ui.discard_requested = true;
  chat_ui_confirm_close(NULL);
  show_toast("正在放弃创意…");
  chat_ui_refresh(NULL);
}

static void chat_ui_discard(lv_event_t *e)
{
  LV_UNUSED(e);
  if (g_chat_ui.confirm) return;
  struct qpk_draft_info app; glass_qpk_get(&app);
  if (app.busy) { show_toast("正在处理草稿，请稍后重试"); return; }
  lv_obj_t *shade = glass_box(g_chat_ui.root, 0, 0, 1024, CHAT_CONTENT_HEIGHT, 0x000000, 0);
  lv_obj_set_style_bg_opa(shade, LV_OPA_60, 0); g_chat_ui.confirm = shade;
  lv_obj_t *card = glass_box(shade, 252, 166, 520, 268, g_chat_ui.background, 22);
  chat_ui_text(card, "放弃这个创意？", 28, 26, 464, 28, g_chat_ui.primary, false);
  chat_ui_text(card, "停止本次生成并清除当前应用草稿。\n聊天记录和已安装的应用会保留。", 28, 84, 464, 20, g_chat_ui.secondary, true);
  chat_ui_button(card, "继续创作", 28, 196, 214, 46, false, chat_ui_confirm_close, NULL);
  chat_ui_button(card, "放弃创意", 258, 196, 234, 46, true, chat_ui_discard_confirm, NULL);
}

static void chat_ui_new(lv_event_t *e)
{
  LV_UNUSED(e);
  if (g_chat_ui.confirm) return;
  if (!g_chat_ui.snapshot->count) { chat_ui_editor_open(NULL); return; }
  lv_obj_t *shade = glass_box(g_chat_ui.root, 0, 0, 1024, CHAT_CONTENT_HEIGHT, 0x000000, 0);
  lv_obj_set_style_bg_opa(shade, LV_OPA_60, 0); g_chat_ui.confirm = shade;
  lv_obj_t *card = glass_box(shade, 272, 178, 480, 242, g_chat_ui.background, 22);
  chat_ui_text(card, "开始新对话？", 28, 26, 424, 28, g_chat_ui.primary, false);
  chat_ui_text(card, "清空当前模式的对话，其他模式保留。\n当前模式正在等待的回复也会停止。", 28, 82, 424, 20, g_chat_ui.secondary, true);
  chat_ui_button(card, "保留对话", 28, 174, 186, 46, false, chat_ui_confirm_close, NULL);
  chat_ui_button(card, "清空并开始", 238, 174, 214, 46, true, chat_ui_new_confirm, NULL);
}

static void chat_ui_return_from_portal(lv_event_t *e)
{
  LV_UNUSED(e);
  glass_chat_show_mode(chat_portal_return_apps,chat_portal_return_ball);
}

static void chat_ui_config(lv_event_t *e)
{
  LV_UNUSED(e);
  chat_portal_return_apps = g_desktop.chat_return_apps;
  chat_portal_return_ball = g_chat_ui.ball_mode;
  if (g_chat_ui.editor) chat_ui_editor_close(NULL);
  glass_portal_show(chat_ui_return_from_portal);
}

static void chat_ui_refresh(lv_timer_t *timer)
{
  LV_UNUSED(timer);
  if (!g_chat_ui.root) return;
  if(g_chat_ui.ball_mode){chat_ball_refresh();return;}
  if(!glass_ui_can_paint(g_chat_ui.root))return;
  uint32_t refresh_started = lv_tick_get();
  if (g_chat_ui.archive_page && glass_chat_page_get(g_chat_ui.archive_page)) {
    struct chat_page *page = g_chat_ui.archive_page;
    bool ready = !page->busy && !page->error;
    glass_pager_update(g_chat_ui.archive_pager,page->page,page->pages,ready);
    if (page->busy) chat_ui_label(g_chat_ui.archive_status, "正在读取完整回复…");
    else if (page->error) chat_ui_label(g_chat_ui.archive_status, "完整回复读取失败，请检查设备存储");
    else {
      char text[128]; snprintf(text, sizeof(text), "完整回复");
      chat_ui_label(g_chat_ui.archive_status, text);
      lv_obj_clean(g_chat_ui.archive_content);
      lv_obj_t *content = glass_box(g_chat_ui.archive_content, 0, 0, 730, 60, g_chat_ui.surface, 0);
      chat_ui_chunks(content, page->text, 730, g_chat_ui.primary, false);
      lv_obj_scroll_to_y(g_chat_ui.archive_content, 0, LV_ANIM_OFF);
    }
  }
  struct chat_snapshot *s = g_chat_ui.snapshot;
  int old_storage = s->storage_error; unsigned previous_count = s->count;
  uint32_t previous_id = s->count ? s->turns[s->count - 1].id : 0;
  bool update = glass_chat_get_mode(s,g_chat_ui.mode);
  struct qpk_draft_info app;
  glass_qpk_get(&app);
  if (g_chat_ui.discard_requested && !app.discarding) {
    g_chat_ui.discard_requested = false;
    if (!app.error) show_toast("已放弃创意，可以开始新的想法");
  }
  if (app.saved_revision && app.saved_revision != g_chat_ui.app.saved_revision)
    show_toast("应用已保存，可以从全部应用打开");
  if (app.error && app.error != g_chat_ui.app.error) show_toast(glass_qpk_error_text(app.error));
  g_chat_ui.app = app;
  char app_text[192];
  if (app.discarding) snprintf(app_text, sizeof(app_text), "正在放弃创意…");
  else if (app.busy && app.save_stage) snprintf(app_text, sizeof(app_text), "%s\n请稍候，完成后会提示", app.save_stage == 1 ? "正在检查保存位置" : app.save_stage == 2 ? "正在写入应用脚本" : "正在写入应用信息");
  else if (app.busy) snprintf(app_text, sizeof(app_text), "正在处理草稿…");
  else if (app.error) snprintf(app_text, sizeof(app_text), "草稿操作未完成\n可检查后重试");
  else if (app.name[0]) snprintf(app_text, sizeof(app_text), "第 %lu 版\n%s",
    (unsigned long)app.revision, app.saved_revision == app.revision ? "已保存到全部应用" :
    app.preview_error[0] ? "预览出错，可反馈修改" : app.previewed_revision == app.revision ? "已预览，可修改或保存" : "草稿已就绪，等待预览");
  else snprintf(app_text, sizeof(app_text), "描述想做的应用\nAI 会先生成预览草稿");
  if(g_chat_ui.mode!=AGENT_MODE_CREATE)snprintf(app_text,sizeof(app_text),"%s",g_chat_ui.mode==AGENT_MODE_CHAT?
    "自然交流\n不执行外部操作":"查询信息、完成任务\n使用已授权的工具");
  chat_ui_label(g_chat_ui.app_status, app_text);
  bool can_preview = app.loaded && app.name[0] && !app.busy && s->phase == CHAT_IDLE;
  bool can_discard = app.loaded && !app.busy && (app.name[0] || s->phase == CHAT_REQUESTING || s->phase == CHAT_QUEUED);
  if (can_discard) lv_obj_remove_state(g_chat_ui.discard_button, LV_STATE_DISABLED);
  else lv_obj_add_state(g_chat_ui.discard_button, LV_STATE_DISABLED);
  if (can_preview) lv_obj_remove_state(g_chat_ui.preview_button, LV_STATE_DISABLED);
  else lv_obj_add_state(g_chat_ui.preview_button, LV_STATE_DISABLED);
  if (can_preview && app.previewed_revision == app.revision && app.saved_revision != app.revision)
    lv_obj_remove_state(g_chat_ui.save_button, LV_STATE_DISABLED);
  else lv_obj_add_state(g_chat_ui.save_button, LV_STATE_DISABLED);
  if (update) {
    if (s->storage_error && s->storage_error != old_storage) show_toast("对话记录未能保存，请检查设备存储");
    if (previous_count != s->count || (s->count && previous_id != s->turns[s->count - 1].id)) {
      lv_obj_scroll_to_y(g_chat_ui.messages, LV_COORD_MAX, LV_ANIM_OFF);
      g_chat_ui.began = lv_tick_get();
    }
    chat_ui_label(g_chat_ui.model, s->model[0] ? s->model : "选择你的 AI 服务，开始第一段对话");
    char count_text[48]; snprintf(count_text, sizeof(count_text), "当前模式 · %u 轮", s->count); chat_ui_label(g_chat_ui.count, count_text);
    const struct chat_turn *stored=s->count?&s->turns[s->count-1]:NULL;
    chat_ui_label(g_chat_ui.storage,stored&&stored->session_history?
      (stored->state==CHAT_WAITING?"本次会话 · 完成后保存 SD":
       stored->camera_observation[0]?"结果已独立保存 SD":"仅本次会话 · 未保存 SD"):
      s->storage_error == -EINVAL ? "记录读取失败，原文件已保留" :
      s->storage_error == -ENOSPC ? "存储空间不足" :
      s->storage_error ? "记录暂未保存" : "自动保存在设备");
    bool other_request=(s->phase==CHAT_REQUESTING||s->phase==CHAT_QUEUED)&&s->active_mode!=g_chat_ui.mode;
    chat_ui_label(g_chat_ui.action_label, other_request?"等待":s->phase == CHAT_REQUESTING || s->phase == CHAT_QUEUED ? "停止" :
                       s->phase == CHAT_STOPPING ? "停止中" : s->phase == CHAT_CLEARING ? "请稍候" :
                       !s->configured ? "去配置" : "发送");
    if (other_request || s->phase == CHAT_LOADING || s->phase == CHAT_CLEARING || s->phase == CHAT_STOPPING)
      lv_obj_add_state(g_chat_ui.action, LV_STATE_DISABLED);
    else lv_obj_remove_state(g_chat_ui.action, LV_STATE_DISABLED);
  }
  /* Receiving continues in the worker while the user scrolls. */
  if (s->revision != g_chat_ui.drawn_revision && !lv_obj_is_scrolling(g_chat_ui.messages)) {
    const struct chat_turn *last = s->count ? &s->turns[s->count - 1] : NULL;
    if (last && g_chat_ui.drawn_count == s->count && g_chat_ui.drawn_id == last->id &&
        g_chat_ui.drawn_state == CHAT_WAITING && last->state == CHAT_WAITING &&
        g_chat_ui.drawn_cards == last->card_count &&
        g_chat_ui.drawn_cards_signature == chat_ui_cards_signature(last)) chat_ui_live();
    else chat_ui_messages();
  }
  const char *status = !s->loaded ? "正在载入对话" : s->phase == CHAT_CLEARING ? "正在新建对话" :
    s->phase == CHAT_STOPPING ? "正在停止回复" : s->phase == CHAT_REQUESTING || s->phase == CHAT_QUEUED ? "正在思考" :
    s->configured ? "随时可以开始" : "需要配置 AI 服务";
  if ((s->phase == CHAT_REQUESTING || s->phase == CHAT_QUEUED)&&s->active_mode!=g_chat_ui.mode) {
    chat_ui_label(g_chat_ui.status,"其他模式正在回复，请稍候");
  } else if (s->phase == CHAT_REQUESTING || s->phase == CHAT_QUEUED) {
    unsigned seconds = (lv_tick_get() - g_chat_ui.began) / 1000;
    char text[160];
    const struct chat_turn *t = s->count ? &s->turns[s->count - 1] : NULL;
    snprintf(text, sizeof(text), "%s · %u 秒", t && t->progress[0] ? t->progress : status, seconds);
    chat_ui_label(g_chat_ui.status, text);
  } else {
    chat_ui_label(g_chat_ui.status, status);
    if (g_chat_ui.waiting && s->phase != CHAT_IDLE) chat_ui_label(g_chat_ui.waiting, s->phase == CHAT_CLEARING ? "正在开始新对话…" : "正在停止回复…");
  }
  if (++g_chat_ui.ticks % 20 == 0 && s->phase == CHAT_IDLE) glass_chat_refresh_config();
  if(g_chat_ui.trace_root&&s->count&&s->turns[s->count-1].state==CHAT_WAITING)
    chat_ui_trace_update(&s->turns[s->count-1]);
  chat_workspace_refresh();
  chat_ui_ball_position(NULL);
  uint32_t elapsed = lv_tick_get() - refresh_started;
  if (elapsed > g_chat_ui.refresh_max_ms) g_chat_ui.refresh_max_ms = elapsed;
}

static void chat_ui_deleted(lv_event_t *e)
{
  LV_UNUSED(e);
  chat_display_restore();
  if(g_chat_ui.ball_mode)chat_ball_stop();
  if(g_chat_ui.voice_face_request)qpk_dl_cancel_request(g_chat_ui.voice_face_request);
  if(!g_chat_ui.camera_result_handoff&&g_chat_ui.voice_camera_turn&&g_chat_ui.snapshot&&g_chat_ui.snapshot->count&&
     g_chat_ui.snapshot->turns[g_chat_ui.snapshot->count-1].id==g_chat_ui.voice_camera_turn)glass_chat_cancel();
  if(g_chat_ui.voice_wave_timer)lv_timer_delete(g_chat_ui.voice_wave_timer);
  free(g_chat_ui.archive_page); g_chat_ui.archive_page = NULL;
  if (g_chat_ui.editor && g_chat_ui.input) {
    glass_ime_commit(g_chat_ui.keyboard);
    glass_chat_copy_utf8(chat_draft, sizeof(chat_draft), lv_textarea_get_text(g_chat_ui.input));
  }
  if (g_chat_ui.timer) lv_timer_delete(g_chat_ui.timer);
  free(g_chat_ui.snapshot);
  memset(&g_chat_ui, 0, sizeof(g_chat_ui));
}

static void glass_chat_back(lv_event_t *e)
{
  LV_UNUSED(e);
  if (g_chat_ui.confirm) chat_ui_confirm_close(NULL);
  else if (g_chat_ui.mode_panel) chat_ui_mode_close(NULL);
  else if (g_chat_ui.editor) chat_ui_editor_close(NULL);
  else if(g_chat_ui.review_shade&&!lv_obj_has_flag(g_chat_ui.review_shade,LV_OBJ_FLAG_HIDDEN))chat_workspace_close(NULL);
  else if (g_desktop.chat_return_apps) glass_apps(NULL);
  else panel_hide(NULL);
}

static void glass_chat_open(lv_event_t *e)
{
  LV_UNUSED(e);
  bool from_apps = g_desktop.apps_page || (g_desktop.chat_page && g_desktop.chat_return_apps);
  glass_chat_show(from_apps);
}

static void chat_ui_open_espclaw(lv_event_t *event)
{
  LV_UNUSED(event);
  chat_interface_modes[0]=g_chat_ui.mode;
  lv_obj_t *previous_root=g_chat_ui.root;
  g_chat_ui.camera_result_handoff=true;
  glass_chat_show(g_desktop.chat_return_apps);
  if(g_chat_ui.root==previous_root)g_chat_ui.camera_result_handoff=false;
}
static void glass_chat_ball_open(lv_event_t *event)
{
  LV_UNUSED(event);
  bool from_apps=g_desktop.apps_page||(g_desktop.chat_page&&g_desktop.chat_return_apps);
  glass_chat_show_mode(from_apps,true);
}
static void glass_chat_show(bool from_apps)
{
  glass_chat_show_mode(from_apps,false);
}
#include "glass_chat_ball.inc"

static void chat_ui_mode_close(lv_event_t *e)
{
  LV_UNUSED(e);if(g_chat_ui.mode_panel)lv_obj_delete(g_chat_ui.mode_panel);g_chat_ui.mode_panel=NULL;
}
static void chat_ui_mode_choose(lv_event_t *e)
{
  enum glass_agent_mode mode=(enum glass_agent_mode)(uintptr_t)lv_event_get_user_data(e);
  if(!glass_agent_mode_valid(mode))return;
  struct chat_activity_snapshot activity;glass_chat_activity_get(&activity);
  if(g_chat_ui.voice_session||(activity.loaded&&activity.phase!=CHAT_IDLE)){
    show_toast("请先结束当前对话，再切换模式");return;
  }
  bool ball=g_chat_ui.ball_mode,apps=g_desktop.chat_return_apps;
  chat_interface_modes[ball?1:0]=mode;glass_chat_show_mode(apps,ball);
}
static void chat_ui_modes(lv_event_t *e)
{
  LV_UNUSED(e);if(g_chat_ui.mode_panel)return;
  if(g_chat_ui.editor||g_chat_ui.voice_session){show_toast("请先结束输入或语音对话");return;}
  lv_obj_t *shade=glass_box(g_chat_ui.root,0,0,1024,CHAT_CONTENT_HEIGHT,0x000000,0);
  lv_obj_set_style_bg_opa(shade,LV_OPA_60,0);g_chat_ui.mode_panel=shade;
  lv_obj_t *box=glass_box(shade,282,100,460,370,g_chat_ui.background,22);
  chat_ui_text(box,"选择模式",24,20,412,24,g_chat_ui.primary,false);
  const enum glass_agent_mode modes[]={AGENT_MODE_CHAT,AGENT_MODE_CREATE,AGENT_MODE_ASSISTANT};
  const char *descriptions[]={"自然交流 · 不执行操作","制作应用 · 先预览草稿","查询与操作 · 已授权的工具"};
  for(unsigned i=0;i<3;i++){
    chat_ui_button(box,glass_agent_mode_title(modes[i]),24,66+i*80,140,48,modes[i]==g_chat_ui.mode,
                   chat_ui_mode_choose,(void *)(uintptr_t)modes[i]);
    chat_ui_text(box,descriptions[i],178,80+i*80,258,16,g_chat_ui.secondary,false);
  }
  chat_ui_button(box,"取消",170,310,120,40,false,chat_ui_mode_close,NULL);
}

#include "glass_chat_workspace.inc"

static void glass_chat_show_mode(bool from_apps,bool ball_mode)
{
  lv_obj_t *card = panel_card("ESPClaw"); if (!card) return;
  lv_obj_clean(card); memset(&g_chat_ui, 0, sizeof(g_chat_ui)); g_chat_ui.root = card;
  g_chat_ui.ball_mode=ball_mode;g_chat_ui.mode=chat_interface_modes[ball_mode?1:0];
  g_chat_ui.snapshot = calloc(1, sizeof(*g_chat_ui.snapshot));
  if (!g_chat_ui.snapshot) { memset(&g_chat_ui, 0, sizeof(g_chat_ui)); panel_hide(NULL); show_toast("内存不足，暂时无法打开聊天"); return; }
  chat_display_expand();
  g_desktop.chat_page = true; g_desktop.chat_return_apps = from_apps;
  bool light = g_desktop.light_theme;
  struct glass_app_palette palette=glass_app_palette(light);
  g_chat_ui.background=palette.canvas;g_chat_ui.surface=palette.paper;
  g_chat_ui.sidebar=palette.paper;g_chat_ui.primary=palette.ink;
  g_chat_ui.secondary=palette.muted;g_chat_ui.accent=palette.accent;
  g_chat_ui.on_accent=palette.on_accent;
  lv_obj_set_pos(card, 0, 0); lv_obj_set_size(card, 1024, CHAT_CONTENT_HEIGHT);
  lv_obj_set_style_radius(card, 0, 0); lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_set_style_bg_color(card, lv_color_hex(g_chat_ui.background), 0);
  lv_obj_add_event_cb(card, chat_ui_deleted, LV_EVENT_DELETE, NULL);
  if(ball_mode){chat_ball_create(card);return;}
  chat_workspace_create(card,from_apps);
}
