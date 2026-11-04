/* SPDX-License-Identifier: Apache-2.0 */
#ifdef GLASS_LOCAL_VOICE
#include "glass_local_voice.h"
#include "glass_local_model.h"
struct local_voice_ui {
  lv_obj_t *card,*heading,*subtitle,*output,*input,*keyboard,*status,*metrics,*send,*check,*wake,*target,*result,*microphone,*level,*footer,*speak,*thinking,*save;
  lv_obj_t *navigation[3];
  lv_timer_t *timer;enum glv_mode mode;uint32_t request,revision,transcript_request;unsigned elapsed_second;char reply[GLV_MAX_REPLY],transcript[1025];
};
static struct local_voice_ui *g_local_ui;
static void local_sidebar_update(void)
{
 if(!g_local_ui)return;
 for(unsigned i=0;i<3;i++)if(g_local_ui->navigation[i])glass_sidebar_item(g_local_ui->navigation[i],(unsigned)g_local_ui->mode==i);
}
static void glass_local_voice_open(lv_event_t *);
struct local_settings_ui {
  lv_obj_t *card,*wake,*target,*check,*status,*monitor,*phrase;
  lv_timer_t *timer;uint32_t revision;
};
static struct local_settings_ui *g_local_settings;
static const char *local_asr_problem(const struct glv_snapshot *s,char *text,size_t cap)
{
  if(s->busy||s->config_pending||s->error||!s->models_ready||s->asr_ready||!s->asr_error)return NULL;
  snprintf(text,cap,"离线转写：%s · 文字功能可用",glass_local_voice_error(s->asr_error));return text;
}
static void local_settings_poll(lv_timer_t *timer)
{
  LV_UNUSED(timer);struct local_settings_ui *ui=g_local_settings;if(!ui)return;
  struct glv_snapshot s;glass_local_voice_get(&s,NULL,0);
  if(ui->revision==s.revision)return;ui->revision=s.revision;
  lv_obj_t *controls[]={ui->wake,ui->target,ui->check};
  for(unsigned i=0;i<3;i++)if(s.busy||s.config_pending)lv_obj_add_state(controls[i],LV_STATE_DISABLED);else lv_obj_remove_state(controls[i],LV_STATE_DISABLED);
  if(s.wake_enabled)lv_obj_add_state(ui->wake,LV_STATE_CHECKED);else lv_obj_remove_state(ui->wake,LV_STATE_CHECKED);
  lv_dropdown_set_selected(ui->target,0);lv_obj_add_state(ui->target,LV_STATE_DISABLED);
  lv_label_set_text(ui->phrase,"使用原助手的唤醒配置");
  lv_label_set_text(ui->monitor,s.config_pending?"正在保存唤醒设置…":s.wake_enabled?"开启后在后台等待唤醒；其他音频任务使用麦克风时会暂停。":"已关闭全局唤醒，APP 的文字对话和朗读仍可使用。");
  int error=s.error?s.error:!s.models_ready?s.model_error:0;
  char problem[192];const char *asr=local_asr_problem(&s,problem,sizeof(problem));
  lv_label_set_text(ui->status,error?glass_local_voice_error(error):s.wake_error?"唤醒模型不可用，可关闭后重新开启":asr?asr:s.busy?"正在处理，请等待或在 APP 中停止":s.models_ready?"大模型已校验 · 语音功能按需加载":"尚未加载模型 · 使用时自动准备");
}
static void local_settings_action(lv_event_t *event)
{
  if(!g_local_settings)return;struct glv_snapshot s;glass_local_voice_get(&s,NULL,0);
  if(s.busy||s.config_pending)return;
  intptr_t action=(intptr_t)lv_event_get_user_data(event);int ret=0;
  if(action==1)ret=glass_local_voice_enable_wake(lv_obj_has_state(g_local_settings->wake,LV_STATE_CHECKED));
  else if(action==2)ret=glass_local_voice_set_wake(false);
  else if(action==3)ret=glass_local_voice_check(NULL);
  if(ret)show_toast(glass_local_voice_error(ret));g_local_settings->revision=UINT32_MAX;local_settings_poll(NULL);
}
static void local_settings_deleted(lv_event_t *event)
{
  if(!g_local_settings||g_local_settings->card!=lv_event_get_target(event))return;
  struct local_settings_ui *ui=g_local_settings;g_local_settings=NULL;
  if(ui->timer)lv_timer_delete(ui->timer);free(ui);
}
static void glass_local_voice_settings(lv_event_t *event)
{
  LV_UNUSED(event);struct local_settings_ui *ui=calloc(1,sizeof(*ui));if(!ui){show_toast("内存不足");return;}
  lv_obj_t *card=panel_card("本地语音设置");if(!card){free(ui);return;}
  detail_page(card);ui->card=card;ui->revision=UINT32_MAX;g_local_settings=ui;
  lv_obj_add_event_cb(card,local_settings_deleted,LV_EVENT_DELETE,NULL);
  lv_obj_t *back=glass_settings_sidebar(card,SETTINGS_LOCAL_VOICE);g_system_page_kind=SETTINGS_LOCAL_VOICE;
  lv_obj_t *back_label=lv_obj_get_child(back,0);
  lv_obj_set_width(back_label,GLASS_SIDEBAR_ITEM_WIDTH-24);lv_obj_set_style_text_align(back_label,LV_TEXT_ALIGN_LEFT,0);
  lv_obj_align(back_label,LV_ALIGN_LEFT_MID,12,0);
  detail_text(card,"本地语音",212,30,600,28,detail_ink());
  detail_text(card,"本地语音仅在 APP 中使用 · 全局唤醒使用原助手",212,70,700,16,detail_muted());
  lv_obj_t *tile=detail_tile(card,212,108,788,150);
  detail_text(tile,"全局唤醒",24,22,560,20,detail_ink());
  ui->phrase=detail_text(tile,"",24,59,600,16,detail_muted());
  ui->monitor=detail_text(tile,"",24,103,732,14,detail_muted());
  ui->wake=lv_switch_create(tile);glass_control_switch_style(ui->wake);
  lv_obj_set_pos(ui->wake,710,22);lv_obj_set_size(ui->wake,52,28);
  lv_obj_set_style_bg_color(ui->wake,lv_color_hex(detail_accent()),LV_PART_INDICATOR|LV_STATE_CHECKED);
  lv_obj_add_event_cb(ui->wake,local_settings_action,LV_EVENT_VALUE_CHANGED,(void *)1);
  tile=detail_tile(card,212,278,788,106);
  detail_text(tile,"唤醒后打开",24,22,340,20,detail_ink());
  detail_text(tile,"固定使用原助手",24,60,340,14,detail_muted());
  ui->target=lv_dropdown_create(tile);glass_input_style(ui->target);
  lv_obj_set_pos(ui->target,420,28);lv_obj_set_size(ui->target,340,44);
  lv_obj_set_style_text_font(ui->target,home_font(16),0);
  lv_obj_set_style_text_font(ui->target,glass_icon_font(16),LV_PART_INDICATOR);
  lv_dropdown_set_options(ui->target,"原助手");
  lv_obj_t *list=lv_dropdown_get_list(ui->target);
  lv_obj_set_style_text_font(list,home_font(16),0);lv_obj_set_style_text_color(list,lv_color_hex(detail_ink()),0);
  lv_obj_set_style_bg_color(list,lv_color_hex(detail_paper()),0);
  lv_obj_add_event_cb(ui->target,local_settings_action,LV_EVENT_VALUE_CHANGED,(void *)2);
  tile=detail_tile(card,212,404,788,112);
  detail_text(tile,"模型资源",24,20,540,20,detail_ink());
  ui->status=detail_text(tile,"",24,62,560,14,detail_muted());
  ui->check=detail_button(tile,"完整检查",620,28,140,local_settings_action,(void *)3);
  detail_text(card,"本地 APP 使用离线转写与朗读；全局唤醒沿用原助手的语音设置。",212,536,788,14,detail_muted());
  ui->timer=lv_timer_create(local_settings_poll,200,NULL);local_settings_poll(NULL);
}
static void local_ui_poll(lv_timer_t *timer);
static void local_thinking(lv_event_t *event)
{
 LV_UNUSED(event);if(!g_local_ui)return;
 int ret=glass_local_voice_thinking(lv_dropdown_get_selected(g_local_ui->thinking)+4);
 if(ret)show_toast(glass_local_voice_error(ret));g_local_ui->revision=UINT32_MAX;local_ui_poll(NULL);
}
static void local_keyboard_close(void)
{
  if(!g_local_ui||!g_local_ui->keyboard)return;
  lv_obj_t *keyboard=g_local_ui->keyboard;g_local_ui->keyboard=NULL;
  glass_ime_commit(keyboard);lv_obj_delete(keyboard);
  lv_obj_set_y(g_local_ui->input,354);lv_obj_remove_flag(g_local_ui->result,LV_OBJ_FLAG_HIDDEN);
}
static void local_keyboard_ready(lv_event_t *event){LV_UNUSED(event);local_keyboard_close();}
static void local_input_focus(lv_event_t *event)
{
  LV_UNUSED(event);if(!g_local_ui||g_local_ui->keyboard)return;
  struct local_voice_ui *ui=g_local_ui;
  lv_obj_add_flag(ui->result,LV_OBJ_FLAG_HIDDEN);lv_obj_set_y(ui->input,108);
  ui->keyboard=lv_keyboard_create(ui->card);
  glass_keyboard_style(ui->keyboard);
  /* LVGL keyboards start bottom-centred. Select the coordinate origin
   * explicitly, and reserve 48 px above the keys for the IME candidate bar. */
  lv_obj_set_size(ui->keyboard,788,GLASS_CONTENT_HEIGHT-232);
  lv_obj_align(ui->keyboard,LV_ALIGN_TOP_LEFT,212,232);
  lv_keyboard_set_textarea(ui->keyboard,ui->input);
  glass_ime_attach(ui->keyboard,home_font(18),true,detail_ink(),detail_paper(),detail_accent());
  glass_ime_set_voice_enabled(ui->keyboard,false);
  lv_obj_add_event_cb(ui->keyboard,local_keyboard_ready,LV_EVENT_READY,NULL);
  lv_obj_add_event_cb(ui->keyboard,local_keyboard_ready,LV_EVENT_CANCEL,NULL);
}
static const char *local_phase_text(enum glv_phase phase)
{
  static const char *names[]={"待命","检查 SD 模型","加载对话模型","正在思考","处理中文发音","加载 V2 声音","正在合成","正在朗读","正在停止","处理未完成","加载本地识别模型","正在转写","正在聆听","检查转写模型","处理输入文字","计算首个回答","执行设备并回读"};
  return (unsigned)phase<sizeof(names)/sizeof(names[0])?names[phase]:"待命";
}
static void local_ui_poll(lv_timer_t *timer)
{
  LV_UNUSED(timer);struct local_voice_ui *ui=g_local_ui;if(!ui)return;
  struct glv_snapshot s;glass_local_voice_get(&s,ui->reply,sizeof(ui->reply));
  bool changed=ui->revision!=s.revision;unsigned seconds=s.elapsed_ms/1000;
  if(!changed&&(!s.busy||ui->elapsed_second==seconds))return;
  ui->revision=s.revision;ui->elapsed_second=seconds;
  if(changed){
    char transcript[sizeof(ui->transcript)];
    bool received=glass_local_voice_get_transcript(s.request,transcript,sizeof(transcript));
    /* Update only for new ASR text; later phase changes must keep user edits. */
    if(received&&(ui->transcript_request!=s.request||strcmp(ui->transcript,transcript)))
      lv_textarea_set_text(ui->input,transcript);
    snprintf(ui->transcript,sizeof(ui->transcript),"%s",transcript);ui->transcript_request=s.request;
    if(s.asr_streaming)lv_label_set_text(ui->output,ui->transcript[0]?ui->transcript:"请说话，正在流式识别……");
    else if(ui->reply[0])lv_label_set_text(ui->output,ui->reply);
    else if(ui->transcript[0])lv_label_set_text(ui->output,ui->transcript);
    else lv_label_set_text(ui->output,ui->mode==GLV_READ?"输入中文，使用你的 V2 声音朗读。":ui->mode==GLV_CONTROL?"例如：打开台灯、关闭床头灯、查询台灯状态。\n只操作已注册清单中的设备。":"你好，我是本地语音。\n输入文字开始一段离线对话。");
    int error=s.error?s.error:(!s.busy&&ui->mode!=GLV_READ&&!s.models_ready?s.model_error:0);
    const char *detail=!s.error&&error?"请检查 SD 模型后重试":s.detail;
    char state[256];snprintf(state,sizeof(state),"%s%s%s",error?glass_local_voice_error(error):local_phase_text(s.phase),detail[0]?" · ":"",detail);
    if(!error&&s.phase==GLV_PREFILL&&s.progress_total)
      snprintf(state,sizeof(state),"处理输入 · %u%% · 完成后开始回答",s.progress_done*100/s.progress_total);
    if(s.capture_pending&&!s.asr_streaming)snprintf(state,sizeof(state),"%s · 麦克风尚未开始，请等待聆听提示",local_phase_text(s.phase));
    if(s.asr_streaming)snprintf(state,sizeof(state),"%s · 录音 %.1f秒 · 已识别 %.1f秒 · 积压 %.1f秒",s.capture_active?"边录边转写":s.capture_pending?"准备麦克风":"录音已结束，继续识别",s.capture_ms/1000.f,s.asr_decoded_ms/1000.f,s.asr_backlog_ms/1000.f);
    if(s.error==-ECANCELED&&s.cancel_source==GLV_CANCEL_CONFIG)
      snprintf(state,sizeof(state),"设置变更，已停止本次任务");
    char problem[192];const char *asr=ui->mode!=GLV_READ?local_asr_problem(&s,problem,sizeof(problem)):NULL;lv_label_set_text(ui->status,asr?asr:state);
  }
  char metrics[240];snprintf(metrics,sizeof(metrics),"%.1f MiB · 总用时 %.1f 秒 · 录音 %.1f 秒 · 转写 %.1f 秒 · 回答 %u token",
    s.memory_peak/1048576.0,s.elapsed_ms/1000.0,s.capture_ms/1000.0,s.asr_elapsed_ms/1000.0,s.tokens);
  if(ui->mode==GLV_CONTROL)snprintf(metrics,sizeof(metrics),"Thinking %u / 最多 %u · 工具 %u · %.1f 秒 · %.1f MiB",s.model_rounds,s.thinking_rounds,s.tool_rounds,s.elapsed_ms/1000.0,s.memory_peak/1048576.0);
  if(s.asr_streaming)snprintf(metrics,sizeof(metrics),"流式转写 · 积压 %.1f 秒 · 已出 %u token · %.1f MiB",s.asr_backlog_ms/1000.f,s.asr_tokens,s.memory_peak/1048576.0);
  lv_label_set_text(ui->metrics,metrics);
  lv_dropdown_set_selected(ui->thinking,s.thinking_rounds>=4&&s.thinking_rounds<=6?s.thinking_rounds-4:1);
  if(!changed)return;
  lv_obj_t *buttons[]={ui->send,ui->check,ui->wake,ui->target};
  for(unsigned i=0;i<4;i++)if(s.busy||s.config_pending)lv_obj_add_state(buttons[i],LV_STATE_DISABLED);else lv_obj_remove_state(buttons[i],LV_STATE_DISABLED);
  if(s.busy||s.config_pending||ui->mode!=GLV_CONTROL)lv_obj_add_state(ui->thinking,LV_STATE_DISABLED);else lv_obj_remove_state(ui->thinking,LV_STATE_DISABLED);
  if(s.busy||s.config_pending||ui->mode!=GLV_DIALOGUE)lv_obj_add_state(ui->save,LV_STATE_DISABLED);else lv_obj_remove_state(ui->save,LV_STATE_DISABLED);
  bool recording=s.busy&&s.phase==GLV_RECORDING&&(!s.asr_streaming||s.capture_active);
  lv_label_set_text(lv_obj_get_child(ui->microphone,0),recording?"结束录音":"语音输入");
  if(ui->mode==GLV_READ||s.config_pending||(!recording&&s.busy))lv_obj_add_state(ui->microphone,LV_STATE_DISABLED);
  else lv_obj_remove_state(ui->microphone,LV_STATE_DISABLED);
  if(s.busy)lv_obj_add_state(ui->input,LV_STATE_DISABLED);else lv_obj_remove_state(ui->input,LV_STATE_DISABLED);
  unsigned level=recording?s.input_level:0;if(level>32768)level=32768;
  lv_bar_set_value(ui->level,(int)(level*100/32768),LV_ANIM_OFF);
  if(recording){lv_obj_add_flag(ui->speak,LV_OBJ_FLAG_HIDDEN);lv_obj_remove_flag(ui->level,LV_OBJ_FLAG_HIDDEN);}
  else {lv_obj_add_flag(ui->level,LV_OBJ_FLAG_HIDDEN);if(ui->mode!=GLV_READ)lv_obj_remove_flag(ui->speak,LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(ui->speak,LV_OBJ_FLAG_HIDDEN);}
  if(s.busy||s.config_pending||!ui->reply[0])lv_obj_add_state(ui->speak,LV_STATE_DISABLED);else lv_obj_remove_state(ui->speak,LV_STATE_DISABLED);
  lv_label_set_text(ui->footer,recording?"边说边转写；停顿或结束录音后提交最终文字。单次最长60秒。":ui->mode==GLV_READ?"V2 声音按需加载 · 24 kHz · 可随时停止":ui->mode==GLV_CONTROL?"按清单执行；状态回读匹配后完成即结束。":"纯文字回答 · 点击朗读回答或语音输入时再准备语音功能");
  if(s.asr_streaming)lv_label_set_text(ui->footer,s.capture_active?"边说边转写；结束后提交最终文字。单次最长60秒。":s.capture_pending?"正在准备麦克风，请稍候。":"录音已结束，正在完成剩余转写；可点击停止取消。");
  if(s.wake_enabled)lv_obj_add_state(ui->wake,LV_STATE_CHECKED);else lv_obj_remove_state(ui->wake,LV_STATE_CHECKED);
  lv_label_set_text(lv_obj_get_child(ui->target,0),"目标：原助手");lv_obj_add_state(ui->target,LV_STATE_DISABLED);
}
static void local_action(lv_event_t *event)
{
  if(!g_local_ui)return;intptr_t action=(intptr_t)lv_event_get_user_data(event);int ret=0;
  if(action==1){local_keyboard_close();ret=glass_local_voice_send(lv_textarea_get_text(g_local_ui->input),g_local_ui->mode,&g_local_ui->request);}
  else if(action==2)glass_local_voice_cancel_from(GLV_CANCEL_UI_STOP);
  else if(action==3)ret=glass_local_voice_new_from(GLV_CANCEL_UI_NEW);
  else if(action==4)ret=glass_local_voice_check(&g_local_ui->request);
  else if(action==5)ret=glass_local_voice_enable_wake(lv_obj_has_state(g_local_ui->wake,LV_STATE_CHECKED));
  else if(action==6)ret=glass_local_voice_save(&g_local_ui->request);
  else if(action==7)ret=glass_local_voice_set_wake(false);
  else if(action==8){
    local_keyboard_close();struct glv_snapshot s;glass_local_voice_get(&s,NULL,0);
    if(s.busy&&s.phase==GLV_RECORDING)ret=glass_local_voice_finish_capture(s.request);
    else ret=g_local_ui->mode==GLV_CONTROL?glass_local_voice_listen_control(&g_local_ui->request):glass_local_voice_listen(&g_local_ui->request);
  }
  else if(action==9){local_keyboard_close();ret=glass_local_voice_send(g_local_ui->reply,GLV_READ,&g_local_ui->request);}
  if(ret)show_toast(glass_local_voice_error(ret));g_local_ui->revision=UINT32_MAX;local_ui_poll(NULL);
}
static void local_tab(lv_event_t *event)
{
  if(!g_local_ui)return;enum glv_mode mode=(enum glv_mode)(intptr_t)lv_event_get_user_data(event);
  if(mode==g_local_ui->mode)return;
  int ret=glass_local_voice_select(mode);if(ret){show_toast(glass_local_voice_error(ret));return;}
  local_keyboard_close();g_local_ui->mode=mode;
  local_sidebar_update();
  lv_label_set_text(g_local_ui->heading,mode==GLV_READ?"文字朗读":mode==GLV_CONTROL?"小助手":"普通对话");
  lv_label_set_text(g_local_ui->subtitle,mode==GLV_READ?"V2 声音 · 舒缓 · 24 kHz":mode==GLV_CONTROL?"控制9000 · Thinking 最多 Low 4 / Medium 5 / High 6":"原始9.87M W8A8 · 独立上下文 · 不操作设备");
  lv_label_set_text(lv_obj_get_child(g_local_ui->send,0),mode==GLV_READ?"朗读":mode==GLV_CONTROL?"执行":"发送");
  g_local_ui->revision=UINT32_MAX;local_ui_poll(NULL);
}
static void local_ui_deleted(lv_event_t *event)
{
  if(!g_local_ui||g_local_ui->card!=lv_event_get_target(event))return;
  struct local_voice_ui *ui=g_local_ui;g_local_ui=NULL;
  /* Leaving the page closes an active foreground recording. Model work and
   * global wake belong to the service, never to this view's timer/widgets. */
  struct glv_snapshot s;glass_local_voice_get(&s,NULL,0);
  if(s.busy&&s.capture_pending&&s.request==ui->request)glass_local_voice_stop_from(ui->request,GLV_CANCEL_VIEW_CLOSE);
  glass_local_voice_release();
  if(ui->timer)lv_timer_delete(ui->timer);free(ui);
}
static void glass_local_voice_open(lv_event_t *event)
{
  LV_UNUSED(event);if(g_local_ui)return;
  struct local_voice_ui *ui=calloc(1,sizeof(*ui));if(!ui){show_toast("内存不足");return;}
  lv_obj_t *card=panel_card("本地语音");if(!card){free(ui);return;}
  detail_page(card);ui->card=card;ui->revision=UINT32_MAX;g_local_ui=ui;
  lv_obj_add_event_cb(card,local_ui_deleted,LV_EVENT_DELETE,NULL);
  detail_sidebar(card,"本地语音",GLASS_ICON_MIC,glass_apps);g_desktop.page_back=glass_apps;
  const int nav_step=GLASS_SIDEBAR_ITEM_HEIGHT+GLASS_SIDEBAR_GAP;
  ui->navigation[GLV_DIALOGUE]=detail_nav(card,"普通对话",GLASS_ICON_SPARKLES,96,GLASS_SIDEBAR_ITEM_WIDTH,local_tab,(void *)GLV_DIALOGUE);
  ui->navigation[GLV_READ]=detail_nav(card,"文字朗读",LV_SYMBOL_AUDIO,96+nav_step,GLASS_SIDEBAR_ITEM_WIDTH,local_tab,(void *)GLV_READ);
#ifdef GLASS_LOCAL_CONTROL
  ui->navigation[GLV_CONTROL]=detail_nav(card,"小助手",LV_SYMBOL_POWER,96+2*nav_step,GLASS_SIDEBAR_ITEM_WIDTH,local_tab,(void *)GLV_CONTROL);
#endif
  local_sidebar_update();
  detail_text(card,"全局唤醒",24,270,116,16,detail_ink());
  detail_text(card,"原助手",24,299,116,14,detail_muted());
  ui->wake=lv_switch_create(card);lv_obj_set_pos(ui->wake,24,338);lv_obj_set_size(ui->wake,52,28);
  lv_obj_set_style_bg_color(ui->wake,lv_color_hex(detail_accent()),LV_PART_INDICATOR|LV_STATE_CHECKED);
  lv_obj_set_style_bg_opa(ui->wake,LV_OPA_COVER,LV_PART_INDICATOR|LV_STATE_CHECKED);
  lv_obj_add_event_cb(ui->wake,local_action,LV_EVENT_VALUE_CHANGED,(void *)5);
  detail_text(card,"关闭后停止后台监听",24,382,116,14,detail_muted());
  ui->target=detail_button(card,"目标：原助手",GLASS_SIDEBAR_PAD,418,GLASS_SIDEBAR_ITEM_WIDTH,local_action,(void *)7);
  ui->check=detail_button(card,"完整检查",GLASS_SIDEBAR_PAD,478,GLASS_SIDEBAR_ITEM_WIDTH,local_action,(void *)4);
  ui->heading=detail_text(card,"普通对话",212,30,520,28,detail_ink());
  detail_text(card,"Thinking",810,8,190,14,detail_muted());
  ui->thinking=lv_dropdown_create(card);glass_input_style(ui->thinking);
  lv_obj_set_pos(ui->thinking,810,32);lv_obj_set_size(ui->thinking,190,44);
  lv_obj_set_style_text_font(ui->thinking,home_font(16),0);
  lv_obj_set_style_text_font(ui->thinking,glass_icon_font(16),LV_PART_INDICATOR);
  lv_dropdown_set_options(ui->thinking,"Low\nMedium\nHigh");lv_dropdown_set_selected(ui->thinking,1);
  lv_obj_t *thinking_list=lv_dropdown_get_list(ui->thinking);
  lv_obj_set_style_text_font(thinking_list,home_font(16),0);lv_obj_set_style_text_color(thinking_list,lv_color_hex(detail_ink()),0);
  lv_obj_set_style_bg_color(thinking_list,lv_color_hex(detail_paper()),0);
  lv_obj_add_event_cb(ui->thinking,local_thinking,LV_EVENT_VALUE_CHANGED,NULL);
  ui->subtitle=detail_text(card,"EdgeIdiot · 默认文字回答",212,70,600,16,detail_muted());
  ui->result=detail_tile(card,212,108,788,224);lv_obj_add_flag(ui->result,LV_OBJ_FLAG_SCROLLABLE);
  ui->output=home_text(ui->result,"",22,22,740,20,detail_ink());lv_label_set_long_mode(ui->output,LV_LABEL_LONG_WRAP);
  ui->input=lv_textarea_create(card);lv_obj_set_pos(ui->input,212,354);lv_obj_set_size(ui->input,788,66);
  lv_obj_set_style_text_font(ui->input,home_font(18),0);lv_obj_set_style_text_color(ui->input,lv_color_hex(detail_ink()),0);
  lv_obj_set_style_bg_color(ui->input,lv_color_hex(detail_paper()),0);lv_obj_set_style_radius(ui->input,12,0);
  lv_textarea_set_max_length(ui->input,256);lv_textarea_set_placeholder_text(ui->input,"输入文字，开始对话或朗读");
  lv_obj_add_event_cb(ui->input,local_input_focus,LV_EVENT_CLICKED,NULL);
  ui->send=detail_button(card,"发送",212,438,100,local_action,(void *)1);
  detail_button(card,"停止",324,438,100,local_action,(void *)2);
  detail_button(card,"新对话",436,438,100,local_action,(void *)3);
  ui->save=detail_button(card,"保存会话",548,438,100,local_action,(void *)6);
  ui->microphone=detail_button(card,"语音输入",660,438,132,local_action,(void *)8);
  ui->speak=detail_button(card,"朗读回答",814,438,182,local_action,(void *)9);
  ui->level=lv_bar_create(card);lv_obj_set_pos(ui->level,814,454);lv_obj_set_size(ui->level,182,10);
  lv_obj_set_style_bg_color(ui->level,lv_color_hex(detail_accent()),LV_PART_INDICATOR);
  ui->status=detail_text(card,"",212,490,788,14,detail_muted());
  ui->metrics=detail_text(card,"",212,515,788,14,detail_muted());
  ui->footer=detail_text(card,"",212,542,788,14,detail_muted());
  ui->timer=lv_timer_create(local_ui_poll,200,NULL);local_ui_poll(NULL);
}
#include "glass_local_wake_popup.inc"

#else
static void glass_local_voice_open(lv_event_t *e){LV_UNUSED(e);show_toast("本地语音未编入当前版本");}
#endif
