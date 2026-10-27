/* Real LVGL launcher/card/player integration; playback I/O is offline. */
#include <stdlib.h>
static size_t music_snapshot_fail_bytes;
static void *music_snapshot_test_calloc(size_t count,size_t bytes)
{
  if(music_snapshot_fail_bytes && count*bytes==music_snapshot_fail_bytes)return NULL;
  return calloc(count,bytes);
}
#define calloc music_snapshot_test_calloc
#define MUSIC_FULL_NO_MAIN
#include "render_music_full.c"
#undef calloc

static void music_app_click(lv_obj_t *root,lv_event_cb_t callback)
{
  lv_obj_update_layout(g_desktop.screen);
  lv_obj_t *button=review_callback_find(root,callback);assert(button);
  lv_obj_send_event(button,LV_EVENT_PRESSED,NULL);
  lv_obj_send_event(button,LV_EVENT_RELEASED,NULL);
  lv_obj_send_event(button,LV_EVENT_CLICKED,NULL);
}

static void music_test_cover(const char *path)
{
  FILE *f=fopen(path,"wb");assert(f);uint32_t dimensions[]={240,180};
  assert(fwrite(dimensions,4,2,f)==2);
  for(unsigned y=0;y<180;y++)for(unsigned x=0;x<240;x++){
    uint16_t color=((x/8)<<11)|((y/3)<<5)|((x+y)%32);
    assert(fwrite(&color,2,1,f)==1);
  }
  fclose(f);
}

/* Check painted pixels, not the label box that previously hid a baseline
 * offset. Both glyphs must leave balanced space inside the actual circle. */
static void music_test_play_center(lv_display_t *display,bool pause)
{
  lv_obj_update_layout(g_desktop.screen);lv_refr_now(display);
  lv_area_t a;lv_obj_get_coords(g_home_music.play,&a);
  uint32_t color=lv_color_to_u32(lv_obj_get_style_text_color(g_home_music.play,0));
  uint16_t ink=((color>>8)&0xf800)|((color>>5)&0x07e0)|((color>>3)&0x001f);
  int x1=a.x2,y1=a.y2,x2=a.x1,y2=a.y1,count=0;
  for(int y=a.y1+10;y<=a.y2-10;y++)for(int x=a.x1+10;x<=a.x2-10;x++){
    unsigned offset=(y*1024+x)*2;
    uint16_t pixel=review_fb[offset]|((uint16_t)review_fb[offset+1]<<8);
    if(pixel!=ink)continue;
    if(x<x1)x1=x;if(x>x2)x2=x;if(y<y1)y1=y;if(y>y2)y2=y;count++;
  }
  assert(count>80&&y2-y1>=15);
  int above=y1-a.y1,below=a.y2-y2;
  printf("HOME_ICON_CENTER %s %s: top=%d bottom=%d ink=%dx%d pixels=%d\n",
    g_desktop.light_theme?"light":"dark",pause?"pause":"play",above,below,x2-x1+1,y2-y1+1,count);
  assert(abs(above-below)<=1);
  if(pause){
    assert(abs((x1-a.x1)-(a.x2-x2))<=1);
    /* The gap is visible all the way down: two bars, not the play triangle. */
    int mid=(a.x1+a.x2)/2;
    for(int y=y1;y<=y2;y++){
      unsigned offset=(y*1024+mid)*2;
      assert((review_fb[offset]|((uint16_t)review_fb[offset+1]<<8))!=ink);
    }
  }else{
    /* The pointed half is narrower than the filled vertical left edge. */
    int left_count=0,right_count=0;
    for(int y=y1;y<=y2;y++){
      unsigned l=(y*1024+x1)*2,r=(y*1024+x2)*2;
      left_count+=(review_fb[l]|((uint16_t)review_fb[l+1]<<8))==ink;
      right_count+=(review_fb[r]|((uint16_t)review_fb[r+1]<<8))==ink;
    }
    assert(left_count>right_count);
  }
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
  g_desktop.screen=glass_box(lv_screen_active(),0,32,1024,568,0,0);
  lv_obj_remove_flag(g_desktop.screen,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_text_font(g_desktop.screen,zh_font(20),0);
  g_desktop.panel=glass_box(g_desktop.screen,0,0,1024,568,0x000000,0);
  lv_obj_add_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN);
  char path[1024];snprintf(path,sizeof(path),"%s/screens.json",review_out);
  review_manifest=fopen(path,"w");assert(review_manifest);fputs("[\n",review_manifest);
  char test_cover[1024];snprintf(test_cover,sizeof(test_cover),"%s/test-cover.rgb565",review_out);
  music_test_cover(test_cover);
  const char *actual_cover=getenv("MUSIC_COVER_FIXTURE");
  const char *cover_path=actual_cover?actual_cover:test_cover;
  glass_edge_back_create();glass_status_create();
  for(unsigned theme=0;theme<2;theme++){
    panel_hide(NULL);g_desktop.light_theme=theme!=0;
    struct music_now_playing now={.state=MUSIC_IDLE};
    review_music_now_fixture(&now);glass_rebuild();glass_update(NULL);
    assert(!g_home_music.cover.canvas);
    assert(!strcmp(lv_label_get_text(g_home_music.artist),"搜索好歌，收藏你的热爱"));
    review_capture(display,"music-home-empty","空闲音乐卡片");
    music_app_click(g_home_music.root,home_music_toggle);
    assert(g_music_ui.root&&!g_music_ui.return_apps);
    music_app_click(g_music_ui.root,music_ui_back);
    assert(!g_music_ui.root&&lv_obj_has_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN));

    now=(struct music_now_playing){.revision=90,.track_id=10,.state=MUSIC_PLAYING,
      .position_ms=42000,.duration_ms=204000,.lyrics_state=MUSIC_LYRICS_READY,.lyrics_started=true};
    strcpy(now.name,"城市与晚风");strcpy(now.artist,"声动 · 原创演示");strcpy(now.album,"晚风来信");
    if(actual_cover){strcpy(now.name,"海阔天空");strcpy(now.artist,"Beyond");strcpy(now.album,"海阔天空");}
    strcpy(now.cover_url,"https://fixture/current-cover.jpg");now.can_skip=true;
    review_cover_fixture(NULL);
    review_music_now_fixture(&now);review_lyrics_fixture(51,10);glass_update(NULL);
    assert(g_home_music.cover.requested&&!g_home_music.cover.canvas);
    review_capture(display,"music-home-cover-loading","等待当前歌曲封面");
    /* An image arriving without a playback revision must still be painted. */
    review_cover_fixture(cover_path);glass_update(NULL);assert(g_home_music.cover.canvas);
    assert(g_home_music.cover.track_id==now.track_id);
    assert(!strcmp(g_home_music.cover.url,now.cover_url));
    assert(!strcmp(lv_label_get_text(g_home_music.title),now.name));
    assert(!strcmp(lv_label_get_text(g_home_music.artist),now.artist));
    assert(!strcmp(lv_label_get_text(g_home_music.elapsed),"00:42"));
    assert(!strcmp(lv_label_get_text(g_home_music.duration_label),"03:24"));
    assert(!lv_obj_has_flag(g_home_music.fade,LV_OBJ_FLAG_HIDDEN));
    assert(lv_bar_get_value(g_home_music.progress)==205);
    review_capture(display,"music-home-playing","桌面音乐播放信息");
    music_test_play_center(display,true);
    music_app_click(g_home_music.root,home_music_toggle);
    struct music_now_playing after;glass_music_now_playing(&after);assert(after.state==MUSIC_PAUSED);
    assert(strstr(lv_label_get_text(g_home_music.status),"已暂停"));
    review_capture(display,"music-home-paused","桌面音乐暂停与居中播放图标");
    music_test_play_center(display,false);
    music_app_click(g_home_music.root,glass_music);
    assert(!g_music_ui.return_apps&&!strcmp(lv_label_get_text(g_music_ui.title),now.name));
    assert(g_music_ui.cover.canvas&&g_music_ui.cover.thumbnail);
    assert(lv_image_get_src(g_music_ui.cover.thumbnail)==lv_canvas_get_image(g_music_ui.cover.canvas));
    music_app_click(g_music_ui.root,music_ui_toggle);
    glass_music_now_playing(&after);assert(after.state==MUSIC_PLAYING);
    music_app_click(g_music_ui.root,music_ui_back);glass_update(NULL);
    music_test_play_center(display,true);

    glass_apps(NULL);lv_obj_update_layout(g_desktop.screen);
    assert(review_find(g_apps_grid,"音乐"));
    assert(review_callback_find(g_apps_grid,glass_music));
    review_capture(display,"music-app-launcher","音乐已注册到全部应用");
    music_app_click(g_apps_grid,glass_music);
    assert(g_music_ui.return_apps&&!g_desktop.apps_page);
    assert(g_music_ui.cover.thumbnail);
    assert(!strcmp(lv_label_get_text(g_music_ui.title),now.name));
    assert(!strcmp(lv_label_get_text(g_music_ui.artist),now.artist));
    assert(!strcmp(lv_label_get_text(g_music_ui.album),now.album));
    review_capture(display,"music-app-playing","音乐应用播放与曲目信息");
    music_ui_configure(NULL);assert(portal_page&&!g_music_ui.root);
    portal_go_back(NULL);assert(g_music_ui.root&&g_music_ui.return_apps);
    music_app_click(g_music_ui.root,music_full_open);assert(g_music_ui.full);
    assert(g_music_ui.full->cover.canvas);
    review_capture(display,"music-full-current-cover","全屏播放器的当前封面");
    music_full_close(NULL);music_app_click(g_music_ui.root,music_ui_back);
    assert(g_desktop.apps_page&&!g_music_ui.root&&!g_music_ui.timer);
    glass_music_now_playing(&after);assert(after.state==MUSIC_PLAYING);
    music_app_click(g_apps_grid,glass_music);
    g_edge_page=g_desktop.current_card;glass_edge_back_async(NULL);
    assert(g_desktop.apps_page&&!g_music_ui.root);
    panel_hide(NULL);glass_update(NULL);

    /* Same URL on a new track retires the previous pixels immediately. */
    now.track_id++;now.revision++;review_cover_fixture(NULL);review_music_now_fixture(&now);glass_update(NULL);
    assert(!g_home_music.cover.canvas&&g_home_music.cover.requested);
    music_app_click(g_home_music.root,glass_music);
    assert(!g_home_music.cover.requested&&g_music_ui.cover.requested);
    glass_rebuild();assert(!g_home_music.cover.requested); /* Covered home must not fetch. */
    review_cover_fixture(cover_path);music_ui_refresh(NULL);
    assert(g_music_ui.cover.canvas&&g_music_ui.cover.thumbnail);
    struct music_now_playing replacement=now;replacement.track_id++;replacement.revision++;replacement.cover_url[0]=0;
    review_music_now_fixture(&replacement);music_ui_refresh(NULL);
    assert(!g_music_ui.cover.canvas&&!g_music_ui.cover.thumbnail);
    review_music_now_fixture(&now);music_ui_refresh(NULL);
    assert(g_music_ui.cover.canvas&&g_music_ui.cover.thumbnail);
    music_app_click(g_music_ui.root,music_ui_back);glass_update(NULL);
    assert(g_home_music.cover.canvas);
    now.cover_url[0]=0;now.track_id++;now.revision++;
    review_music_now_fixture(&now);glass_update(NULL);assert(!g_home_music.cover.canvas);
    review_capture(display,"music-home-no-cover","当前歌曲无封面时的占位");
    /* Invalid decoder output must not divide by zero or retain old artwork. */
    char invalid_cover[1024];snprintf(invalid_cover,sizeof(invalid_cover),"%s/zero-cover.rgb565",review_out);
    FILE *empty=fopen(invalid_cover,"wb");assert(empty);uint32_t zero[2]={0,0};
    assert(fwrite(zero,4,2,empty)==2);fclose(empty);
    review_cover_fixture(invalid_cover);strcpy(now.cover_url,"https://fixture/broken-cover.jpg");
    review_music_now_fixture(&now);glass_update(NULL);assert(!g_home_music.cover.canvas&&g_home_music.cover.done);
    now.cover_url[0]=0;review_cover_fixture(cover_path);review_music_now_fixture(&now);glass_update(NULL);
    music_app_click(g_home_music.root,home_music_next);
    glass_music_now_playing(&after);assert(after.track_id==now.track_id+1);

    /* Service-driven changes arrive without reopening home, including empty
     * artist, unknown duration, overflow progress and cleared playback. */
    strcpy(now.name,"一首很长很长的歌名，用于检查桌面卡片的文字边界与省略显示");
    now.artist[0]=0;now.state=MUSIC_LOADING;now.position_ms=0;now.duration_ms=0;now.revision++;
    review_music_now_fixture(&now);glass_update(NULL);
    assert(!strcmp(lv_label_get_text(g_home_music.artist),"未知歌手"));
    assert(!strcmp(lv_label_get_text(g_home_music.duration_label),"--:--"));
    review_capture(display,"music-home-long-title","长歌名与加载状态");
    now.state=MUSIC_ERROR;review_music_now_fixture(&now);glass_update(NULL);
    assert(strstr(lv_label_get_text(g_home_music.status),"播放失败"));
    now.state=MUSIC_ENDED;now.position_ms=250000;now.duration_ms=204000;
    review_music_now_fixture(&now);glass_update(NULL);assert(lv_bar_get_value(g_home_music.progress)==1000);
    memset(&now,0,sizeof(now));review_music_now_fixture(&now);glass_update(NULL);
    assert(!strcmp(lv_label_get_text(g_home_music.artist),"搜索好歌，收藏你的热爱"));
    assert(lv_bar_get_value(g_home_music.progress)==0);
    for(unsigned round=0;round<30;round++){
      glass_apps(NULL);music_app_click(g_apps_grid,glass_music);
      assert(g_music_ui.snapshot);
      if(!round){
        struct music_snapshot *saved=g_music_ui.snapshot;
        lv_obj_t *root=g_music_ui.root;lv_timer_t *timer=g_music_ui.timer;
        music_snapshot_fail_bytes=sizeof(*saved);
        glass_music_app(root,true);
        music_snapshot_fail_bytes=0;
        assert(g_music_ui.root==root&&g_music_ui.snapshot==saved&&g_music_ui.timer==timer);
      }
      music_app_click(g_music_ui.root,music_ui_back);assert(g_desktop.apps_page&&!g_music_ui.timer&&!g_music_ui.snapshot);
      panel_hide(NULL);glass_rebuild();assert(g_home_music.root&&g_home_music.valid);
    }
  }
  lv_obj_delete(g_glass.home);g_glass.home=NULL;assert(!g_home_music.root);
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("MUSIC_APP_OK: both themes; registered launcher; shared transport and metadata; current artwork and borrowed mini-player thumbnail; late cover with unchanged revision; same-URL track replacement; no-cover and invalid-size fallback; home/foreground owner isolation; deferred hidden-home fetch; cover-tap launch; next track; fullscreen; return; 60 reopen/rebuild cycles and image/timer cleanup");
  return 0;
}
