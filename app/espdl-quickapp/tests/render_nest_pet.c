#define GLASS_LOCAL_SPEECH 1
#define main all_ui_reference_main
#include "render_all_ui.c"
#undef main
#include "glass_seagull.h"
extern void review_chat_dialog_fixture(bool);
extern unsigned review_chat_cancel_count(void);
static void frames(unsigned n)
{for(unsigned i=0;i<n;i++){lv_tick_inc(50);lv_timer_handler();glass_grok_test_step_visible();usleep(3000);}}
static lv_point_t pointer_point;
static lv_indev_state_t pointer_state;
static void pointer_read(lv_indev_t *input,lv_indev_data_t *data)
{(void)input;data->point=pointer_point;data->state=pointer_state;}
static lv_indev_t *pointer_input;
static unsigned video_clicks;
static void video_clicked(lv_event_t *event){(void)event;video_clicks++;}
static void pointer_tap_at(int x,int y)
{
  pointer_point=(lv_point_t){x,y};pointer_state=LV_INDEV_STATE_PRESSED;
  lv_indev_read(pointer_input);lv_tick_inc(20);
  pointer_state=LV_INDEV_STATE_RELEASED;lv_indev_read(pointer_input);frames(2);
}
static void check_mini(void)
{
  pointer_input=lv_indev_create();lv_indev_set_type(pointer_input,LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(pointer_input,pointer_read);
  review_chat_dialog_fixture(true);
  wake_mini_present(90,lv_tick_get());assert(glass_grok_ready(g_wake_mini.ball));frames(3);
  assert(!lv_obj_has_flag(g_wake_mini.dismiss,LV_OBJ_FLAG_HIDDEN));
  pointer_tap_at(1000,10);assert(!g_wake_mini.visible);
  lv_obj_t *video=glass_box(g_desktop.screen,0,0,1024,568,0,0);
  local_video_show(video,g_desktop.font16);fullscreen_clicked(NULL);
  assert(local_video_fullscreen_active());
  lv_obj_add_event_cb(fullscreen,video_clicked,LV_EVENT_CLICKED,NULL);
  wake_mini_present(1,lv_tick_get());frames(20);
  assert(glass_grok_ready(g_wake_mini.ball));
  lv_area_t bubble;lv_obj_get_coords(g_wake_mini.bubble,&bubble);
  pointer_tap_at(bubble.x1+20,bubble.y1+20);assert(g_wake_mini.visible);
  lv_area_t bird;lv_obj_get_coords(glass_grok_root(g_wake_mini.ball),&bird);
  bool found=false;
  for(int y=bird.y1;y<=bird.y2&&!found;y++)for(int x=bird.x1;x<=bird.x2;x++){
    if(glass_grok_hit_test(g_wake_mini.ball,x,y)){
      pointer_tap_at(x,y);assert(g_wake_mini.visible);found=true;break;
    }
  }
  assert(found);review_capture(lv_display_get_default(),"wake-seagull","全局唤醒小海鸥");
  assert(!glass_voice_start_request(&g_wake_mini.voice_request));g_wake_mini.voice_owned=true;
  g_wake_mini.result_pending=true;strcpy(g_wake_mini.pending_prompt,"must not send after dismiss");
  assert(!glass_chat_send_bot("fixture",AGENT_MODE_ASSISTANT));
  struct chat_activity_snapshot active;glass_chat_activity_get(&active);g_wake_mini.turn=active.turn_id;
  unsigned cancels=review_chat_cancel_count();pointer_tap_at(1000,10);
  assert(review_chat_cancel_count()==cancels+1&&!video_clicks);
  struct glass_voice_snapshot voice;glass_voice_get(&voice);
  assert(!voice.active&&!g_wake_mini.visible&&!g_wake_mini.voice_owned);
  assert(!g_wake_mini.pending_prompt[0]&&!g_wake_mini.result_pending&&!g_wake_mini.turn);
  assert(lv_obj_has_flag(g_wake_mini.dismiss,LV_OBJ_FLAG_HIDDEN));
  assert(local_video_fullscreen_active());
  /* A stale popup cannot cancel a newer microphone owner's request. */
  wake_mini_present(2,lv_tick_get());frames(2);
  uint32_t newer;assert(!glass_voice_start_request(&newer));
  g_wake_mini.voice_owned=true;g_wake_mini.voice_request=newer-1;
  wake_mini_dismiss_at(1000,10);glass_voice_get(&voice);
  assert(voice.active&&voice.request==newer);glass_voice_cancel();
  assert(local_video_fullscreen_active());fullscreen_exit(NULL);lv_obj_delete(video);
  puts("WAKE_OUTSIDE_OK: every page dismissible; fullscreen video retained; bird/bubble protected; outside cancels own capture, retires result/mailbox; newer owner preserved");
}
static void check_local_wake(void)
{
  extern void review_local_fixture(int,const char *);
  extern void review_local_asr_fixture(bool,unsigned,const char *);
  extern unsigned review_local_stops(void);
  lv_obj_t *underlying=g_desktop.panel;
  bool hidden=lv_obj_has_flag(underlying,LV_OBJ_FLAG_HIDDEN);
  enum glv_phase phases[]={GLV_RECORDING,GLV_GENERATING,GLV_SPEAKING};
  for(unsigned i=0;i<3;i++){
    review_local_fixture(GLV_IDLE,"");review_local_asr_fixture(true,0,"");
    glass_local_voice_woke();assert(glass_grok_ready(g_local_wake.bird));frames(20);assert(g_local_wake.root);
    assert(g_desktop.panel==underlying&&lv_obj_has_flag(underlying,LV_OBJ_FLAG_HIDDEN)==hidden);
    lv_area_t dialog;lv_obj_get_coords(g_local_wake.dialog,&dialog);
    pointer_tap_at(dialog.x1+20,dialog.y1+20);assert(g_local_wake.root);
    lv_area_t bird;lv_obj_get_coords(glass_grok_root(g_local_wake.bird),&bird);
    bool found=false;
    for(int y=bird.y1;y<=bird.y2&&!found;y++)for(int x=bird.x1;x<=bird.x2;x++){
      if(glass_grok_hit_test(g_local_wake.bird,x,y)){
        pointer_tap_at(x,y);assert(g_local_wake.root);found=true;break;
      }
    }
    assert(found);review_local_fixture(phases[i],"");frames(4);
    if(!i)review_capture(lv_display_get_default(),"local-wake-seagull","本地唤醒小海鸥浮层");
    unsigned stops=review_local_stops();pointer_tap_at(900,200);
    assert(!g_local_wake.root&&review_local_stops()==stops+1);
    assert(g_desktop.panel==underlying&&lv_obj_has_flag(underlying,LV_OBJ_FLAG_HIDDEN)==hidden);
    assert(glass_local_voice_wake_enabled());
  }
  review_local_fixture(GLV_IDLE,"");glass_local_voice_woke();frames(2);
  review_local_fixture(GLV_IDLE,"");uint32_t newer;assert(!glass_local_voice_listen(&newer));
  unsigned stops=review_local_stops();local_wake_poll(NULL);frames(2);
  assert(!g_local_wake.root&&review_local_stops()==stops);
  review_local_fixture(GLV_IDLE,"");glass_local_voice_woke();frames(2);
  extern unsigned review_local_send_count(void);
  extern enum glv_mode review_local_sent_mode(void);
  unsigned sends=review_local_send_count();
  /* The same microphone request runs ASR and AI. UI must not replay it. */
  review_local_asr_fixture(true,10,"今天天气如何");
  review_local_fixture(GLV_GENERATING,"正在生成回复");frames(4);
  assert(review_local_send_count()==sends);
  review_local_fixture(GLV_IDLE,"今天适合出去走走。记得带好随身物品，祝你有一个轻松愉快的下午。");frames(4);
  assert(g_local_wake.speech_started&&review_local_send_count()==sends+1);
  assert(review_local_sent_mode()==GLV_READ);
  assert(lv_obj_get_child_count(g_local_wake.dialog)==1);
  review_local_fixture(GLV_SPEAKING,"今天适合出去走走。记得带好随身物品，祝你有一个轻松愉快的下午。");frames(4);
  review_capture(lv_display_get_default(),"local-wake-reply","无按钮轻量唤醒气泡");
  review_local_fixture(GLV_IDLE,"今天适合出去走走。记得带好随身物品，祝你有一个轻松愉快的下午。");frames(4);
  assert(g_local_wake.root&&review_local_send_count()==sends+1);
  frames(125);assert(!g_local_wake.root&&review_local_send_count()==sends+1);
  review_local_fixture(GLV_IDLE,"");glass_local_voice_woke();frames(2);
  pointer_tap_at(900,200);assert(!g_local_wake.root);
  review_local_fixture(GLV_IDLE,"晚到的结果");frames(10);
  assert(!g_local_wake.root&&review_local_send_count()==sends+1);
  lv_indev_delete(pointer_input);
  puts("BUTTONLESS_WAKE_OK: no buttons; no duplicate AI submission; auto TTS exactly once; auto hide; late results cannot replay");
  puts("LOCAL_WAKE_OK: underlying page preserved; actual pointer protects dialog/bird; outside stops ASR/generation/TTS; newer request and global wake preserved");
}
#ifdef GLASS_REVIEW_REAL_PET
static void check_bird_actions(lv_display_t *display)
{
  static const char *names[]={"idle","tilt","shake","stretch","peck","hop","flap","doze","fly","roll-forward","roll-backward"};
  lv_obj_t *stage=glass_box(g_desktop.screen,0,0,1024,568,0,0);
  lv_obj_set_style_bg_color(stage,lv_color_hex(g_desktop.light_theme?0xf4f3f7:0x1f2429),0);
  lv_obj_set_style_bg_opa(stage,LV_OPA_COVER,0);
  for(unsigned i=0;i<3;i++)pet_engine_tick(1000); /* Expire old speech before preview. */
  pet_engine_set_home(430,340);pet_engine_set_visible(true);
  pet_engine_set_mode(PET_STILL);
  assert(pet_lvgl_create(stage,g_desktop.font16)==0);
  size_t bytes=pet_lvgl_sprite_bytes();assert(bytes==SEAGULL_BYTES);
  for(unsigned motion=1;motion<SEAGULL_MOTION_COUNT;motion++){
    pet_engine_motion(motion);
    struct pet_state_s pet;pet_engine_get(&pet);assert(pet.motion==motion);
    int x=pet.x,y=pet.y;
    for(unsigned frame=0;frame<48;frame++){
      frames(1);pet_engine_get(&pet);assert(pet.x==x&&pet.y==y);
      {char key[64];snprintf(key,sizeof(key),"bird-%s-%02u",names[motion],frame);
        review_capture(display,key,names[motion]);}
      assert(pet_lvgl_sprite_bytes()==bytes);
    }
    pet_engine_get(&pet);assert(!pet.motion);
    struct seagull_pose pose=seagull_motion_pose(motion,SEAGULL_MOTION_MS);
    assert(!pose.angle&&!pose.x&&!pose.y&&pose.sx==256&&pose.sy==256&&pose.emotion==2);
  }
  pet_engine_motion(SEAGULL_DOZE);pet_engine_drag_begin(430,340);
  pet_engine_drag_move(500,350);struct pet_state_s pet;pet_engine_get(&pet);
  assert(pet.dragging&&!pet.motion);pet_engine_motion(SEAGULL_HOP);
  pet_engine_get(&pet);assert(!pet.motion);pet_engine_drag_end();
  pet_engine_get(&pet);assert(!pet.dragging&&pet.motion==SEAGULL_SHAKE);
  pet_engine_feed("fries");pet_engine_get(&pet);assert(pet.motion==SEAGULL_PECK);
  pet_engine_set_follow(700,400);pet_engine_get(&pet);assert(!pet.motion);
  pet_engine_set_visible(false);pet_engine_motion(SEAGULL_FLAP);
  pet_engine_get(&pet);assert(!pet.visible&&!pet.motion);
  pet_lvgl_destroy();lv_obj_delete(stage);frames(3);
  puts("BIRD_ACTIONS_OK: ten bounded gestures, real LVGL renderer, stable memory, drag/follow priority, feed and hide lifecycle");
}
#endif

static void check_wake_return(void)
{
  extern void review_local_fixture(int,const char *);
  extern void review_local_asr_fixture(bool,unsigned,const char *);
  pointer_input=lv_indev_create();lv_indev_set_type(pointer_input,LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(pointer_input,pointer_read);
  for(unsigned theme=0;theme<2;theme++)for(unsigned page=0;page<6;page++){
    panel_hide(NULL);g_desktop.light_theme=!theme;
    if(page==1)glass_settings(NULL);
    if(page==2)glass_storage_page(NULL);
    if(page==3)glass_chat_open(NULL);
    if(page==4)glass_music(NULL);
    if(page==5)glass_lock_open(NULL);
    frames(3);
    lv_obj_t *card=g_desktop.current_card,*lock=g_lock_ui.root;
    bool hidden=lv_obj_has_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN);
    wake_mini_present(100+page,lv_tick_get());frames(3);
    assert(g_wake_mini.visible);
    /* Real left-edge pointer swipe must close the overlay, even on Home/lock. */
    pointer_point=(lv_point_t){8,220};pointer_state=LV_INDEV_STATE_PRESSED;
    lv_indev_read(pointer_input);lv_tick_inc(20);
    pointer_point.x=100;lv_indev_read(pointer_input);lv_tick_inc(20);
    pointer_state=LV_INDEV_STATE_RELEASED;lv_indev_read(pointer_input);frames(3);
    assert(!g_wake_mini.visible);
    assert(g_desktop.current_card==card&&g_lock_ui.root==lock);
    assert(lv_obj_has_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN)==hidden);
    /* A release queued before an idle timeout must never go back a second time. */
    wake_mini_present(200+page,lv_tick_get());g_edge_wake=true;
    g_edge_page=card;wake_mini_hide();glass_edge_back_async(NULL);
    assert(g_desktop.current_card==card&&g_lock_ui.root==lock);
    /* App-local popup obeys the same return priority. */
    review_local_fixture(GLV_IDLE,"");review_local_asr_fixture(true,0,"");
    glass_local_voice_woke();assert(g_local_wake.root);
    g_edge_page=card;glass_edge_back_async(NULL);frames(3);
    assert(!g_local_wake.root&&g_desktop.current_card==card&&g_lock_ui.root==lock);
    if(lock)lock_unlock(NULL);
  }
  panel_hide(NULL);
  review_chat_dialog_fixture(true);glass_chat_ball_open(NULL);frames(3);
  lv_obj_t *ball_page=g_desktop.current_card;
  assert(chat_ball_accept_wake()&&g_chat_ui.voice_session&&g_chat_ui.voice_from_wake);
  g_edge_page=ball_page;glass_edge_back_async(NULL);frames(3);
  assert(g_desktop.current_card==ball_page&&!g_chat_ui.voice_session);
  assert(chat_ball_accept_wake());glass_chat_back(NULL);
  assert(g_desktop.current_card==ball_page&&!g_chat_ui.voice_session);
  panel_hide(NULL);lv_indev_delete(pointer_input);
  puts("WAKE_RETURN_OK: Home/settings/files/chat/music/lock; both themes; actual edge pointer, queued timeout and local popup; original page identities preserved");
}

int main(int argc,char **argv)
{
  assert(argc==3);review_out=argv[2];
  FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);size_t size=ftell(f);rewind(f);
  void *font=malloc(size);assert(font&&fread(font,1,size,f)==size);fclose(f);
  lv_init();lv_display_t *display=lv_display_create(1024,600);
  lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(display,review_fb,NULL,sizeof(review_fb),LV_DISPLAY_RENDER_MODE_FULL);
  lv_display_set_flush_cb(display,review_flush);
  g_desktop.font16=lv_tiny_ttf_create_data(font,size,16);
  g_desktop.font20=lv_tiny_ttf_create_data(font,size,20);
  g_desktop.font28=lv_tiny_ttf_create_data(font,size,28);
  g_desktop.font16->fallback=glass_icon_font(16);g_desktop.font20->fallback=glass_icon_font(20);
  g_desktop.font28->fallback=glass_icon_font(24);
  g_glass.font48=lv_tiny_ttf_create_data(font,size,48);glass_home_fonts_init(font,size);
  g_glass.reduced_motion=true;g_glass.temperature_c=-999;g_glass.humidity_pct=-1;g_glass.pm25=-1;
  g_desktop.screen=glass_box(lv_screen_active(),0,32,1024,568,0,0);
  g_desktop.panel=glass_box(g_desktop.screen,0,0,1024,568,0,0);
  lv_obj_add_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN);glass_status_create();
  pet_engine_init(1024,568);pet_engine_set_visible(false);
  char path[1024];snprintf(path,sizeof(path),"%s/screens.json",review_out);
  review_manifest=fopen(path,"w");assert(review_manifest);fputs("[\n",review_manifest);
  for(unsigned theme=0;theme<2;theme++){
    g_desktop.light_theme=theme==0;glass_rebuild();frames(20);
    lv_obj_t *root=lv_obj_get_child(g_glass.home_cards[1],0);
    lv_obj_t *image=lv_obj_get_child(root,0);assert(lv_obj_check_type(image,&lv_image_class));
    assert(!lv_obj_has_flag(image,LV_OBJ_FLAG_HIDDEN));
    review_capture(display,"nest-home","海鸥小窝");
    bool moved=false;
    for(unsigned frame=0;frame<270;frame++){
      frames(1);
      if(lv_image_get_rotation(image)!=0){
        if(!moved)review_capture(display,"nest-tilt","窝内歪头");
        moved=true;
      }
    }
    assert(moved&&lv_image_get_rotation(image)==0);
    assert(lv_image_get_scale_x(image)==144&&lv_image_get_scale_y(image)==144);
    lv_obj_t *toggle=lv_obj_get_child(root,-2); /* Last child is loading notice. */
    assert(!lv_obj_check_type(toggle,&lv_button_class));
    assert(lv_obj_get_child_count(toggle)==0);
    for(unsigned i=0;i<20;i++){
      struct pet_state_s pet;
      unsigned taps=0;
      do {lv_obj_send_event(toggle,LV_EVENT_CLICKED,NULL);frames(2);pet_engine_get(&pet);taps++;} while(!pet.visible&&taps<5);
      assert(pet.visible&&!pet.dragging&&taps<=5);
      for(unsigned rest=taps;rest<5;rest++){
        lv_obj_send_event(toggle,LV_EVENT_CLICKED,NULL);frames(2);
        pet_engine_get(&pet);assert(pet.visible);
      }
      assert(pet.x>=100&&pet.x<220&&pet.y>=300&&pet.y<420);
      assert(lv_obj_has_flag(image,LV_OBJ_FLAG_HIDDEN));
      if(i==0)review_capture(display,"nest-away","桌宠出窝后");
      /* Existing desktop pet show/hide controls share the same state. */
      frames(61);
      if(i%2)pet_engine_set_visible(false);
      else lv_obj_send_event(toggle,LV_EVENT_CLICKED,NULL);
      frames(2);pet_engine_get(&pet);assert(!pet.visible);
      assert(!lv_obj_has_flag(image,LV_OBJ_FLAG_HIDDEN));
    }
#ifdef GLASS_REVIEW_REAL_PET
    check_bird_actions(display);
#endif
  }
  /* Local inference may evict the LVGL image cache before a wake popup draws. */
  lv_image_cache_resize(0,true);
  lock_display_font=lv_tiny_ttf_create_data(font,size,192);
  g_glass.font72=lv_tiny_ttf_create_data(font,size,128);
  check_wake_return();
  check_mini();
  check_local_wake();
  glass_chat_ball_open(NULL);frames(20);assert(g_chat_ui.ball_mode);
  lv_obj_send_event(g_chat_ui.root,LV_EVENT_CLICKED,NULL);frames(2);
  assert(g_chat_ui.root&&g_chat_ui.ball_mode);panel_hide(NULL);
  puts("ORDINARY_AI_PAGE_UNCHANGED_OK");
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("NEST_PET_OK: actual production LVGL/real pet state; two themes, 40 depart/return cycles, alpha hit test and wake cancellation ownership");
  return 0;
}
