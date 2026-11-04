#define GLASS_LOCAL_SPEECH 1
#define main all_ui_reference_main
#include "render_all_ui.c"
#undef main
#include "glass_seagull.h"
extern void review_chat_dialog_fixture(bool);
extern unsigned review_chat_cancel_count(void);
static void frames(unsigned n)
{for(unsigned i=0;i<n;i++){lv_tick_inc(50);lv_timer_handler();glass_ui_memory_poll();glass_grok_test_step_visible();usleep(3000);}}
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
  wake_mini_present(90,lv_tick_get());frames(3);
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
    glass_local_voice_woke();frames(20);assert(g_local_wake.root);
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
  lv_indev_delete(pointer_input);pointer_input=NULL;
  puts("BUTTONLESS_WAKE_OK: no buttons; no duplicate AI submission; auto TTS exactly once; auto hide; late results cannot replay");
  puts("LOCAL_WAKE_OK: underlying page preserved; actual pointer protects dialog/bird; outside stops ASR/generation/TTS; newer request and global wake preserved");
}
static unsigned timer_count(void)
{unsigned count=0;for(lv_timer_t *t=lv_timer_get_next(NULL);t;t=lv_timer_get_next(t))count++;return count;}
static unsigned objects(lv_obj_t *root)
{unsigned n=1;for(unsigned i=0;i<lv_obj_get_child_count(root);i++)n+=objects(lv_obj_get_child(root,i));return n;}
static void page_ready(unsigned page)
{g_lock_page=page;glass_lock_open(NULL);frames(8);assert(g_lock_ui.root);assert(lock_art_test_fail?lock_art_bytes==0:(lock_art_bytes>0&&lock_art_bytes<384u*1024u));}
static void pointer_stroke(int x,int y,int ex,int ey)
{
  pointer_point=(lv_point_t){x,y};pointer_state=LV_INDEV_STATE_PRESSED;lv_indev_read(pointer_input);
  for(int i=1;i<=8;i++){
    lv_tick_inc(20);pointer_point=(lv_point_t){x+(ex-x)*i/8,y+(ey-y)*i/8};lv_indev_read(pointer_input);
  }
  pointer_state=LV_INDEV_STATE_RELEASED;lv_indev_read(pointer_input);frames(4);
}
static atomic_uint memory_test_stage;
static bool memory_test_timeout;
static void *memory_test_worker(void *unused)
{
 (void)unused;int ret=glass_ui_memory_begin(memory_test_timeout?1:200);
 if(memory_test_timeout){assert(ret==-ETIMEDOUT);glass_ui_memory_end();return NULL;}
 assert(!ret);atomic_store(&memory_test_stage,1);
 while(atomic_load(&memory_test_stage)!=2)usleep(1000);
 glass_ui_memory_end();return NULL;
}
static void check_memory_policy(void)
{
 glass_ui_memory_poll();
 assert(!glass_lyric_font_request(4));
 for(unsigned i=0;i<5000&&glass_lyric_font_state(4)!=3;i++){glass_lyric_font_poll();usleep(1000);}
 assert(glass_lyric_font_state(4)==3);glass_lyric_font_set_global(4);
 lv_font_t *selected=glass_lyric_font_get(4,20);assert(selected);
 struct glass_font_cache_status font_before,font_during,font_after;
 glass_lyric_font_cache_status(4,&font_before);assert(font_before.preloaded_bytes==0&&font_before.glyph_capacity==512);
 lv_obj_t *glyph_probe=lv_label_create(lv_layer_top());char samples[96];for(unsigned i=0;i<94;i++)samples[i]=(char)(33+i);samples[94]=0;
 lv_label_set_text(glyph_probe,samples);lv_obj_set_style_text_font(glyph_probe,selected,0);lv_obj_set_width(glyph_probe,1000);frames(3);
 uint32_t original=(uint32_t)lv_cache_get_max_size(LV_GLOBAL_DEFAULT()->img_cache,NULL);
 lv_image_cache_resize(131072,true);uint32_t saved=131072;
 assert(!glass_ui_memory_begin(0));assert(!glass_ui_memory_begin(0));
 assert(!lv_image_cache_is_enabled());
 struct gmat_stats material_stats;gmat_status(&material_stats);assert(material_stats.pressure&&!material_stats.cache_bytes);
 glass_lyric_font_cache_status(4,&font_during);assert(font_during.glyph_capacity==64&&glass_lyric_font_get(4,20)==selected);
 glass_ui_memory_end();glass_ui_memory_poll();assert(!lv_image_cache_is_enabled());
 glass_ui_memory_end();glass_ui_memory_poll();assert(lv_cache_get_max_size(LV_GLOBAL_DEFAULT()->img_cache,NULL)==saved);
 pthread_t worker;atomic_store(&memory_test_stage,0);memory_test_timeout=false;assert(!pthread_create(&worker,NULL,memory_test_worker,NULL));
 for(unsigned i=0;i<1000&&!atomic_load(&memory_test_stage);i++){glass_ui_memory_poll();usleep(1000);}
 assert(atomic_load(&memory_test_stage)==1&&!lv_image_cache_is_enabled());
 page_ready(2);frames(2);assert(lock_art_bytes>0&&lock_art_bytes<384u*1024u);assert(!lock_art_decode_sessions&&lock_art_line_peak<=2048);
 atomic_store(&memory_test_stage,2);pthread_join(worker,NULL);frames(1);
 assert(lv_cache_get_max_size(LV_GLOBAL_DEFAULT()->img_cache,NULL)==saved);
 lock_unlock(NULL);frames(1);assert(!lock_art_bytes&&!lock_art_decode_sessions);
 memory_test_timeout=true;assert(!pthread_create(&worker,NULL,memory_test_worker,NULL));pthread_join(worker,NULL);
 glass_ui_memory_poll();assert(!atomic_load(&ui_memory_users)&&lv_cache_get_max_size(LV_GLOBAL_DEFAULT()->img_cache,NULL)==saved);
 assert(!glass_ui_memory_begin(0));glass_ui_memory_end();glass_ui_memory_poll();
 assert(lv_cache_get_max_size(LV_GLOBAL_DEFAULT()->img_cache,NULL)==saved);
 lv_image_cache_resize(original,true);
 glass_lyric_font_cache_status(4,&font_after);assert(font_after.glyph_capacity==512&&font_after.preloaded_bytes==0&&glass_lyric_font_get(4,20)==selected);
 lv_obj_delete(glyph_probe);glass_lyric_font_set_global(global_font_selected);
 printf("FONT_MEMORY_OK preload=%u normal=%u during=%u restored=%u reads=%llu stable_pointer=1\n",font_after.preloaded_bytes,font_before.glyph_capacity,font_during.glyph_capacity,font_after.glyph_capacity,(unsigned long long)font_after.reads);
 gmat_status(&material_stats);assert(!material_stats.pressure&&material_stats.cache_bytes<=65536&&!material_stats.blob_bytes);printf("MATERIAL_UI_OK cache=%zu blobs=%u after_pressure=0\n",material_stats.cache_bytes,material_stats.blobs);
 printf("UI_MEMORY_OK restored=%u line_peak=%u users=%u timeout_balanced=1\n",saved,lock_art_line_peak,atomic_load(&ui_memory_users));
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
  pointer_input=lv_indev_create();lv_indev_set_type(pointer_input,LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(pointer_input,pointer_read);
  g_desktop.font16=glass_material_font_create(font,size,16,LV_FONT_KERNING_NORMAL,128);
  g_desktop.font20=glass_material_font_create(font,size,20,LV_FONT_KERNING_NORMAL,128);
  g_desktop.font28=glass_material_font_create(font,size,28,LV_FONT_KERNING_NORMAL,128);
  g_desktop.font16->fallback=glass_icon_font(16);g_desktop.font20->fallback=glass_icon_font(20);
  g_desktop.font28->fallback=glass_icon_font(24);
  lock_display_font=glass_material_font_create(font,size,192,LV_FONT_KERNING_NORMAL,128);g_glass.font72=glass_material_font_create(font,size,128,LV_FONT_KERNING_NORMAL,128);g_glass.font48=glass_material_font_create(font,size,48,LV_FONT_KERNING_NORMAL,128);glass_home_fonts_init(font,size);
  g_desktop.screen=glass_box(lv_screen_active(),0,32,1024,568,0,0);
  g_desktop.panel=glass_box(g_desktop.screen,0,0,1024,568,0,0);
  lv_obj_add_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN);glass_status_create();
  pet_engine_init(1024,568);pet_engine_set_visible(false);
  char path[1024];snprintf(path,sizeof(path),"%s/screens.json",review_out);
  review_manifest=fopen(path,"w");assert(review_manifest);fputs("[\n",review_manifest);
  extern void review_music_fixture(int);
  extern void review_music_now_fixture(const struct music_now_playing *);
  extern void review_lock_photo_fixture(int);
  extern void review_weather_fixture(int);
  extern void review_cover_fixture(const char *);
  char cover_path[1024];snprintf(cover_path,sizeof(cover_path),"%s/cover.rgb565",review_out);
  struct tm date={.tm_mon=10,.tm_mday=4,.tm_wday=3};char formatted[80];
  lock_date_format(formatted,sizeof(formatted),&date,GLASS_LANGUAGE_ZH_CN);assert(!strcmp(formatted,"11月4日  星期三"));
  lock_date_format(formatted,sizeof(formatted),&date,GLASS_LANGUAGE_EN);assert(!strcmp(formatted,"Wed, Nov 4"));
  for(unsigned theme=0;theme<2;theme++){
    g_desktop.light_theme=theme==0;g_glass.reduced_motion=theme==0;glass_rebuild();frames(20);
    unsigned timers=timer_count(),top_objects=objects(lv_layer_top());
    review_music_fixture(0);page_ready(0);
    assert(lv_obj_has_flag(g_lock_ui.player,LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_child_count(g_lock_ui.root)<=7);
    review_capture(display,"lock-clock-empty","极简时间");
    /* Time crossing midnight changes calendar without rebuilding its objects. */
    lock_clock_update(1793721540);int old_day=g_lock_ui.local.tm_yday;
    lock_clock_update(1793721660);assert(g_lock_ui.local.tm_yday!=old_day);
    lock_clock_update(0);assert(!g_lock_ui.time_valid);
    lock_clock_update(time(NULL));assert(g_lock_ui.time_valid);
    lv_obj_t *cover=glass_box(lv_layer_top(),0,0,1024,600,0x000000,0);
    time_t second=g_lock_ui.second;lock_refresh(NULL);
    assert(lv_obj_get_child(lv_layer_top(),-1)==cover&&g_lock_ui.second==second);
    lv_obj_delete(cover);lock_refresh(NULL);
    check_local_wake();assert(g_lock_ui.root);lock_refresh(NULL);
    lock_unlock(NULL);frames(3);
    lock_art_test_fail=true;page_ready(0);
    assert(lock_art_bytes==0);
    assert(lv_color_to_u32(lv_obj_get_style_bg_color(g_lock_ui.root,0))==lv_color_to_u32(lv_color_hex(0xf2d8bf)));
    lock_unlock(NULL);frames(3);lock_art_test_fail=false;
    timers=timer_count();top_objects=objects(lv_layer_top()); /* Warm shared wake UI first. */
    for(unsigned repeat=0;repeat<10;repeat++)for(unsigned page=0;page<4;page++){
      review_music_fixture(2);review_lock_photo_fixture(repeat%3);review_weather_fixture(repeat%2?0:1);
      page_ready(page);
      if(!repeat){char name[64];snprintf(name,sizeof(name),"lock-page-%u",page);review_capture(display,name,"锁屏页面");}
      if(page==1&&repeat==0){
        struct music_now_playing song=*g_lock_ui.shown;
        song.track_id=100;song.revision++;
        strcpy(song.name,"海阔天空");strcpy(song.artist,"BEYOND");
        strcpy(song.cover_url,"https://fixture/current-track-cover.jpg");
        review_cover_fixture(NULL);review_music_now_fixture(&song);lock_refresh(NULL);
        assert(!g_lock_ui.cover.canvas);
        /* Artwork completion must be handled even if playback revision is unchanged. */
        review_cover_fixture(cover_path);lock_refresh(NULL);frames(2);
        assert(g_lock_ui.cover.canvas&&g_lock_ui.cover.done);
        assert(lv_obj_get_parent(g_lock_ui.cover.canvas)==g_lock_ui.cover.host);
        lv_obj_update_layout(g_lock_ui.root);
        assert(lv_obj_get_width(g_lock_ui.cover.host)==368&&lv_obj_get_height(g_lock_ui.cover.host)==368);
        review_capture(display,"lock-page-1","音乐锁屏真实封面显示 · 歌曲与封面为测试示例");
        song.track_id++;song.revision++;song.cover_url[0]=0;
        review_music_now_fixture(&song);lock_refresh(NULL);assert(!g_lock_ui.cover.canvas);
        review_cover_fixture(NULL);review_music_now_fixture(NULL);lock_refresh(NULL);
        puts("LOCK_COVER_OK: delayed completion with unchanged playback revision; centered 368px artwork; next track clears stale image");
      }
      if(page<2){
        struct music_now_playing song=*g_lock_ui.shown;
        strcpy(song.name,"很长很长的歌名也能完整保持整洁，不挤到播放器按钮");song.revision++;
        review_music_now_fixture(&song);lock_refresh(NULL);
        assert(lv_label_get_long_mode(g_lock_ui.title)==LV_LABEL_LONG_DOT);
        assert(lv_obj_get_height(g_lock_ui.title)<=48);
        review_music_now_fixture(NULL);
      }
      if(page==2&&repeat==0){
        extern void review_weather_snapshot_fixture(const struct glass_weather_snapshot *);
        struct glass_weather_snapshot weather=g_lock_ui.weather;weather.revision++;weather.data.temperature=-100;
        review_weather_snapshot_fixture(&weather);lock_weather_update();lv_obj_update_layout(g_lock_ui.root);
        assert(lv_obj_get_x(g_lock_ui.weather_degree)>112&&lv_obj_get_x(g_lock_ui.weather_degree)<700);
        review_weather_snapshot_fixture(NULL);
      }
      if(page==3&&repeat==2){
        review_capture(display,"lock-photo-bright","亮照片底栏可读性");
        uint16_t upper,lower;
        memcpy(&upper,review_fb+(510*1024+512)*2,2);
        memcpy(&lower,review_fb+(598*1024+512)*2,2);
        assert(((upper>>5)&63)>=50&&((lower>>5)&63)<=32);
      }
      /* Pending page navigation must be cancelled when the lock closes. */
      lock_page_set((page+1)%4);lock_unlock(NULL);frames(3);
      assert(!g_lock_ui.root&&!g_lock_page_pending);
      assert(lock_art_bytes==0);
      if(timer_count()!=timers||objects(lv_layer_top())!=top_objects)
        fprintf(stderr,"LOCK_COUNTS page=%u cycle=%u timers=%u/%u objects=%u/%u\n",page,repeat,timer_count(),timers,objects(lv_layer_top()),top_objects);
      assert(timer_count()==timers&&objects(lv_layer_top())==top_objects);
    }
    page_ready(0);lock_page_set(1);frames(4);assert(g_lock_page==1&&g_lock_ui.root);
    if(!pointer_input){pointer_input=lv_indev_create();lv_indev_set_type(pointer_input,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(pointer_input,pointer_read);}
    struct glass_pager *footer=lv_obj_get_user_data(lv_obj_get_child(g_lock_ui.root,-1));
    lv_obj_t *active=footer->slots[g_lock_page].hit;
    pointer_point=(lv_point_t){476+24*g_lock_page,572};pointer_state=LV_INDEV_STATE_PRESSED;lv_indev_read(pointer_input);
    assert(lv_obj_has_state(active,LV_STATE_PRESSED));
    assert(lv_obj_get_style_bg_opa(active,LV_PART_MAIN)>LV_OPA_TRANSP);
    review_capture(display,"lock-rail-pressed","融合横条按压反馈");
    pointer_state=LV_INDEV_STATE_RELEASED;lv_indev_read(pointer_input);frames(3);assert(!g_lock_ui.root);
    page_ready(0);pointer_stroke(512,575,360,575);assert(g_lock_ui.root&&g_lock_page==1);
    pointer_stroke(512,575,512,450);assert(!g_lock_ui.root&&lock_art_bytes==0);
    check_memory_policy();
    puts("LOCK_REFINED_OK: four pages, wake overlay on top, opaque-cover pause/resume, midnight/invalid time, long labels, page-switch cancellation, real pointer unlock; stable timer and object counts");
  }
  fputs("\n]\n",review_manifest);fclose(review_manifest);return 0;
}
