/* SPDX-License-Identifier: Apache-2.0 */
/* Wake owns a temporary overlay, never the page underneath it. */
static struct {
  lv_obj_t *root,*dialog,*text;
  struct glass_grok *bird;
  lv_timer_t *timer;
  uint32_t request,revision,idle_at;
  bool speech_started;
  enum glv_mode mode;
  char transcript[1025],reply[GLV_MAX_REPLY];
} g_local_wake;

static void local_wake_close(void)
{
  if(!g_local_wake.root)return;
  struct glv_snapshot s;glass_local_voice_get(&s,NULL,0);
  if(g_local_wake.request&&s.request==g_local_wake.request){
    glass_local_voice_stop_from(g_local_wake.request,GLV_CANCEL_VIEW_CLOSE);
    glass_local_voice_release();
  }
  lv_obj_t *root=g_local_wake.root;
  if(g_local_wake.timer)lv_timer_delete(g_local_wake.timer);
  memset(&g_local_wake,0,sizeof(g_local_wake));
  lv_obj_add_flag(root,LV_OBJ_FLAG_HIDDEN);lv_obj_delete_async(root);
}
static void local_wake_outside(lv_event_t *event)
{
  LV_UNUSED(event);if(!g_local_wake.root)return;
  lv_indev_t *input=lv_indev_active();if(!input)return;
  lv_point_t p;lv_indev_get_point(input,&p);
  lv_area_t a;lv_obj_get_coords(g_local_wake.dialog,&a);
  if((p.x>=a.x1&&p.x<=a.x2&&p.y>=a.y1&&p.y<=a.y2)||
     glass_grok_hit_test(g_local_wake.bird,p.x,p.y))return;
  local_wake_close();
}
static void local_wake_poll(lv_timer_t *timer)
{
  LV_UNUSED(timer);if(!g_local_wake.root)return;
  struct glv_snapshot s;glass_local_voice_get(&s,g_local_wake.reply,sizeof(g_local_wake.reply));
  if(s.request!=g_local_wake.request){local_wake_close();return;}
  if(!s.busy&&g_local_wake.idle_at&&lv_tick_get()-g_local_wake.idle_at>=6000){local_wake_close();return;}
  if(s.revision==g_local_wake.revision)return;g_local_wake.revision=s.revision;
  bool transcript=glass_local_voice_get_transcript(s.request,g_local_wake.transcript,sizeof(g_local_wake.transcript));
  const char *text=s.error?glass_local_voice_error(s.error):g_local_wake.reply[0]?g_local_wake.reply:
    transcript?g_local_wake.transcript:s.phase==GLV_RECORDING?"我在听，请说话…":
    s.busy?"稍等，正在准备…":"我在，请说话。";
  lv_label_set_text(g_local_wake.text,text);
  glass_grok_select(g_local_wake.bird,s.phase==GLV_RECORDING?35:s.busy?30:s.error?34:2);
  if(s.busy){g_local_wake.idle_at=0;return;}
  /* The microphone worker already performs ASR -> AI. Never resubmit its
   * transcript: that could execute a device action twice. Speak once only. */
  if(!s.error&&!g_local_wake.speech_started&&g_local_wake.reply[0]){
    g_local_wake.speech_started=true;
    int ret=glass_local_voice_send(g_local_wake.reply,GLV_READ,&g_local_wake.request);
    if(!ret){g_local_wake.revision=UINT32_MAX;return;}
    lv_label_set_text(g_local_wake.text,glass_local_voice_error(ret));
  }
  if(!g_local_wake.idle_at)g_local_wake.idle_at=lv_tick_get();
}
static void glass_local_voice_woke(void)
{
  if(g_local_wake.root)return;
  lv_display_trigger_activity(NULL);
  struct glv_snapshot s;glass_local_voice_get(&s,NULL,0);
  if(s.busy||s.config_pending){show_toast("本地语音正在处理，请等待或停止。");return;}
  enum glv_mode mode=GLV_DIALOGUE;
#ifdef GLASS_LOCAL_CONTROL
  if(g_local_ui&&g_local_ui->mode==GLV_CONTROL)mode=GLV_CONTROL;
#endif
  uint32_t request;int ret;
#ifdef GLASS_LOCAL_CONTROL
  ret=mode==GLV_CONTROL?glass_local_voice_listen_control(&request):glass_local_voice_listen(&request);
#else
  ret=glass_local_voice_listen(&request);
#endif
  if(ret){show_toast(glass_local_voice_error(ret));return;}
  g_local_wake.request=request;g_local_wake.mode=mode;g_local_wake.revision=UINT32_MAX;
  lv_obj_t *root=lv_obj_create(lv_layer_top());g_local_wake.root=root;
  lv_obj_remove_style_all(root);lv_obj_set_size(root,LV_PCT(100),LV_PCT(100));
  lv_obj_remove_flag(root,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(root,LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(root,local_wake_outside,LV_EVENT_CLICKED,NULL);
  lv_obj_t *box=glass_box(root,88,466,364,110,detail_paper(),18);g_local_wake.dialog=box;
  lv_obj_add_flag(box,LV_OBJ_FLAG_CLICKABLE);lv_obj_remove_flag(box,LV_OBJ_FLAG_EVENT_BUBBLE|LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *content=lv_obj_create(box);lv_obj_remove_style_all(content);
  lv_obj_set_pos(content,16,14);lv_obj_set_size(content,332,82);
  lv_obj_set_scroll_dir(content,LV_DIR_VER);lv_obj_set_scrollbar_mode(content,LV_SCROLLBAR_MODE_OFF);
  g_local_wake.text=detail_text(content,"我在，请说话。",0,0,332,18,detail_ink());
  lv_label_set_long_mode(g_local_wake.text,LV_LABEL_LONG_WRAP);
  g_local_wake.bird=glass_grok_create_wake_sprite(root,8,504,zh_font);
  glass_grok_set_reduced_motion(g_local_wake.bird,g_glass.reduced_motion);
  if(g_local_wake.bird){lv_obj_t *bird=glass_grok_root(g_local_wake.bird);lv_obj_add_flag(bird,LV_OBJ_FLAG_CLICKABLE);lv_obj_add_event_cb(bird,local_wake_outside,LV_EVENT_CLICKED,NULL);}
  g_local_wake.timer=lv_timer_create(local_wake_poll,150,NULL);local_wake_poll(NULL);
}
