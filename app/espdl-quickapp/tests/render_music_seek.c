/* SPDX-License-Identifier: Apache-2.0 */
#define MUSIC_FULL_NO_MAIN
/* The music fixture does not enter the video full-screen presentation. */
static _Bool fullscreen_on_screen;
#include "render_music_full.c"
extern unsigned review_music_seek_requests;
extern void review_lyrics_step_fixture(unsigned);

static void lyric_sync_checks(struct music_now_playing *now)
{
  struct music_full_ui *f=g_music_ui.full;assert(f);
  g_glass.reduced_motion=false;now->lyrics_state=MUSIC_LYRICS_READY;
  now->position_ms=4000;now->revision++;review_music_now_fixture(now);review_lyrics_fixture(20,1);
  lv_timer_reset(g_music_ui.timer);lv_timer_reset(g_music_ui.sync_timer);
  lv_tick_inc(50);lv_timer_handler();assert(f->current==1);
  /* Normal transitions finish within 220 ms, including neighbour stagger. */
  now->position_ms=8000;review_music_now_fixture(now);review_lyrics_fixture(20,2);
  music_ui_sync_tick(NULL);assert(f->current==2&&f->move_ms<=220);
  assert(lv_anim_get(f->rows[2%MUSIC_LYRICS_WINDOW],music_full_lyric_offset));
  lv_tick_inc(80);lv_timer_handler();
  now->position_ms=12000;review_music_now_fixture(now);review_lyrics_fixture(20,3);
  music_ui_sync_tick(NULL);assert(f->current==3);
  for(unsigned i=0;i<MUSIC_LYRICS_WINDOW;i++){
    assert(!lv_anim_get(f->rows[i],music_full_lyric_offset));
    assert(lv_obj_get_style_translate_y(f->rows[i],0)==0);
  }
  /* A late tick never spends another animation duration on the old line. */
  now->position_ms=16500;review_music_now_fixture(now);review_lyrics_fixture(20,4);
  music_ui_sync_tick(NULL);assert(f->current==4&&f->move_ms==0);
  assert(!lv_anim_get(f->rows[4],music_full_lyric_offset));
  review_lyrics_step_fixture(100);
  now->position_ms=500;review_music_now_fixture(now);review_lyrics_fixture(20,5);
  music_ui_sync_tick(NULL);assert(f->current==5&&f->move_ms<=50);
  assert(!lv_anim_get(f->rows[5],music_full_lyric_offset));
  now->position_ms=950;review_music_now_fixture(now);review_lyrics_fixture(20,9);
  music_ui_sync_tick(NULL);assert(f->current==9);
  assert(lv_obj_get_scroll_y(f->viewport)==9*MUSIC_FULL_PITCH);
  now->state=MUSIC_PAUSED;now->pause_requested=true;review_music_now_fixture(now);
  music_ui_sync_tick(NULL);lv_tick_inc(500);lv_timer_handler();assert(f->current==9);
  review_lyrics_step_fixture(4000);g_glass.reduced_motion=true;
  now->state=MUSIC_PLAYING;now->pause_requested=false;now->lyrics_state=MUSIC_LYRICS_MISSING;
  now->position_ms=15000;now->revision++;review_music_now_fixture(now);
}

static void seek_drag(lv_indev_t *input, lv_obj_t *slider, struct music_seek_ui *seek)
{
  lv_obj_update_layout(lv_layer_top());lv_obj_update_layout(g_desktop.screen);
  lv_area_t a;lv_obj_get_coords(slider,&a);
  int y=(a.y1+a.y2)/2;
  unsigned requests=review_music_seek_requests;
  touch(input,a.x1+4,y,true);
  assert(seek->dragging);
  touch(input,a.x2-4,y,true);
  int forward=lv_slider_get_value(slider);assert(forward>850);
  music_ui_refresh(NULL);assert(lv_slider_get_value(slider)==forward);
  assert(review_music_seek_requests==requests);
  touch(input,a.x1+lv_obj_get_width(slider)/4,y,true);
  int backward=lv_slider_get_value(slider);assert(backward>150&&backward<350);
  music_ui_refresh(NULL);assert(lv_slider_get_value(slider)==backward);
  assert(!strcmp(lv_label_get_text(seek->elapsed),"00:15"));
  assert(review_music_seek_requests==requests);
  touch(input,a.x1+lv_obj_get_width(slider)/4,y,false);
  assert(!seek->dragging&&review_music_seek_requests==requests+1);
  struct music_now_playing now;glass_music_now_playing(&now);
  assert(now.position_ms>=14000&&now.position_ms<=16000);
}

int main(int argc,char **argv)
{
  assert(argc==3);review_out=argv[2];
  FILE *file=fopen(argv[1],"rb");assert(file);fseek(file,0,SEEK_END);size_t size=ftell(file);rewind(file);
  void *font=malloc(size);assert(font&&fread(font,1,size,file)==size);fclose(file);
  lv_init();lv_display_t *display=lv_display_create(1024,600);
  lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(display,review_fb,NULL,sizeof(review_fb),LV_DISPLAY_RENDER_MODE_FULL);
  lv_display_set_flush_cb(display,review_flush);
  g_desktop.font16=lv_tiny_ttf_create_data(font,size,16);
  g_desktop.font20=lv_tiny_ttf_create_data(font,size,20);
  g_desktop.font28=lv_tiny_ttf_create_data(font,size,28);
  g_glass.font48=lv_tiny_ttf_create_data(font,size,48);glass_home_fonts_init(font,size);
  g_glass.reduced_motion=true;g_glass.temperature_c=-999;g_glass.humidity_pct=-1;g_glass.pm25=-1;
  g_desktop.screen=glass_box(lv_screen_active(),0,32,1024,568,0,0);
  lv_obj_remove_flag(g_desktop.screen,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_text_font(g_desktop.screen,zh_font(20),0);
  g_desktop.panel=glass_box(g_desktop.screen,0,0,1024,568,0,0);
  lv_obj_add_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN);
  char path[1024];snprintf(path,sizeof(path),"%s/screens.json",review_out);
  review_manifest=fopen(path,"w");assert(review_manifest);fputs("[\n",review_manifest);
  lv_indev_t *input=lv_indev_create();lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(input,touch_read);lv_indev_set_display(input,display);
  glass_edge_back_create();glass_status_create();
  for(unsigned theme=0;theme<2;theme++) {
    g_desktop.light_theme=theme!=0;glass_rebuild();glass_status_tick();review_music_fixture(2);
    struct music_now_playing now={.revision=1,.track_id=100+theme,.position_ms=12000,
      .duration_ms=60000,.volume=40,.state=MUSIC_PLAYING,.lyrics_state=MUSIC_LYRICS_MISSING};
    strcpy(now.name,"缓冲与进度条测试");strcpy(now.artist,"本地音乐");
    review_music_now_fixture(&now);glass_music(NULL);
    seek_drag(input,g_music_ui.progress,&g_music_ui.seek);
    review_capture(display,"music-seek-main","音乐 · 可拖动进度条");
    music_full_open(NULL);assert(g_music_ui.full);
    seek_drag(input,g_music_ui.full->progress,&g_music_ui.full->seek);
    review_capture(display,"music-seek-full","全屏歌词 · 可拖动进度条");
    lyric_sync_checks(&now);
    music_full_close(NULL);
    /* A track switch between press/release must reject the old gesture. */
    unsigned requests=review_music_seek_requests;
    lv_obj_send_event(g_music_ui.progress,LV_EVENT_PRESSED,NULL);
    now.track_id++;now.revision+=10;review_music_now_fixture(&now);
    lv_slider_set_value(g_music_ui.progress,900,LV_ANIM_OFF);
    lv_obj_send_event(g_music_ui.progress,LV_EVENT_RELEASED,NULL);
    assert(review_music_seek_requests==requests);
    now.duration_ms=0;now.revision++;review_music_now_fixture(&now);music_ui_refresh(NULL);
    assert(lv_obj_has_state(g_music_ui.progress,LV_STATE_DISABLED));
    panel_hide(NULL);
  }
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("MUSIC_SEEK_UI_OK: actual LVGL pointer drag in both directions, main/fullscreen and dark/light; preview survives refresh; one seek on release; stale gesture and unknown duration rejected");
  puts("MUSIC_LYRIC_SYNC_OK: 50ms timer without slow page refresh; audio-timed 220ms animation deadline; rapid lines cancel old motion; late/dense/missed lines settle immediately; pause retains position");
  return 0;
}
