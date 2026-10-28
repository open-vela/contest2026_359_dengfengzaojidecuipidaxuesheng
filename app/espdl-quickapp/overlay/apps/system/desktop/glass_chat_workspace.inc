/* SPDX-License-Identifier: Apache-2.0 */
/* Native ESP workbench: mode navigation, conversation, contextual inspector. */
static void chat_workspace_border(lv_obj_t *object)
{
  lv_obj_set_style_border_width(object,1,0);
  lv_obj_set_style_border_color(object,lv_color_hex(detail_line()),0);
}
static void chat_workspace_voice(lv_event_t *event)
{
  LV_UNUSED(event);chat_interface_modes[1]=g_chat_ui.mode;
  glass_chat_show_mode(g_desktop.chat_return_apps,true);
}
static void chat_workspace_close(lv_event_t *event)
{
  LV_UNUSED(event);if(g_chat_ui.review_shade)lv_obj_add_flag(g_chat_ui.review_shade,LV_OBJ_FLAG_HIDDEN);
}
static void chat_workspace_inspect(lv_event_t *event)
{
  LV_UNUSED(event);if(!g_chat_ui.review_shade)return;
  lv_obj_remove_flag(g_chat_ui.review_shade,LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(g_chat_ui.review_shade);
}
static void chat_workspace_current_task(lv_event_t *event)
{
  LV_UNUSED(event);chat_workspace_close(NULL);
  lv_obj_scroll_to_y(g_chat_ui.messages,LV_COORD_MAX,LV_ANIM_OFF);
}
static void chat_workspace_refresh(void)
{
  if(!g_chat_ui.task_title||g_chat_ui.ball_mode)return;
  const struct chat_snapshot *s=g_chat_ui.snapshot;
  char title[161];
  if(s->count){glass_chat_copy_utf8(title,sizeof(title),s->turns[s->count-1].prompt);char *nl=strchr(title,'\n');if(nl)*nl=0;}
  else snprintf(title,sizeof(title),glass_tr("新任务"));
  chat_ui_label(g_chat_ui.task_title,title);
  chat_ui_label(g_chat_ui.sidebar_task,s->count?title:glass_tr("还没有对话"));
  chat_ui_label(g_chat_ui.artifact_name,g_chat_ui.mode==AGENT_MODE_CREATE?(g_chat_ui.app.name[0]?g_chat_ui.app.name:glass_tr("等待应用草稿")):
    g_chat_ui.mode==AGENT_MODE_ASSISTANT?glass_tr("Skills 与 MCP"):glass_tr("与你聊聊"));
  const struct chat_turn *last=s->count?&s->turns[s->count-1]:NULL;
  bool active=s->active_mode==g_chat_ui.mode&&(s->phase==CHAT_REQUESTING||s->phase==CHAT_QUEUED);
  lv_obj_set_style_bg_color(g_chat_ui.activity_dot,lv_color_hex(active?0xc59146:last&&last->state==CHAT_FAILED?0xc86659:g_chat_ui.secondary),0);
  /* Keep the same sprite/reader across empty and active conversations. */
  bool compact=s->count!=0;
  /* An active conversation starts just below the toolbar. Its own trace row
   * already shows progress, so the separate status strip needs no space. */
  int messages_top=compact?76:132;
  lv_obj_set_y(g_chat_ui.messages,messages_top);
  lv_obj_set_height(g_chat_ui.messages,CHAT_CONTENT_HEIGHT-152-messages_top);
  if(compact){
    lv_obj_add_flag(g_chat_ui.status,LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_chat_ui.activity_dot,LV_OBJ_FLAG_HIDDEN);
  }else{
    lv_obj_remove_flag(g_chat_ui.status,LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(g_chat_ui.activity_dot,LV_OBJ_FLAG_HIDDEN);
  }
  if(g_chat_ui.ball&&compact!=g_chat_ui.workspace_ball_compact){
    g_chat_ui.workspace_ball_compact=compact;
    lv_obj_set_pos(g_chat_ui.ball_mount,compact?196:CHAT_WORKSPACE_CENTER-64,compact?60:48);
    lv_obj_set_size(g_chat_ui.ball_mount,compact?64:128,compact?64:128);
    glass_grok_geometry(g_chat_ui.ball,0,0,compact?64:128,compact?64:128,compact?48:96);
  }
  if(g_chat_ui.ball){
    struct chat_activity_snapshot activity;glass_chat_activity_get(&activity);
    if(!last||activity.turn_id!=last->id){
      memset(&activity,0,sizeof(activity));activity.loaded=s->loaded;activity.configured=s->configured;
      activity.phase=CHAT_IDLE;if(last){activity.turn_id=last->id;activity.state=last->state;}
    }
    glass_grok_select(g_chat_ui.ball,glass_grok_emotion(&activity,NULL));
  }
}

static void chat_workspace_create(lv_obj_t *card,bool from_apps)
{
  lv_obj_t *sidebar=glass_box(card,0,0,GLASS_SIDEBAR_WIDTH,CHAT_CONTENT_HEIGHT,detail_paper(),0);
  glass_sidebar_surface(sidebar);
  chat_ui_text(sidebar,"ESPClaw",24,30,128,20,detail_ink(),false);
  lv_obj_t *new_task=chat_ui_button(sidebar,glass_tr("+  新任务"),12,96,140,44,false,chat_ui_new,NULL);glass_sidebar_item(new_task,false);
  chat_ui_text(sidebar,glass_tr("模式"),20,158,128,16,g_chat_ui.secondary,false);
  const enum glass_agent_mode modes[]={AGENT_MODE_CHAT,AGENT_MODE_CREATE,AGENT_MODE_ASSISTANT};
  const char *labels[]={glass_tr("聊天"),glass_tr("创作"),glass_tr("助手")};
  const char *icons[]={GLASS_ICON_CHAT,GLASS_ICON_SPARKLES,GLASS_ICON_SLIDERS};
  for(unsigned i=0;i<3;i++){
    lv_obj_t *button=chat_ui_button(sidebar,labels[i],12,186+i*52,140,44,false,chat_ui_mode_choose,(void *)(uintptr_t)modes[i]);
    glass_sidebar_item(button,modes[i]==g_chat_ui.mode);
    lv_obj_t *caption=lv_obj_get_child(button,0);
    lv_obj_set_width(caption,90);lv_obj_align(caption,LV_ALIGN_LEFT_MID,42,0);
    home_icon(button,icons[i],12,12,20,modes[i]==g_chat_ui.mode?detail_accent():detail_muted());
  }
  lv_obj_t *task=chat_ui_button(sidebar,glass_tr("还没有对话"),12,344,140,38,false,chat_workspace_current_task,NULL);
  glass_sidebar_item(task,false);
  g_chat_ui.sidebar_task=lv_obj_get_child(task,0);
  g_chat_ui.count=chat_ui_text(sidebar,glass_tr("当前模式 · 0 轮"),20,418,132,16,g_chat_ui.secondary,false);
  g_chat_ui.storage=chat_ui_text(sidebar,glass_tr("自动保存在设备"),20,440,132,16,g_chat_ui.secondary,false);
  lv_obj_add_flag(g_chat_ui.count,LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(g_chat_ui.storage,LV_OBJ_FLAG_HIDDEN);
  glass_sidebar_item(chat_ui_button(sidebar,glass_tr("能力设置"),12,CHAT_CONTENT_HEIGHT-108,140,44,false,chat_ui_config,NULL),false);
  detail_back_button(sidebar,from_apps?glass_tr("返回应用"):glass_tr("返回桌面"),glass_chat_back);

  g_chat_ui.task_title=chat_ui_text(card,glass_tr("新任务"),196,20,520,20,g_chat_ui.primary,false);
  chat_ui_button(card,g_chat_ui.mode==AGENT_MODE_CREATE?glass_tr("草稿"):glass_tr("工具"),830,14,82,38,false,chat_workspace_inspect,NULL);
  char voice[48];snprintf(voice,sizeof(voice),"%s%s",GLASS_ICON_MIC,glass_tr(" 语音"));
  chat_ui_button(card,voice,922,14,82,38,false,chat_workspace_voice,NULL);
  lv_obj_t *rule=glass_box(card,168,63,856,1,g_chat_ui.secondary,0);lv_obj_set_style_bg_opa(rule,LV_OPA_20,0);
  g_chat_ui.activity_dot=glass_box(card,196,82,6,6,g_chat_ui.secondary,3);
  g_chat_ui.status=chat_ui_text(card,glass_tr("正在载入任务…"),212,74,680,16,g_chat_ui.secondary,false);
  g_chat_ui.messages=glass_box(card,180,132,826,CHAT_CONTENT_HEIGHT-284,g_chat_ui.background,0);
  lv_obj_add_flag(g_chat_ui.messages,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_scroll_dir(g_chat_ui.messages,LV_DIR_VER);
  lv_obj_set_scrollbar_mode(g_chat_ui.messages,LV_SCROLLBAR_MODE_AUTO);
  lv_obj_add_event_cb(g_chat_ui.messages,chat_ui_ball_position,LV_EVENT_SCROLL,NULL);
  g_chat_ui.ball_mount=glass_box(card,CHAT_WORKSPACE_CENTER-64,48,128,128,g_chat_ui.background,0);
  lv_obj_set_style_bg_opa(g_chat_ui.ball_mount,LV_OPA_TRANSP,0);lv_obj_remove_flag(g_chat_ui.ball_mount,LV_OBJ_FLAG_CLICKABLE);
  g_chat_ui.ball=glass_grok_create_sprite(g_chat_ui.ball_mount,0,0,128,128,96,!g_desktop.light_theme,zh_font);
  glass_grok_set_notice_text(g_chat_ui.ball,glass_tr("加载小球…"),glass_tr("小球素材未就绪"));
  glass_grok_set_reduced_motion(g_chat_ui.ball,g_glass.reduced_motion);
  if(g_chat_ui.ball){glass_grok_gaze(g_chat_ui.ball,0,0);glass_clickable(glass_grok_root(g_chat_ui.ball),chat_workspace_voice,NULL);}

  lv_obj_t *composer=glass_box(card,CHAT_WORKSPACE_LEFT,CHAT_CONTENT_HEIGHT-130,CHAT_WORKSPACE_WIDTH,104,g_chat_ui.surface,22);chat_workspace_border(composer);
  glass_clickable(composer,chat_ui_editor_open,NULL);
  g_chat_ui.draft_label=chat_ui_text(composer,chat_draft[0]?chat_draft:chat_ui_placeholder(),20,16,764,20,g_chat_ui.secondary,false);
  lv_obj_set_height(g_chat_ui.draft_label,32);
  chat_ui_button(composer,glass_tr(glass_agent_mode_title(g_chat_ui.mode)),14,61,122,32,false,chat_ui_modes,NULL);
  g_chat_ui.model=chat_ui_text(composer,"",152,69,530,16,g_chat_ui.secondary,false);
  g_chat_ui.action=chat_ui_button(composer,glass_tr("发送"),720,59,68,38,true,chat_ui_action,NULL);
  g_chat_ui.action_label=lv_obj_get_child(g_chat_ui.action,0);
  chat_ui_text(card,glass_tr("轻点小球进入语音 · 轻点输入框描述任务"),200,CHAT_CONTENT_HEIGHT-22,800,16,g_chat_ui.secondary,false);

  /* The inspector stays in this page. Closing it never deletes the ball. */
  g_chat_ui.review_shade=glass_box(card,0,0,1024,CHAT_CONTENT_HEIGHT,0x000000,0);
  lv_obj_set_style_bg_opa(g_chat_ui.review_shade,LV_OPA_20,0);glass_clickable(g_chat_ui.review_shade,chat_workspace_close,NULL);
  lv_obj_t *panel=glass_box(g_chat_ui.review_shade,676,76,328,476,g_chat_ui.surface,20);chat_workspace_border(panel);
  chat_ui_text(panel,g_chat_ui.mode==AGENT_MODE_CREATE?glass_tr("草稿与进度"):glass_tr("工具与技能"),20,22,240,20,g_chat_ui.primary,false);
  chat_ui_button(panel,LV_SYMBOL_CLOSE,264,16,44,38,false,chat_workspace_close,NULL);
  g_chat_ui.artifact_name=chat_ui_text(panel,"",20,88,288,20,g_chat_ui.primary,true);lv_obj_set_height(g_chat_ui.artifact_name,62);
  g_chat_ui.app_status=chat_ui_text(panel,glass_tr("正在载入…"),20,168,288,16,g_chat_ui.secondary,true);lv_obj_set_height(g_chat_ui.app_status,74);
  g_chat_ui.preview_button=chat_ui_button(panel,glass_tr("预览草稿"),20,266,288,44,false,chat_ui_preview_app,NULL);chat_workspace_border(g_chat_ui.preview_button);
  g_chat_ui.save_button=chat_ui_button(panel,glass_tr("安装应用"),20,320,288,44,true,chat_ui_save_app,NULL);
  g_chat_ui.discard_button=chat_ui_button(panel,glass_tr("放弃草稿"),20,380,288,38,false,chat_ui_discard,NULL);
  if(g_chat_ui.mode!=AGENT_MODE_CREATE){
    lv_obj_add_flag(g_chat_ui.preview_button,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(g_chat_ui.save_button,LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_chat_ui.discard_button,LV_OBJ_FLAG_HIDDEN);
    chat_ui_text(panel,g_chat_ui.mode==AGENT_MODE_CHAT?glass_tr("聊天模式不执行外部操作。"):glass_tr("助手使用你已配置并授权的工具。"),20,264,288,16,g_chat_ui.secondary,true);
    chat_ui_button(panel,glass_tr("管理能力"),20,346,288,44,false,chat_ui_config,NULL);
  }
#ifdef GLASS_LOCAL_SPEECH
  chat_ui_button(panel,glass_tr("端侧语音"),20,428,288,32,false,glass_speech_open,NULL);
#endif
  lv_obj_add_flag(g_chat_ui.review_shade,LV_OBJ_FLAG_HIDDEN);
  glass_chat_refresh_config();if(g_chat_ui.mode==AGENT_MODE_CREATE)(void)glass_qpk_start();
  g_chat_ui.timer=lv_timer_create(chat_ui_refresh,250,NULL);chat_ui_refresh(NULL);
}
