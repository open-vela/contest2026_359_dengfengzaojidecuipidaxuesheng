/* SPDX-License-Identifier: Apache-2.0 */
/* UI-thread global wake presentation. No pointer/tap path can summon it. */
#define WAKE_MINI_IDLE_MS 6000u
static struct {
  lv_obj_t *root,*bubble,*text,*caption;
  struct glass_grok *ball;
  lv_obj_t *result_root;
  uint32_t event,shown_at,idle_at,retry_at,request,heard_at,listening_at;
  uint32_t voice_request,voice_revision,command_event;
  uint32_t turn,result_signature,speech_request,speech_retry_at,speech_wait_at;
  size_t speech_cursor;
  bool result_pending,speech_owned;
  bool visible,voice_owned,heard,busy;
  char pending_prompt[CHAT_PROMPT_BYTES+1];
} g_wake_mini;
#if LV_USE_SNAPSHOT
static lv_draw_buf_t *wake_mini_frame;
static uint32_t wake_mini_frame_at;
static void wake_mini_camera(void)
{
  if(!g_wake_mini.visible||!qpk_runtime_camera_active()){
    if(wake_mini_frame){qpk_runtime_wake_overlay(NULL,0,0,0,0,0);lv_draw_buf_destroy(wake_mini_frame);wake_mini_frame=NULL;}
    return;
  }
  if(wake_mini_frame&&lv_tick_get()-wake_mini_frame_at<100)return;
  wake_mini_frame_at=lv_tick_get();
  glass_grok_refresh(g_wake_mini.ball);
  if(wake_mini_frame&&(wake_mini_frame->header.w!=(unsigned)lv_obj_get_width(g_wake_mini.root)||
     wake_mini_frame->header.h!=(unsigned)lv_obj_get_height(g_wake_mini.root))){lv_draw_buf_destroy(wake_mini_frame);wake_mini_frame=NULL;}
  if(!wake_mini_frame)wake_mini_frame=lv_snapshot_create_draw_buf(g_wake_mini.root,LV_COLOR_FORMAT_ARGB8888);
  if(wake_mini_frame&&lv_snapshot_take_to_draw_buf(g_wake_mini.root,LV_COLOR_FORMAT_ARGB8888,wake_mini_frame)==LV_RESULT_OK){
    lv_area_t area;lv_obj_get_coords(g_wake_mini.root,&area);
    qpk_runtime_wake_overlay(wake_mini_frame->data,wake_mini_frame->header.w,wake_mini_frame->header.h,wake_mini_frame->header.stride,area.x1,area.y1);
  }else qpk_runtime_wake_overlay(NULL,0,0,0,0,0);
}
#else
static void wake_mini_camera(void){}
#endif

static void wake_mini_pass_through(lv_obj_t *object)
{
  lv_obj_remove_flag(object,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
  for(unsigned i=0;i<lv_obj_get_child_count(object);i++)
    wake_mini_pass_through(lv_obj_get_child(object,i));
}
static void wake_mini_create(void)
{
  if(g_wake_mini.root)return;
  lv_obj_t *root=lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(root);lv_obj_set_size(root,388,112);
  lv_obj_add_flag(root,LV_OBJ_FLAG_HIDDEN);
  g_wake_mini.root=root;
  /* Same sprite constructor, app expression bank and front-facing gaze as
   * the large conversation ball, scaled to a black 48px body. */
  g_wake_mini.ball=glass_grok_create_sprite(root,0,44,64,64,48,true,zh_font);
  if(g_wake_mini.ball){
    lv_obj_set_pos(glass_grok_root(g_wake_mini.ball),0,44);
    glass_grok_gaze(g_wake_mini.ball,0,0);
    glass_grok_select(g_wake_mini.ball,2);
    glass_grok_set_active(g_wake_mini.ball,false);
  }
  lv_obj_t *bubble=lv_obj_create(root);g_wake_mini.bubble=bubble;
  lv_obj_set_pos(bubble,72,4);lv_obj_set_size(bubble,312,96);
  lv_obj_set_style_radius(bubble,20,0);lv_obj_set_style_pad_all(bubble,14,0);
  lv_obj_set_style_border_width(bubble,1,0);
  g_wake_mini.caption=lv_label_create(bubble);
  lv_obj_remove_style_all(g_wake_mini.caption);
  lv_obj_set_style_text_font(g_wake_mini.caption,zh_font(12),0);
  lv_label_set_text(g_wake_mini.caption,glass_tr("语音唤醒 · 空闲 6 秒后收起"));
  g_wake_mini.text=lv_label_create(bubble);
  lv_obj_remove_style_all(g_wake_mini.text);
  lv_obj_set_pos(g_wake_mini.text,0,24);lv_obj_set_width(g_wake_mini.text,280);
  lv_obj_set_style_text_font(g_wake_mini.text,zh_font(16),0);
  lv_label_set_long_mode(g_wake_mini.text,LV_LABEL_LONG_DOT);
  lv_obj_set_height(g_wake_mini.text,42);
  wake_mini_pass_through(root);
}
static void wake_mini_hide(void)
{
  if(!g_wake_mini.visible)return;
  g_wake_mini.visible=false;
  lv_obj_add_flag(g_wake_mini.root,LV_OBJ_FLAG_HIDDEN);
  glass_grok_set_active(g_wake_mini.ball,false);
  printf("[wake-mini] hidden age_ms=%lu idle_ms=%lu\n",(unsigned long)(lv_tick_get()-g_wake_mini.shown_at),(unsigned long)(lv_tick_get()-g_wake_mini.idle_at));
}
static void wake_mini_present(uint32_t event,uint32_t now)
{
  /* Called exclusively after consuming a fresh microphone wake event. */
  wake_mini_create();g_wake_mini.event=event;g_wake_mini.shown_at=g_wake_mini.idle_at=now;
  if(!g_wake_mini.result_pending&&!g_wake_mini.speech_owned){
    g_wake_mini.turn=g_wake_mini.result_signature=0;
    if(g_wake_mini.result_root){lv_obj_delete(g_wake_mini.result_root);g_wake_mini.result_root=NULL;}
    lv_obj_remove_flag(g_wake_mini.bubble,LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(g_wake_mini.root,112);lv_obj_set_pos(glass_grok_root(g_wake_mini.ball),0,44);
  }
  g_wake_mini.busy=false;
  g_wake_mini.visible=true;
  bool light=g_desktop.light_theme;
  lv_obj_set_style_bg_color(g_wake_mini.bubble,lv_color_hex(light?0xffffff:0x242b38),0);
  lv_obj_set_style_bg_opa(g_wake_mini.bubble,LV_OPA_COVER,0);
  lv_obj_set_style_border_color(g_wake_mini.bubble,lv_color_hex(light?0xdce5f0:0x43516a),0);
  lv_obj_set_style_text_color(g_wake_mini.text,lv_color_hex(light?0x202b3d:0xf1f5fb),0);
  lv_obj_set_style_text_color(g_wake_mini.caption,lv_color_hex(light?0x526985:0xaebed4),0);
  lv_label_set_text(g_wake_mini.text,glass_tr("我在，请说。 说完后自动发送"));
  lv_obj_align(g_wake_mini.root,LV_ALIGN_BOTTOM_LEFT,16,-16);
  lv_obj_set_style_opa(g_wake_mini.root,LV_OPA_COVER,0);
  lv_obj_remove_flag(g_wake_mini.root,LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(g_wake_mini.root);
  lv_obj_invalidate(g_wake_mini.root);
  glass_grok_set_reduced_motion(g_wake_mini.ball,g_glass.reduced_motion);
  glass_grok_set_active(g_wake_mini.ball,true);
  glass_grok_select(g_wake_mini.ball,2);
  printf("[wake-mini] shown event=%lu idle_timeout_ms=6000\n",(unsigned long)event);
}

#ifdef GLASS_LOCAL_SPEECH
/* Keep the UI independent of the audio driver's buffer structures. */
bool music_pcm_session_active(void);
static uint32_t wake_mini_clock(void)
{struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);return (uint32_t)((uint64_t)now.tv_sec*1000+now.tv_nsec/1000000);}
static void wake_mini_result_service(const struct speech_snapshot *speech,const struct glass_voice_snapshot *voice,uint32_t now,bool ball_page)
{
  /* A retired, completed result must not scan/copy chat history on every
   * desktop loop. Pending speech still runs even on the large-ball page. */
  if(!g_wake_mini.turn||(!g_wake_mini.visible&&!g_wake_mini.result_pending&&!g_wake_mini.speech_owned))return;
  struct chat_result_snapshot result;
  if(!glass_chat_result_get(&result,g_wake_mini.turn,AGENT_MODE_ASSISTANT,0)){
    if(g_wake_mini.speech_owned)glass_speech_stop_request(g_wake_mini.speech_request);
    g_wake_mini.speech_owned=g_wake_mini.result_pending=false;g_wake_mini.turn=0;
    if(g_wake_mini.result_root){lv_obj_delete(g_wake_mini.result_root);g_wake_mini.result_root=NULL;}
    if(g_wake_mini.visible){
      lv_obj_remove_flag(g_wake_mini.bubble,LV_OBJ_FLAG_HIDDEN);lv_label_set_text(g_wake_mini.text,glass_tr("对话已更新"));
      lv_obj_set_height(g_wake_mini.root,112);lv_obj_set_pos(glass_grok_root(g_wake_mini.ball),0,44);g_wake_mini.idle_at=now;
    }
    return;
  }
  if(g_wake_mini.visible&&!ball_page){
    uint32_t signature=chat_result_signature(&result);
    if(signature!=g_wake_mini.result_signature){
      if(g_wake_mini.result_root)lv_obj_delete(g_wake_mini.result_root);
      g_wake_mini.result_root=chat_result_build(g_wake_mini.root,72,4,312,&result);
      wake_mini_pass_through(g_wake_mini.result_root);
      lv_obj_add_flag(g_wake_mini.bubble,LV_OBJ_FLAG_HIDDEN);
      int height=126+(int)result.card_count*50;
      lv_obj_set_height(g_wake_mini.root,height);
      lv_obj_set_pos(glass_grok_root(g_wake_mini.ball),0,height-68);
      g_wake_mini.result_signature=signature;
    }
  }
  if(!g_wake_mini.result_pending)return;
  if(result.state==CHAT_WAITING)return;
  if(g_wake_mini.speech_owned){
    if(speech->request!=g_wake_mini.speech_request||(!speech->active&&speech->error)){
      g_wake_mini.speech_owned=g_wake_mini.result_pending=false;
      show_toast(glass_tr("语音播报未完成，结果已保留"));g_wake_mini.idle_at=now;return;
    }
    if(speech->active)return;
    g_wake_mini.speech_owned=false;
  }
  if((result.state!=CHAT_DONE&&g_wake_mini.speech_cursor)||
     (result.state==CHAT_DONE&&(result.card_count?g_wake_mini.speech_cursor>=result.card_count:
        !glass_chat_result_get(&result,g_wake_mini.turn,AGENT_MODE_ASSISTANT,g_wake_mini.speech_cursor)||!result.reply[0]))){
    g_wake_mini.result_pending=false;g_wake_mini.idle_at=now;return;
  }
  if(voice->active||(speech->active&&!speech->background_wake))return;
  char text[241];size_t next;
  if(result.state==CHAT_DONE&&result.card_count){
    result.cards[0]=result.cards[g_wake_mini.speech_cursor];result.card_count=1;
    chat_result_voice(&result,text,sizeof(text));next=g_wake_mini.speech_cursor+1;
  }else{
    chat_result_voice(&result,text,sizeof(text));next=result.state==CHAT_DONE?result.next_offset:1;
  }
  if(!text[0]){g_wake_mini.result_pending=false;g_wake_mini.idle_at=now;return;}
  if((int32_t)(now-g_wake_mini.speech_retry_at)<0)return;
  int ret=glass_speech_say_request(text,&g_wake_mini.speech_request);
  if(!ret){g_wake_mini.speech_owned=true;g_wake_mini.speech_cursor=next;g_wake_mini.speech_retry_at=g_wake_mini.speech_wait_at=0;}
  else if(ret==-EBUSY){
    if(!g_wake_mini.speech_wait_at)g_wake_mini.speech_wait_at=now;
    if(now-g_wake_mini.speech_wait_at>=10000){g_wake_mini.result_pending=false;g_wake_mini.idle_at=now;show_toast(glass_tr("音频正忙，结果已保留"));}
    else g_wake_mini.speech_retry_at=now+500;
  }
  else {g_wake_mini.result_pending=false;g_wake_mini.idle_at=now;show_toast(glass_tr("语音暂不可用，结果已保留"));}
}
static void wake_mini_service(void)
{
  uint32_t now=lv_tick_get();
  struct speech_snapshot speech;glass_speech_get(&speech);
  struct glass_voice_snapshot voice;glass_voice_get(&voice);
  bool ball_page=chat_ball_is_present();
  if(ball_page){
    if(g_wake_mini.turn&&(g_wake_mini.visible||g_wake_mini.result_pending))chat_ball_show_result(g_wake_mini.turn);
    if(g_wake_mini.voice_owned&&voice.request==g_wake_mini.voice_request&&chat_ball_adopt_wake_capture())g_wake_mini.voice_owned=false;
    wake_mini_hide();
  }
  bool fresh=speech.wake_event&&speech.wake_event!=g_wake_mini.event;
  if(fresh){
    g_wake_mini.event=speech.wake_event;
    uint32_t event_age=wake_mini_clock()-speech.wake_ms;
    printf("[wake-mini] event=%lu age_ms=%lu phase=%d background=%u\n",
      (unsigned long)speech.wake_event,(unsigned long)event_age,speech.phase,speech.background_wake);
    if(event_age<=1500){
      /* A fresh local wake is a new microphone request, including a wake heard
       * in somebody else's ASR PCM. Never merely show a ball bound to that
       * other page's recording. Drop the previous mini's output ownership. */
      if(g_wake_mini.speech_owned)glass_speech_stop_request(g_wake_mini.speech_request);
      struct chat_activity_snapshot previous;glass_chat_activity_get(&previous);
      if(previous.phase!=CHAT_IDLE)glass_chat_cancel();
      g_wake_mini.pending_prompt[0]=0;
      g_wake_mini.speech_owned=g_wake_mini.result_pending=false;
      if(!ball_page)wake_mini_present(speech.wake_event,now);
      else g_wake_mini.shown_at=now;
      {
        bool page_owned=chat_ball_accept_wake();
        /* A large-ball page always keeps presentation there, even if AI is
         * unavailable; it must never fall back to a hidden mini capture. */
        int ret=page_owned?0:ball_page?-EBUSY:glass_voice_start_request(&g_wake_mini.voice_request);
        g_wake_mini.voice_owned=!page_owned&&!ret;g_wake_mini.heard=false;g_wake_mini.listening_at=0;
        glass_voice_get(&voice);
        printf("[wake-mini] capture event=%lu request=%lu owner=%s result=%d\n",
          (unsigned long)speech.wake_event,(unsigned long)voice.request,page_owned?"large":g_wake_mini.voice_owned?"mini":"none",ret);
        if(ret&&g_wake_mini.visible)lv_label_set_text(g_wake_mini.text,glass_tr("我在。请先配置语音识别服务"));
      }
      if(g_wake_mini.visible){
      if(speech.active&&!speech.background_wake&&speech.phase==SPEECH_COMMAND)
        lv_label_set_text(g_wake_mini.text,glass_tr("我在，请说离线指令。"));
      else if(voice.active&&!g_wake_mini.voice_owned)
        lv_label_set_text(g_wake_mini.text,glass_tr("我在，语音会话正在聆听。"));
      else if(g_chat_ui.voice_session&&!voice.active)
        lv_label_set_text(g_wake_mini.text,glass_tr("我在，正在处理你的请求。"));
      }
    }
  }
  if(g_wake_mini.voice_owned&&voice.request!=g_wake_mini.voice_request){
    g_wake_mini.voice_owned=false;g_wake_mini.idle_at=now;
    if(g_wake_mini.visible)lv_label_set_text(g_wake_mini.text,glass_tr("麦克风已交给新的语音请求"));
  }
  if(g_wake_mini.voice_owned){
    if(voice.active&&voice.phase==GLASS_VOICE_LISTENING){
      if(!g_wake_mini.listening_at)g_wake_mini.listening_at=now;
      if(voice.level>650||(voice.text[0]&&voice.revision!=g_wake_mini.voice_revision)){
        g_wake_mini.heard=true;g_wake_mini.heard_at=now;
      }
      if(g_wake_mini.visible&&voice.text[0]&&strcmp(lv_label_get_text(g_wake_mini.text),voice.text))
        lv_label_set_text(g_wake_mini.text,voice.text);
      if(g_wake_mini.heard&&now-g_wake_mini.heard_at>=2000){
        glass_voice_stop_request(g_wake_mini.voice_request);
        if(g_wake_mini.visible)lv_label_set_text(g_wake_mini.text,glass_tr("正在识别…"));
      }
      /* Bound an empty capture from actual listening readiness, rather than
       * cutting a spoken command at six seconds after the wake event. */
      else if(!g_wake_mini.heard&&now-g_wake_mini.listening_at>=WAKE_MINI_IDLE_MS)glass_voice_stop_request(g_wake_mini.voice_request);
    }else if(!voice.active){
      g_wake_mini.voice_owned=false;
      if(voice.phase==GLASS_VOICE_DONE&&voice.text[0]){
        /* A wake can finish ASR before the old HTTP/tool cancellation drains.
         * Retain this new request until it can be accepted once. */
        glass_chat_copy_utf8(g_wake_mini.pending_prompt,sizeof(g_wake_mini.pending_prompt),voice.text);
        if(g_wake_mini.visible)lv_label_set_text(g_wake_mini.text,voice.text);
      }else if(g_wake_mini.visible)lv_label_set_text(g_wake_mini.text,voice.phase==GLASS_VOICE_ERROR?
        glass_voice_error_text(voice.error):glass_tr("没有听清，请再说一次唤醒词"));
      g_wake_mini.retry_at=now+1500;
    }
  }
  if(g_wake_mini.pending_prompt[0]){
    struct chat_activity_snapshot activity;glass_chat_activity_get(&activity);
    if(activity.phase==CHAT_IDLE){
      int ret=glass_chat_send_bot(g_wake_mini.pending_prompt,AGENT_MODE_ASSISTANT);
      if(!ret){
        struct chat_result_snapshot result;
        if(glass_chat_result_get(&result,0,AGENT_MODE_ASSISTANT,0)){
          g_wake_mini.turn=result.turn_id;g_wake_mini.result_pending=true;g_wake_mini.speech_cursor=0;
          g_wake_mini.result_signature=0;g_wake_mini.speech_retry_at=g_wake_mini.speech_wait_at=0;
        }
      }
      if(ret!=-EBUSY){g_wake_mini.pending_prompt[0]=0;
        if(ret&&g_wake_mini.visible)lv_label_set_text(g_wake_mini.text,glass_tr("AI 暂不可用，请检查配置"));}
    }
  }
  wake_mini_result_service(&speech,&voice,now,ball_page);
  if(g_wake_mini.visible){
    struct chat_activity_snapshot activity;glass_chat_activity_get(&activity);
    glass_grok_select(g_wake_mini.ball,glass_grok_emotion(&activity,&voice));
    bool speech_input=voice.active&&voice.phase==GLASS_VOICE_LISTENING&&
      (voice.level>650||(voice.text[0]&&voice.revision!=g_wake_mini.voice_revision));
    bool was_busy=g_wake_mini.busy;
    g_wake_mini.busy=g_wake_mini.pending_prompt[0]||g_wake_mini.result_pending||activity.phase!=CHAT_IDLE||speech_input||
      (voice.active&&voice.phase!=GLASS_VOICE_LISTENING)||
      (speech.active&&!speech.background_wake&&speech.phase!=SPEECH_WAKE)||
      speech_ui_command_pending();
    bool command=speech.command_event&&speech.command_event!=g_wake_mini.command_event&&
      wake_mini_clock()-speech.command_ms<=2500;
    bool transcript=voice.text[0]&&voice.revision!=g_wake_mini.voice_revision;
    g_wake_mini.voice_revision=voice.revision;g_wake_mini.command_event=speech.command_event;
    if(g_wake_mini.busy||was_busy||command||transcript)g_wake_mini.idle_at=now;
    uint32_t age=now-g_wake_mini.shown_at,idle=now-g_wake_mini.idle_at;
    if(idle>=WAKE_MINI_IDLE_MS){
      if(g_wake_mini.voice_owned&&voice.active)glass_voice_stop_request(g_wake_mini.voice_request);
      wake_mini_hide();
    }else{
      int offset=g_glass.reduced_motion?0:age<180?(180-age)*48/180:
        idle>5820?(idle-5820)*48/180:0;
      if(lv_obj_get_style_x(g_wake_mini.root,0)!=16-offset||
         lv_obj_get_style_y(g_wake_mini.root,0)!=-16+offset||
         lv_obj_get_style_align(g_wake_mini.root,0)!=LV_ALIGN_BOTTOM_LEFT)
        lv_obj_align(g_wake_mini.root,LV_ALIGN_BOTTOM_LEFT,16-offset,-16+offset);
      unsigned opacity=g_glass.reduced_motion?255:
        age<180?age*255/180:idle>5820?(6000-idle)*255/180:255;
      if(lv_obj_get_style_opa(g_wake_mini.root,0)!=opacity)
        lv_obj_set_style_opa(g_wake_mini.root,opacity,0);
      lv_obj_t *parent=lv_obj_get_parent(g_wake_mini.root);
      if(lv_obj_get_child(parent,-1)!=g_wake_mini.root)lv_obj_move_foreground(g_wake_mini.root);
    }
  }
  wake_mini_camera();
  /* Page presence never owns the microphone. An ASR capture observes wake
   * words in its own PCM stream; idle pages use this background capture.
   * Leave completed foreground TTS results for their owner to consume. */
  if(g_wake_mini.speech_owned||g_wake_mini.voice_owned||voice.active||speech.active||
     (g_chat_ui.voice_session&&g_chat_ui.speech_owned)||music_pcm_session_active()||
     (int32_t)(now-g_wake_mini.retry_at)<0)return;
  int ret=glass_speech_background_wake(&g_wake_mini.request);
  g_wake_mini.retry_at=now+(ret?10000:1500);
}
#else
static void wake_mini_service(void){}
#endif
