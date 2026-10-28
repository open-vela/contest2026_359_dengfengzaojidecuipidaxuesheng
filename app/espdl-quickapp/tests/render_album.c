/* SPDX-License-Identifier: Apache-2.0 */
#define UNIFIED_DEVICE_NO_MAIN
#include "render_unified_devices.c"
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
static lv_obj_t *album_review_pager(lv_obj_t *root)
{
  if(!root||!lv_obj_is_visible(root))return NULL;
  if(lv_obj_get_user_data(root)&&lv_obj_get_height(root)==40&&lv_obj_get_width(root)<=136)return root;
  for(unsigned i=0;i<lv_obj_get_child_count(root);i++){
    lv_obj_t *found=album_review_pager(lv_obj_get_child(root,i));if(found)return found;
  }
  return NULL;
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
  g_desktop.screen=glass_box(lv_screen_active(),0,GLASS_STATUS_HEIGHT,1024,GLASS_CONTENT_HEIGHT,0,0);lv_obj_remove_flag(g_desktop.screen,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_text_font(g_desktop.screen,zh_font(20),0);
  g_desktop.panel=glass_box(g_desktop.screen,0,0,1024,GLASS_CONTENT_HEIGHT,0x000000,0);lv_obj_add_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN);
  char path[1024];snprintf(path,sizeof(path),"%s/screens.json",review_out);review_manifest=fopen(path,"w");assert(review_manifest);fputs("[\n",review_manifest);




  g_glass.font72=lv_tiny_ttf_create_data(font,size,72);
  lv_indev_t *input=lv_indev_create();lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(input,touch_read);lv_indev_set_display(input,display);


  extern void review_album_fixture(int);
  glass_edge_back_create();glass_status_create();
  for(unsigned theme=0;theme<2;theme++) {
    g_desktop.light_theme=theme!=0;glass_rebuild();review_album_fixture(1);launch_local_video(NULL);
    for(unsigned i=0;i<18;i++){lv_tick_inc(101);lv_timer_handler();}
    glass_status_tick();review_capture(display,"album-grid","相册 · 照片与视频（离线合成图片）");
    review_click("照片");for(unsigned i=0;i<18;i++){lv_tick_inc(101);lv_timer_handler();}
    review_capture(display,"album-photos","相册 · 照片筛选");
    review_click("晨光.jpg");for(unsigned i=0;i<3;i++){lv_tick_inc(101);lv_timer_handler();}
    lv_obj_update_layout(g_desktop.screen);lv_obj_t *pager=album_review_pager(g_desktop.panel);assert(pager);
    review_capture(display,"album-photo","照片查看 · 比例适配");
    struct glass_pager *state=lv_obj_get_user_data(pager);assert(state->slots[1].hit);lv_obj_send_event(state->slots[1].hit,LV_EVENT_CLICKED,NULL);for(unsigned i=0;i<3;i++){lv_tick_inc(101);lv_timer_handler();}
    lv_obj_update_layout(g_desktop.screen);assert(review_label_find(g_desktop.panel,"山间.jpeg"));
    review_capture(display,"album-portrait","照片 · 竖图与奇数行宽");
    touch(input,720,350,true);touch(input,680,350,true);touch(input,600,350,true);touch(input,500,350,true);touch(input,400,350,true);touch(input,400,350,false);
    for(unsigned i=0;i<3;i++){lv_tick_inc(101);lv_timer_handler();}
    lv_obj_update_layout(g_desktop.screen);assert(review_label_find(g_desktop.panel,"落日.jpg"));
    review_click("全屏");lv_tick_inc(101);lv_timer_handler();review_capture(display,"album-fullscreen","照片 · 沉浸查看");
    lv_obj_t *close=review_find(lv_layer_top(),"退出全屏");assert(close);lv_obj_send_event(close,LV_EVENT_CLICKED,NULL);lv_tick_inc(101);lv_timer_handler();
    review_click("返回相册");for(unsigned i=0;i<18;i++){lv_tick_inc(101);lv_timer_handler();}
    review_click("视频");for(unsigned i=0;i<3;i++){lv_tick_inc(101);lv_timer_handler();}
    review_capture(display,"album-videos","相册 · 视频筛选");
    review_click("海边短片.mp4");lv_tick_inc(101);lv_timer_handler();review_capture(display,"album-video","视频查看 · 返回相册入口");
    review_click("返回相册");for(unsigned i=0;i<3;i++){lv_tick_inc(101);lv_timer_handler();}
    review_click("全部");for(unsigned i=0;i<18;i++){lv_tick_inc(101);lv_timer_handler();}
    pager=album_review_pager(g_desktop.panel);assert(pager);state=lv_obj_get_user_data(pager);lv_obj_send_event(state->slots[1].hit,LV_EVENT_CLICKED,NULL);for(unsigned i=0;i<18;i++){lv_tick_inc(101);lv_timer_handler();}review_capture(display,"album-page2","相册 · 分页加载");
    pager=album_review_pager(g_desktop.panel);assert(pager);state=lv_obj_get_user_data(pager);lv_obj_send_event(state->slots[0].hit,LV_EVENT_CLICKED,NULL);for(unsigned i=0;i<18;i++){lv_tick_inc(101);lv_timer_handler();}
    review_click("相机");for(unsigned i=0;i<18;i++){lv_tick_inc(101);lv_timer_handler();}review_capture(display,"album-folder","相册 · 子目录");
    review_click("上一级");for(unsigned i=0;i<18;i++){lv_tick_inc(101);lv_timer_handler();}
    review_click("本机");for(unsigned i=0;i<18;i++){lv_tick_inc(101);lv_timer_handler();}
    lv_obj_update_layout(g_desktop.screen);assert(review_find(g_desktop.panel,"camera-00000001.bmp"));
    review_capture(display,"album-camera","本机相机照片 · BMP 格式");
    review_click("SD 卡");for(unsigned i=0;i<18;i++){lv_tick_inc(101);lv_timer_handler();}
    review_album_fixture(2);review_click("刷新");for(unsigned i=0;i<3;i++){lv_tick_inc(101);lv_timer_handler();}review_capture(display,"album-empty","相册 · 空目录");
    review_album_fixture(3);review_click("刷新");for(unsigned i=0;i<3;i++){lv_tick_inc(101);lv_timer_handler();}review_capture(display,"album-no-sd","相册 · SD 卡未就绪");
    review_album_fixture(4);review_click("刷新");for(unsigned i=0;i<3;i++){lv_tick_inc(101);lv_timer_handler();}
    review_click("损坏.jpg");for(unsigned i=0;i<3;i++){lv_tick_inc(101);lv_timer_handler();}review_capture(display,"album-bad-photo","相册 · 图片读取失败");
    panel_hide(NULL);
    for(unsigned i=0;i<25;i++){review_album_fixture(1);launch_local_video(NULL);lv_tick_inc(101);lv_timer_handler();panel_hide(NULL);}
    local_album_open_path("/sdcard/../private.jpg");
  }
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("ALBUM_NATIVE_OK: both themes, real grid/viewer UI, filters, page navigation, folder/up and internal camera photos, photo next/swipe/fullscreen, video/back, empty/missing-SD/corrupt, 50 repeated reopen/cancel cycles");return 0;
}
