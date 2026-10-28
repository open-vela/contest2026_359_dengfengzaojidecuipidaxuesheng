/* SPDX-License-Identifier: Apache-2.0 */
#define UNIFIED_DEVICE_NO_MAIN
#include "render_unified_devices.c"
extern void review_lyrics_fixture(unsigned,int);
extern void review_lyrics_text_fixture(const char *);
extern void review_lyrics_next_fixture(const char *);
extern void review_lyrics_translation_fixture(const char *);
extern void review_music_now_fixture(const struct music_now_playing *);
extern void review_lock_photo_fixture(int);
extern void review_cover_fixture(const char *);
static lv_point_t touch_point;
static lv_indev_state_t touch_state;
static unsigned leaked_touches;
static void touch_read(lv_indev_t *input,lv_indev_data_t *data)
{(void)input;data->point=touch_point;data->state=touch_state;}
static void touch(lv_indev_t *input,int x,int y,bool pressed)
{touch_point=(lv_point_t){x,y};touch_state=pressed?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;lv_tick_inc(20);lv_indev_read(input);}
static void underlying_click(lv_event_t *e){(void)e;leaked_touches++;}
static unsigned lock_object_count(lv_obj_t *root)
{unsigned count=1;for(unsigned i=0;i<lv_obj_get_child_count(root);i++)count+=lock_object_count(lv_obj_get_child(root,i));return count;}
static int lyric_screen_y(struct music_full_ui *full,unsigned index)
{
  unsigned slot=index%MUSIC_LYRICS_WINDOW;
  assert(full->row_index[slot]==index&&!lv_obj_has_flag(full->rows[slot],LV_OBJ_FLAG_HIDDEN));
  return MUSIC_FULL_PAD+index*MUSIC_FULL_PITCH-lv_obj_get_scroll_y(full->viewport)+
    lv_obj_get_style_translate_y(full->rows[slot],0);
}
static int lyric_screen_x(struct music_full_ui *full,unsigned index)
{return 22+lv_obj_get_style_translate_x(full->rows[index%MUSIC_LYRICS_WINDOW],0);}
static void lyric_arc_bounds(struct music_full_ui *full)
{
  lv_obj_update_layout(full->root);
  for(unsigned i=0;i<MUSIC_LYRICS_WINDOW;i++){
    if(lv_obj_has_flag(full->rows[i],LV_OBJ_FLAG_HIDDEN))continue;
    int x=lv_obj_get_style_translate_x(full->rows[i],0);
    assert(x>=0&&x<=MUSIC_FULL_ARC_WIDTH);
    assert(22+x+lv_obj_get_width(full->rows[i])<=560);
  }
}
#ifndef MUSIC_FULL_NO_MAIN
#include <unistd.h>
static void await_native(struct music_pv_ui *pv)
{
  music_pv_refresh(pv);assert(pv->valid);
}
int main(int argc,char **argv)
{
  assert(argc==3);review_out=argv[2];
  FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);size_t size=ftell(f);rewind(f);
  void *font=malloc(size);assert(font);assert(fread(font,1,size,f)==size);fclose(f);
  lv_init();lv_display_t *display=lv_display_create(1024,600);
  lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(display,review_fb,NULL,sizeof(review_fb),LV_DISPLAY_RENDER_MODE_FULL);
  lv_display_set_flush_cb(display,review_flush);
  g_desktop.font16=lv_tiny_ttf_create_data(font,size,16);
  g_desktop.font20=lv_tiny_ttf_create_data(font,size,20);
  g_desktop.font28=lv_tiny_ttf_create_data(font,size,28);
  g_desktop.font16->fallback=glass_icon_font(16);g_desktop.font20->fallback=glass_icon_font(20);g_desktop.font28->fallback=glass_icon_font(24);
  g_glass.font48=lv_tiny_ttf_create_data(font,size,48);glass_home_fonts_init(font,size);
  g_glass.reduced_motion=true;g_glass.temperature_c=-999;g_glass.humidity_pct=-1;g_glass.pm25=-1;
  g_desktop.screen=glass_box(lv_screen_active(),0,32,1024,568,0,0);lv_obj_remove_flag(g_desktop.screen,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_text_font(g_desktop.screen,zh_font(20),0);
  g_desktop.panel=glass_box(g_desktop.screen,0,0,1024,568,0x000000,0);lv_obj_add_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN);
  char path[1024];snprintf(path,sizeof(path),"%s/screens.json",review_out);review_manifest=fopen(path,"w");assert(review_manifest);fputs("[\n",review_manifest);




  g_glass.font72=lv_tiny_ttf_create_data(font,size,72);
  lv_indev_t *input=lv_indev_create();lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(input,touch_read);lv_indev_set_display(input,display);
  glass_edge_back_create();glass_status_create();
  for(unsigned theme=0;theme<2;theme++) {
    g_desktop.light_theme=theme!=0;glass_rebuild();glass_status_tick();review_music_fixture(2);
    struct music_now_playing now={.revision=40,.track_id=100,.position_ms=180000,.duration_ms=600000,
      .volume=40,.state=MUSIC_PLAYING,.lyrics_state=MUSIC_LYRICS_READY,.can_skip=true,.lyrics_started=true};
    strcpy(now.name,"城市与晚风");strcpy(now.artist,"声动 · 原创演示");
    review_music_now_fixture(&now);review_lyrics_fixture(192,48);
    glass_music(NULL);lv_obj_update_layout(g_desktop.screen);lv_obj_t *page=g_desktop.current_card;lv_timer_t *timer=g_music_ui.timer;
    lv_obj_t *entry=review_callback_find(page,music_full_open);assert(entry);
    lv_obj_send_event(entry,LV_EVENT_CLICKED,NULL);assert(g_music_ui.full);
    struct music_full_ui *full=g_music_ui.full;
    lv_obj_update_layout(full->root);lv_area_t bounds;lv_obj_get_coords(full->root,&bounds);
    assert(bounds.x1==0&&bounds.y1==0&&bounds.x2==1023&&bounds.y2==599);
    assert(lv_obj_has_flag(g_status_ui.bar,LV_OBJ_FLAG_HIDDEN));
    assert(full->total==192&&full->current==48&&full->window.count==MUSIC_LYRICS_WINDOW);
    assert(lyric_screen_x(full,48)==22);
    assert(lyric_screen_x(full,47)==lyric_screen_x(full,49));
    assert(lyric_screen_x(full,50)>lyric_screen_x(full,49));
    lyric_arc_bounds(full);
    unsigned objects=lock_object_count(full->root);lv_obj_t *slot=full->rows[0];
    int previous_y=lyric_screen_y(full,49);
    int previous_x=lyric_screen_x(full,49);
    g_glass.reduced_motion=false;
    review_lyrics_translation_fixture("Give today to the gentle evening breeze");
    review_lyrics_fixture(192,49);music_ui_refresh(NULL);
    assert(lyric_screen_y(full,49)==previous_y);
    assert(lyric_screen_x(full,49)==previous_x);
    unsigned current_slot=49%MUSIC_LYRICS_WINDOW,next_slot=50%MUSIC_LYRICS_WINDOW;
    assert(!lv_obj_has_flag(full->translations[current_slot],LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_parent(full->translations[current_slot])==full->rows[current_slot]);
    lv_obj_t *animated=NULL;
    for(unsigned i=0;i<MUSIC_LYRICS_WINDOW;i++)if(full->row_active[i])animated=full->labels[i];
    assert(animated&&lv_anim_get(animated,music_full_lyric_opacity));
    int start_opacity=lv_obj_get_style_text_opa(animated,0);
    int start_x=lyric_screen_x(full,49);
    lv_tick_inc(120);lv_timer_handler();
    assert(lv_obj_get_style_text_opa(animated,0)>start_opacity);
    assert(lyric_screen_x(full,49)<start_x);
    lyric_arc_bounds(full);
    assert(lv_obj_get_style_text_opa(animated,0)==lv_obj_get_style_text_opa(full->translations[current_slot],0));
    assert(lv_obj_get_style_translate_y(full->rows[current_slot],0)<
      lv_obj_get_style_translate_y(full->rows[next_slot],0));
    lv_anim_t *moving=lv_anim_get(full->rows[current_slot],music_full_lyric_offset);assert(moving);
    int32_t animation_time=moving->act_time;
    music_full_draw();music_full_draw();
    assert(lv_anim_get(full->rows[current_slot],music_full_lyric_offset)==moving&&moving->act_time==animation_time);
    review_capture(display,"full-lyrics-cascade","全屏歌词 · 半弧滑动与双语同步");
    /* A rapid next line drops the unfinished transition and catches up to
     * the current audio line instead of accumulating visual delay. */
    previous_y=lyric_screen_y(full,50);
    previous_x=lyric_screen_x(full,50);
    review_lyrics_fixture(192,50);music_ui_refresh(NULL);
    assert(lyric_screen_y(full,50)==MUSIC_FULL_PAD&&full->labels[current_slot]==animated);
    assert(lyric_screen_x(full,50)==22);
    for(unsigned i=0;i<MUSIC_LYRICS_WINDOW;i++)assert(!lv_anim_get(full->rows[i],music_full_lyric_offset));
    now.state=MUSIC_PAUSED;review_music_now_fixture(&now);music_ui_refresh(NULL);
    for(unsigned i=0;i<MUSIC_LYRICS_WINDOW;i++){
      assert(!lv_anim_get(full->labels[i],music_full_lyric_opacity));
      assert(!lv_anim_get(full->translations[i],music_full_lyric_opacity));
      assert(!lv_anim_get(full->rows[i],music_full_lyric_offset));
      assert(lv_obj_get_style_translate_y(full->rows[i],0)==0);
    }
    now.state=MUSIC_PLAYING;review_music_now_fixture(&now);
    review_lyrics_translation_fixture(NULL);
    review_lyrics_fixture(192,51);music_ui_refresh(NULL);
    lv_tick_inc(700);lv_timer_handler();music_full_draw();
    assert(lv_obj_get_style_text_opa(full->labels[51%MUSIC_LYRICS_WINDOW],0)==LV_OPA_COVER);
    assert(lv_obj_get_style_text_opa(full->labels[52%MUSIC_LYRICS_WINDOW],0)>
      lv_obj_get_style_text_opa(full->labels[53%MUSIC_LYRICS_WINDOW],0));
    for(unsigned i=0;i<MUSIC_LYRICS_WINDOW;i++)assert(lv_obj_get_style_translate_y(full->rows[i],0)==0);
    assert(lyric_screen_x(full,51)==22);
    assert(lyric_screen_x(full,50)==lyric_screen_x(full,52));
    lyric_arc_bounds(full);
    review_capture(display,"full-lyrics-arc","全屏歌词 · 半弧轨迹与当前句聚焦");
    review_lyrics_fixture(192,52);music_ui_refresh(NULL);
    int entering_x=lyric_screen_x(full,52),leaving_x=lyric_screen_x(full,51);
    for(unsigned frame=0;frame<14;frame++){
      lyric_arc_bounds(full);
      assert(lyric_screen_x(full,52)<=entering_x&&lyric_screen_x(full,51)>=leaving_x);
      entering_x=lyric_screen_x(full,52);leaving_x=lyric_screen_x(full,51);
      if(!theme){
        char id[64];snprintf(id,sizeof(id),"full-lyrics-arc-frame-%02u",frame);
        review_capture(display,id,"全屏歌词 · 半弧滑入滑出动画帧");
      }
      lv_tick_inc(45);lv_timer_handler();
    }
    assert(lyric_screen_x(full,52)==22&&lyric_screen_x(full,51)>22);
    /* A seek bypasses both the cascade and fade, with no delayed tails. */
    review_lyrics_fixture(192,70);music_ui_refresh(NULL);
    for(unsigned i=0;i<MUSIC_LYRICS_WINDOW;i++){
      assert(!lv_anim_get(full->rows[i],music_full_lyric_offset));
      assert(!lv_anim_get(full->labels[i],music_full_lyric_opacity));
    }
    review_lyrics_fixture(192,71);music_ui_refresh(NULL);
    assert(lv_anim_get(full->rows[71%MUSIC_LYRICS_WINDOW],music_full_lyric_offset));
    g_glass.reduced_motion=true;music_ui_refresh(NULL);
    for(unsigned i=0;i<MUSIC_LYRICS_WINDOW;i++){
      assert(!lv_anim_get(full->labels[i],music_full_lyric_opacity));
      assert(!lv_anim_get(full->rows[i],music_full_lyric_offset));
      assert(lv_obj_get_style_translate_y(full->rows[i],0)==0);
      assert(lv_obj_get_style_text_opa(full->labels[i],0)==LV_OPA_COVER);
    }
    review_lyrics_fixture(192,48);music_ui_refresh(NULL);
    review_capture(display,"full-lyrics-playing","全屏歌词 · 自动跟随与当前行高亮");
    /* PV uses the real LRC cut times, never a second wall-clock player. */
    now.position_ms=192100;review_music_now_fixture(&now);g_glass.reduced_motion=false;
    lv_obj_send_event(full->mode,LV_EVENT_CLICKED,NULL);
    struct music_pv_ui *pv=&full->pv;
    await_native(pv);
    assert(pv->visible&&!lv_timer_get_paused(pv->timer)&&pv->valid&&pv->current==48);
    assert(pv->start==192000&&pv->end==196000&&lv_obj_has_flag(full->viewport,LV_OBJ_FLAG_HIDDEN));
    int opacity=lv_obj_get_style_opa(pv->groups[0],0);assert(opacity<LV_OPA_COVER);
    unsigned updates=pv->updates,plans=pv->plans;
    now.state=MUSIC_PAUSED;review_music_now_fixture(&now);
    lv_tick_inc(800);lv_timer_handler();
    assert(lv_obj_get_style_opa(pv->groups[0],0)==opacity&&pv->updates==updates&&pv->plans==plans);
    now.state=MUSIC_PLAYING;now.position_ms=192700;review_music_now_fixture(&now);music_pv_refresh(pv);
    music_ui_refresh(NULL);assert(lv_obj_get_style_opa(pv->groups[0],0)==LV_OPA_COVER);
    unsigned pv_objects=lock_object_count(full->root),seen=0;
    const char *samples[]={"把喧嚣留在身后","让平凡的今天闪闪发光","A new song, a quiet night!","月","你好，世界！"};
    for(unsigned style=0;style<MUSIC_PV_LAYOUTS;style++){
      assert(pv->style_index==style);seen|=1u<<pv->layout;
      for(unsigned scene=0;scene<5;scene++){
        review_lyrics_text_fixture(samples[scene]);pv->valid=false;now.position_ms=192800;
        review_music_now_fixture(&now);music_pv_refresh(pv);lv_obj_update_layout(pv->root);
        size_t bytes=0;for(unsigned i=0;i<pv->count;i++){
          bytes+=strlen(pv->parts[i]);
          assert(lv_obj_get_x(pv->groups[i])>=0&&lv_obj_get_x(pv->groups[i])+lv_obj_get_width(pv->groups[i])<=976);
          assert(lv_obj_get_y(pv->groups[i])>=60&&lv_obj_get_y(pv->groups[i])+lv_obj_get_height(pv->groups[i])<=336);
        }
        assert(bytes==strlen(samples[scene]));
        char id[64];snprintf(id,sizeof(id),"native-effect-%u-scene-%u",style,scene);
        review_capture(display,id,"实时歌词 · 三套固定效果，无歌曲帧素材");
      }
      lv_obj_send_event(pv->style_button,LV_EVENT_CLICKED,NULL);
      assert(lock_object_count(full->root)==pv_objects);
    }
    assert(seen==7&&pv->style_index==0);
    review_lyrics_text_fixture("把喧嚣留在身后");
    pv->style_index=2;pv->valid=false;now.position_ms=192320;
    review_music_now_fixture(&now);music_pv_refresh(pv);assert(pv->layout==2);
    const char *partial=lv_label_get_text(pv->text[0]);size_t partial_bytes=strlen(partial);
    assert(partial_bytes>0&&partial_bytes<strlen(pv->parts[0])&&partial_bytes%3==0);
    updates=pv->updates;plans=pv->plans;now.state=MUSIC_PAUSED;review_music_now_fixture(&now);
    lv_tick_inc(1000);lv_timer_handler();
    assert(strlen(lv_label_get_text(pv->text[0]))==partial_bytes&&pv->updates==updates&&pv->plans==plans);
    now.state=MUSIC_PLAYING;now.position_ms=193700;review_music_now_fixture(&now);music_pv_refresh(pv);
    for(unsigned i=0;i<pv->count;i++)assert(!strcmp(lv_label_get_text(pv->text[i]),pv->parts[i]));
    /* Backward seeks reconstruct the exact reveal, without wall-clock drift. */
    now.position_ms=192000;review_music_now_fixture(&now);music_pv_refresh(pv);
    for(unsigned i=0;i<pv->count;i++)assert(!strlen(lv_label_get_text(pv->text[i])));
    g_glass.reduced_motion=true;music_pv_refresh(pv);
    for(unsigned i=0;i<pv->count;i++)assert(!strcmp(lv_label_get_text(pv->text[i]),pv->parts[i]));
    review_lyrics_text_fixture(NULL);
    now.position_ms=192001;review_music_now_fixture(&now);g_glass.reduced_motion=true;music_pv_refresh(pv);
    assert(lv_obj_get_style_opa(pv->groups[0],0)==LV_OPA_COVER);
    char long_text[MUSIC_LYRIC_TEXT_MAX];memset(long_text,'W',sizeof(long_text)-1);long_text[sizeof(long_text)-1]=0;
    review_lyrics_text_fixture(long_text);pv->valid=false;music_pv_refresh(pv);
    size_t bytes=0;for(unsigned i=0;i<pv->count;i++)bytes+=strlen(pv->parts[i]);
    assert(pv->count==3&&bytes==strlen(long_text));
    lv_obj_update_layout(pv->root);
    for(unsigned i=0;i<pv->count;i++)assert(lv_obj_get_height(pv->text[i])<=88);
    char cjk[MUSIC_LYRIC_TEXT_MAX];cjk[0]=0;for(unsigned i=0;i<63;i++)strcat(cjk,"字");
    review_lyrics_text_fixture(cjk);pv->valid=false;music_pv_refresh(pv);
    bytes=0;for(unsigned i=0;i<pv->count;i++){assert(strlen(pv->parts[i])%3==0);bytes+=strlen(pv->parts[i]);}
    assert(bytes==189);
    review_capture(display,"full-lyrics-pv-long","文字 PV · 长行安全分段");
    review_lyrics_fixture(192,-1);music_pv_refresh(pv);assert(!pv->valid&&!lv_obj_has_flag(pv->hint,LV_OBJ_FLAG_HIDDEN));
    now.track_id++;now.state=MUSIC_LOADING;now.lyrics_state=MUSIC_LYRICS_LOADING;
    review_music_now_fixture(&now);music_pv_refresh(pv);assert(!pv->valid);
    for(unsigned i=0;i<MUSIC_PV_PARTS;i++)assert(lv_obj_has_flag(pv->groups[i],LV_OBJ_FLAG_HIDDEN));
    review_lyrics_text_fixture(NULL);review_lyrics_fixture(192,48);
    now.track_id=100;now.state=MUSIC_PLAYING;now.lyrics_state=MUSIC_LYRICS_READY;now.position_ms=180000;
    review_music_now_fixture(&now);lv_obj_send_event(full->mode,LV_EVENT_CLICKED,NULL);
    assert(!pv->visible&&lv_timer_get_paused(pv->timer)&&!lv_obj_has_flag(full->viewport,LV_OBJ_FLAG_HIDDEN));
    const char *title=lv_label_get_text(full->title);music_ui_refresh(NULL);assert(title==lv_label_get_text(full->title));
    g_glass.reduced_motion=false;review_lyrics_fixture(192,49);music_ui_refresh(NULL);
    assert(lv_anim_get(full->rows[49%MUSIC_LYRICS_WINDOW],music_full_lyric_offset));
    touch(input,700,300,true);touch(input,700,280,true);touch(input,700,230,true);touch(input,700,170,true);touch(input,700,170,false);
    assert(!full->follow);
    for(unsigned i=0;i<MUSIC_LYRICS_WINDOW;i++){
      assert(!lv_anim_get(full->rows[i],music_full_lyric_offset));
      assert(!lv_anim_get(full->labels[i],music_full_lyric_opacity));
    }
    g_glass.reduced_motion=true;
    lv_obj_scroll_to_y(full->viewport,130*MUSIC_FULL_PITCH,LV_ANIM_OFF);music_full_draw();
    int manual_y=lv_obj_get_scroll_y(full->viewport);
    lyric_arc_bounds(full);
    assert(full->window.first>100&&full->rows[0]==slot&&lock_object_count(full->root)==objects);
    now.revision++;now.position_ms=230000;review_music_now_fixture(&now);review_lyrics_fixture(192,75);music_ui_refresh(NULL);
    assert(lv_obj_get_scroll_y(full->viewport)==manual_y&&!full->follow);
    review_capture(display,"full-lyrics-browse","全屏歌词 · 手动浏览，不打断播放");
    lv_obj_send_event(full->follow_button,LV_EVENT_CLICKED,NULL);
    assert(full->follow&&lv_obj_get_scroll_y(full->viewport)==75*MUSIC_FULL_PITCH);
    lv_obj_send_event(lv_obj_get_parent(full->play),LV_EVENT_CLICKED,NULL);
    assert(!strcmp(lv_label_get_text(full->status),"已暂停"));
    review_capture(display,"full-lyrics-paused","全屏歌词 · 暂停与播放控制");
    lv_obj_send_event(lv_obj_get_parent(full->play),LV_EVENT_CLICKED,NULL);
    bool clicked=false;
    for(unsigned i=0;i<lv_obj_get_child_count(full->root);i++){
      lv_obj_t *child=lv_obj_get_child(full->root,i);
      if(child==full->next){lv_obj_send_event(child,LV_EVENT_CLICKED,NULL);clicked=true;break;}
    }
    assert(clicked&&full->window.count==0);
    assert(!strcmp(lv_label_get_text(full->hint),"正在获取歌词…"));
    review_capture(display,"full-lyrics-loading","全屏歌词 · 切歌清理旧歌词");
    now.track_id=102;now.revision=50;now.lyrics_state=MUSIC_LYRICS_MISSING;now.position_ms=0;
    strcpy(now.name,"下一首示例");review_music_now_fixture(&now);music_ui_refresh(NULL);
    assert(!strcmp(lv_label_get_text(full->hint),"暂无歌词 · 仍可继续听歌"));
    review_capture(display,"full-lyrics-missing","全屏歌词 · 没有歌词也能播放");
    music_full_close(NULL);assert(!g_music_ui.full&&g_desktop.current_card==page&&g_music_ui.timer==timer);
    assert(!lv_obj_has_flag(g_status_ui.bar,LV_OBJ_FLAG_HIDDEN));
    struct music_now_playing after;glass_music_now_playing(&after);assert(after.state==MUSIC_PLAYING);
    now.track_id=100;now.lyrics_state=MUSIC_LYRICS_READY;review_music_now_fixture(&now);
    g_glass.reduced_motion=false;
    for(unsigned i=0;i<30;i++){
      review_lyrics_fixture(192,48);music_full_open(NULL);
      review_lyrics_fixture(192,49);music_ui_refresh(NULL);
      lv_obj_t *moving_row=g_music_ui.full->rows[49%MUSIC_LYRICS_WINDOW];
      assert(lv_anim_get(moving_row,music_full_lyric_offset));
      if(i%2)music_full_mode(NULL);music_full_close(NULL);
      assert(!lv_anim_get(moving_row,music_full_lyric_offset));
    }
    g_glass.reduced_motion=true;
    music_full_open(NULL);g_edge_page=page;glass_edge_back_async(NULL);assert(!g_music_ui.full&&g_desktop.current_card==page);
    music_full_open(NULL);glass_lock_open(NULL);assert(!g_music_ui.full&&g_lock_ui.root);lock_unlock(NULL);
    music_full_open(NULL);panel_hide(NULL);assert(!g_music_ui.full&&!g_music_ui.root);
    assert(!lv_obj_has_flag(g_status_ui.bar,LV_OBJ_FLAG_HIDDEN));
  }
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("MUSIC_FULL_NATIVE_OK: both themes; 3 live effects; arbitrary CJK/Latin lyrics; bounded geometry; UTF-8 reveal, backward seek, pause and reduced motion; audio-clock cuts and repeated PV teardown; half-ellipse lyric entry/exit, bounded widths, continuous rapid advances on both axes, stable row recycling, synchronized translations and distance fading; 600px fullscreen; 192 lyrics with 9 retained rows; pointer interruption; manual hold/follow; transport; return without stopping; lock, edge back, page deletion and 60 reopen cycles during active animations");return 0;
}
#endif
