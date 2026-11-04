/* Actual LVGL page + actual cached JPEG worker. No board or network. */
#include <stdlib.h>
#include <unistd.h>
static size_t artwork_fail_calloc_bytes;
static void *artwork_test_calloc(size_t n,size_t bytes)
{return artwork_fail_calloc_bytes&&n*bytes==artwork_fail_calloc_bytes?NULL:calloc(n,bytes);}
#define calloc artwork_test_calloc
#define MUSIC_FULL_NO_MAIN
#include "render_music_full.c"
#undef calloc
#include "glass_ui_probe_pointer.h"
extern unsigned review_artwork_http;
extern void review_music_library_count(unsigned);
static void artwork_prepare(const struct music_song *song)
{
  int ret=-EBUSY;
  for(unsigned i=0;i<5000&&ret==-EBUSY;i++){ret=glass_music_artwork_prepare(song,NULL,NULL);if(ret==-EBUSY)usleep(1000);}
  assert(!ret);
}
static bool artwork_row_visible(lv_obj_t *host)
{
 if(!host||!g_music_ui.list)return false;lv_area_t a,b;lv_obj_get_coords(host,&a);lv_obj_get_coords(g_music_ui.list,&b);return a.y2>=b.y1&&a.y1<=b.y2;
}

static unsigned artwork_visible(void)
{
  unsigned n=0;
  for(unsigned i=0;g_music_ui.artwork&&i<GLASS_COVER_THUMBNAIL_COUNT;i++)n+=artwork_row_visible(g_music_ui.artwork->views[i].host);
  return n;
}
static unsigned artwork_ready(void)
{
  unsigned n=0;
  for(unsigned i=0;g_music_ui.artwork&&i<GLASS_COVER_THUMBNAIL_COUNT;i++){
    struct music_cover_view *v=&g_music_ui.artwork->views[i];
    if(v->canvas){
      const lv_image_dsc_t *d=lv_canvas_get_image(v->canvas);
      assert(d&&d->data&&d->header.w&&d->header.w<=64&&d->header.h&&d->header.h<=64);if(artwork_row_visible(v->host))n++;
    }
  }
  return n;
}
static void artwork_poll(unsigned expected,bool full)
{
  for(unsigned i=0;i<10000;i++){
    lv_tick_inc(20);lv_timer_handler();music_ui_refresh(NULL);
    if(full?g_music_ui.full&&g_music_ui.full->cover.canvas:
      artwork_visible()&&artwork_ready()>=expected&&g_music_ui.cover.canvas&&g_music_ui.cover.thumbnail)return;
    usleep(1000);
  }
  fprintf(stderr,"artwork timeout visible=%u ready=%u expected=%u main=%d mini=%d full=%d\n",artwork_visible(),artwork_ready(),expected,
    !!g_music_ui.cover.canvas,!!g_music_ui.cover.thumbnail,g_music_ui.full&&g_music_ui.full->cover.canvas);
  for(unsigned i=0;i<GLASS_COVER_THUMBNAIL_COUNT;i++){
    struct music_cover_view *v=&g_music_ui.artwork->views[i];
    fprintf(stderr,"view%u owner=%u req=%d done=%d error=%d track=%u host=%p canvas=%p\n",i,v->owner,v->requested,v->done,v->error,v->track_id,(void *)v->host,(void *)v->canvas);
  }
  assert(false);
}
static void artwork_open(void)
{
  glass_music(NULL);assert(g_music_ui.root&&g_music_ui.artwork);
  g_music_ui.favorites=true;music_ui_list();music_ui_refresh(NULL);
}
static void artwork_close(void)
{panel_hide(NULL);assert(!g_music_ui.root&&!g_music_ui.artwork&&!g_music_ui.snapshot);}
static void artwork_scroll_preserves(int y)
{
 lv_obj_t *hosts[GLASS_COVER_THUMBNAIL_COUNT],*images[GLASS_COVER_THUMBNAIL_COUNT];unsigned count=0;
 for(unsigned i=0;i<GLASS_COVER_THUMBNAIL_COUNT;i++){
  struct music_cover_view *v=&g_music_ui.artwork->views[i];
  if(v->canvas&&artwork_row_visible(v->host)){hosts[count]=v->host;images[count++]=v->canvas;}
 }
 lv_obj_scroll_to_y(g_music_ui.list,y,LV_ANIM_OFF);music_ui_refresh(NULL);unsigned shared=0;
 for(unsigned j=0;j<count;j++)if(artwork_row_visible(hosts[j])){
  struct music_cover_view *found=NULL;
  for(unsigned i=0;i<GLASS_COVER_THUMBNAIL_COUNT;i++)if(g_music_ui.artwork->views[i].host==hosts[j])found=&g_music_ui.artwork->views[i];
  assert(found&&found->canvas==images[j]);shared++;
 }
 assert(shared);printf("MUSIC_SCROLL_SHARED_IMAGES_OK y=%d shared=%u\n",y,shared);
}
static void artwork_bottom_bounds(void)
{
  lv_obj_update_layout(g_music_ui.root);
  lv_area_t title,artist,bar;
  lv_obj_get_coords(g_music_ui.bottom_title,&title);lv_obj_get_coords(g_music_ui.bottom_artist,&artist);
  lv_obj_get_coords(lv_obj_get_parent(g_music_ui.bottom_title),&bar);
  assert(title.y2<artist.y1&&artist.y2<=bar.y2&&title.x2<=bar.x2);
}
static void artwork_stop_preserves(struct music_now_playing *now,bool full)
{
  lv_obj_t *image=full?g_music_ui.full->cover.canvas:g_music_ui.cover.canvas;
  lv_obj_t *mini=g_music_ui.cover.thumbnail;assert(image);
  now->state=MUSIC_STOPPED;now->track_id++;now->revision++;review_music_now_fixture(now);music_ui_refresh(NULL);
  assert(image==(full?g_music_ui.full->cover.canvas:g_music_ui.cover.canvas));
  if(!full)assert(mini==g_music_ui.cover.thumbnail);
  now->state=MUSIC_PLAYING;now->revision++;review_music_now_fixture(now);music_ui_refresh(NULL);
  puts("MUSIC_STOP_CANVAS_RETAINED_OK");
}
static void artwork_real_drag(void)
{
  lv_obj_scroll_to_y(g_music_ui.list,0,LV_ANIM_OFF);music_ui_refresh(NULL);artwork_poll(artwork_visible(),false);
  int before=lv_obj_get_scroll_y(g_music_ui.list);
  assert(desktop_ui_probe_pointer_stroke(450,430,450,285));
  int after=lv_obj_get_scroll_y(g_music_ui.list);
  assert(after>before+30);music_ui_refresh(NULL);artwork_poll(artwork_visible(),false);
  printf("MUSIC_NATIVE_POINTER_SCROLL_OK before=%d after=%d\n",before,after);
}
static void artwork_home_stop(struct music_now_playing *now)
{
  for(unsigned i=0;i<5000&&!g_home_music.cover.canvas;i++){
    home_music_refresh();lv_tick_inc(20);lv_timer_handler();usleep(1000);
  }
  assert(g_home_music.cover.canvas);lv_obj_t *image=g_home_music.cover.canvas;
  now->state=MUSIC_STOPPED;now->track_id++;now->revision++;review_music_now_fixture(now);home_music_refresh();
  assert(g_home_music.cover.canvas==image);
  now->state=MUSIC_PLAYING;now->revision++;review_music_now_fixture(now);home_music_refresh();
  puts("MUSIC_HOME_STOP_CANVAS_RETAINED_OK");
}

int main(int argc,char **argv)
{
  assert(argc==3);review_out=argv[2];
  FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);size_t size=ftell(f);rewind(f);
  void *font=malloc(size);assert(font&&fread(font,1,size,f)==size);fclose(f);
  char path[1024];snprintf(path,sizeof(path),"%s/large-cover.jpg",review_out);
  f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);size_t jpeg_size=ftell(f);rewind(f);
  unsigned char *jpeg=malloc(jpeg_size);assert(jpeg&&fread(jpeg,1,jpeg_size,f)==jpeg_size);fclose(f);assert(jpeg_size>2u*1024u*1024u);
  lv_init();lv_display_t *display=lv_display_create(1024,600);
  lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(display,review_fb,NULL,sizeof(review_fb),LV_DISPLAY_RENDER_MODE_FULL);
  lv_display_set_flush_cb(display,review_flush);
  g_desktop.font16=lv_tiny_ttf_create_data(font,size,16);g_desktop.font20=lv_tiny_ttf_create_data(font,size,20);
  g_desktop.font28=lv_tiny_ttf_create_data(font,size,28);
  g_desktop.font16->fallback=glass_icon_font(16);g_desktop.font20->fallback=glass_icon_font(20);g_desktop.font28->fallback=glass_icon_font(24);
  g_glass.font48=lv_tiny_ttf_create_data(font,size,48);glass_home_fonts_init(font,size);
  g_glass.reduced_motion=true;g_glass.temperature_c=-999;g_glass.humidity_pct=-1;g_glass.pm25=-1;
  g_desktop.screen=glass_box(lv_screen_active(),0,32,1024,568,0,0);
  lv_obj_remove_flag(g_desktop.screen,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_style_text_font(g_desktop.screen,zh_font(20),0);
  g_desktop.panel=glass_box(g_desktop.screen,0,0,1024,568,0,0);lv_obj_add_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN);
  snprintf(path,sizeof(path),"%s/screens.json",review_out);review_manifest=fopen(path,"w");assert(review_manifest);fputs("[\n",review_manifest);
  glass_edge_back_create();glass_status_create();review_music_library_count(5);review_music_fixture(2);
  struct music_snapshot *library=malloc(sizeof(*library));assert(library);glass_music_get(library);assert(library->favorite_count==5);
  struct music_now_playing now={.revision=70,.track_id=20,.state=MUSIC_PLAYING,.duration_ms=200000,.position_ms=40000};
  strcpy(now.name,library->favorites[0].name);strcpy(now.artist,library->favorites[0].artist);
  assert(!music_favorite_cover_url(&library->favorites[0],now.cover_url));review_music_now_fixture(&now);
  for(unsigned i=0;i<library->favorite_count;i++)if(i!=2){
    assert(music_favorite_cover_write(&library->favorites[i],jpeg,jpeg_size));
    artwork_prepare(&library->favorites[i]);
    char url[1024],original[256],backup[264];assert(!music_favorite_cover_url(&library->favorites[i],url));
    assert(!music_favorite_cover_path(url,original));snprintf(backup,sizeof(backup),"%s.saved",original);assert(!rename(original,backup));
  }
  for(unsigned theme=0;theme<2;theme++){
    g_desktop.light_theme=theme;glass_rebuild();artwork_open();
    artwork_poll(2,false);assert(artwork_visible()>=2);
    if(!theme){
      assert(music_favorite_cover_write(&library->favorites[2],jpeg,jpeg_size));
      artwork_prepare(&library->favorites[2]);
      char url[1024],original[256],backup[264];assert(!music_favorite_cover_url(&library->favorites[2],url));
      assert(!music_favorite_cover_path(url,original));snprintf(backup,sizeof(backup),"%s.saved",original);assert(!rename(original,backup));
      lv_tick_inc(5100);artwork_poll(artwork_visible(),false);
      assert(g_music_ui.snapshot->revision==now.revision);
    }
    artwork_poll(artwork_visible(),false);review_capture(display,"music-artwork-list","真实 JPEG 列表缩略图、主封面及迷你封面");
    struct glass_artwork_stats warm;glass_lock_artwork_stats(&warm);
    artwork_stop_preserves(&now,false);artwork_real_drag();
    strcpy(now.name,"一首很长很长的歌名，用于测试底栏单行省略");strcpy(now.artist,"很长很长的歌手姓名与乐队名称");now.revision++;
    review_music_now_fixture(&now);music_ui_refresh(NULL);artwork_bottom_bounds();
    review_capture(display,"music-bottom-long-title","长歌名和歌手单行省略不重叠");
    now.name[0]=now.artist[0]=0;now.revision++;review_music_now_fixture(&now);music_ui_refresh(NULL);artwork_bottom_bounds();
    review_capture(display,"music-bottom-idle","空闲底栏文字不重叠");
    strcpy(now.name,library->favorites[0].name);strcpy(now.artist,library->favorites[0].artist);now.revision++;
    review_music_now_fixture(&now);music_ui_refresh(NULL);artwork_bottom_bounds();
    puts("MUSIC_BOTTOM_SINGLE_LINE_OK: idle placeholders and long names do not overlap or overflow the mini player");
    artwork_scroll_preserves(180);artwork_poll(artwork_visible(),false);
    artwork_scroll_preserves(0);artwork_poll(artwork_visible(),false);
    artwork_scroll_preserves(280);artwork_poll(artwork_visible(),false);
    artwork_scroll_preserves(100);artwork_poll(artwork_visible(),false);
    assert(artwork_ready()==artwork_visible());review_capture(display,"music-artwork-scroll","滚动后列表封面");
    struct music_artwork_ui *old=g_music_ui.artwork;struct music_snapshot *snap=g_music_ui.snapshot;
    artwork_fail_calloc_bytes=sizeof(*old);glass_music_app(g_music_ui.root,false);artwork_fail_calloc_bytes=0;
    assert(g_music_ui.artwork==old&&g_music_ui.snapshot==snap);
    music_full_open(NULL);assert(g_music_ui.full);artwork_poll(0,true);
    artwork_stop_preserves(&now,true);
    review_capture(display,"music-artwork-full","真实 JPEG 全屏封面");
    lv_obj_t *retained[GLASS_COVER_THUMBNAIL_COUNT];
    for(unsigned i=0;i<GLASS_COVER_THUMBNAIL_COUNT;i++)retained[i]=g_music_ui.artwork->views[i].canvas;
    music_full_close(NULL);music_ui_refresh(NULL);
    for(unsigned i=0;i<GLASS_COVER_THUMBNAIL_COUNT;i++)assert(retained[i]==g_music_ui.artwork->views[i].canvas);
    puts("MUSIC_THUMBNAILS_RETAINED_OK: unchanged row images survive closing full player without blank reload");
    music_full_open(NULL);artwork_poll(0,true);
    lv_obj_t *old_full_root=g_music_ui.full->root;
    glass_music_app(g_music_ui.root,false);
    assert(!lv_obj_is_valid(old_full_root));
    g_music_ui.favorites=true;music_ui_list();artwork_poll(artwork_visible(),false);
    puts("MUSIC_ARTWORK_SAME_CARD_REPLACE_OK: full overlay, old workers, timers, row pixels and artwork heap retired before rebuilding same card");
    artwork_close();
    artwork_home_stop(&now);
    for(unsigned round=0;round<30;round++){
      artwork_open();
      if(round%3==0){lv_tick_inc(5100);artwork_poll(artwork_visible(),false);}
      else {music_ui_refresh(NULL);lv_tick_inc(20);}
      if(round%2==0){music_full_open(NULL);music_ui_refresh(NULL);}
      artwork_close();lv_tick_inc(20);lv_timer_handler();
    }
    struct glass_artwork_stats after;glass_lock_artwork_stats(&after);
    assert(after.jpeg_decodes==warm.jpeg_decodes&&after.cache_hits>warm.cache_hits);
    puts("MUSIC_WARM_UI_ZERO_JPEG_OK: real drags, home/main/mini/full, stops and repeated page returns use prepared assets with originals absent");
  }
  /* Drain the last cancelled real worker before removing its local fixtures. */
  for(unsigned i=0;i<1000;i++){
    int ret=glass_lock_cover_request(GLASS_COVER_THUMBNAIL_FIRST,now.cover_url);
    if(!ret){glass_lock_cover_cancel(GLASS_COVER_THUMBNAIL_FIRST);break;}
    assert(ret==-EBUSY);usleep(1000);
  }
  usleep(100000);assert(review_artwork_http==0);
  for(unsigned i=0;i<library->favorite_count;i++){
    char url[1024],original[256],backup[264];assert(!music_favorite_cover_url(&library->favorites[i],url));
    assert(!music_favorite_cover_path(url,original));snprintf(backup,sizeof(backup),"%s.saved",original);assert(!unlink(backup));
    music_favorite_cache_forget(&library->favorites[i]);
  }
  free(jpeg);free(library);fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("MUSIC_REAL_ARTWORK_UI_OK: actual >2MiB cached JPEG worker + LVGL, both themes, row/main/mini/full artwork, late pin without revision change, scroll, allocation failure preserves page, 60 reopen/close cycles including active-worker cancellation; zero HTTP");
  return 0;
}
