/* Real LVGL pointer dispatch and production navigation/QuickJS callbacks. */
#define main all_ui_reference_main
#include "render_all_ui.c"
#undef main

static lv_indev_t *input;
static lv_point_t pointer;
static bool pressed;
static void read_pointer(lv_indev_t *i,lv_indev_data_t *d)
{(void)i;d->point=pointer;d->state=pressed?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;}
static void drain(void)
{for(unsigned i=0;i<5;i++){lv_tick_inc(25);lv_timer_handler();}lv_obj_update_layout(g_desktop.screen);lv_obj_update_layout(lv_layer_top());lv_obj_update_layout(lv_layer_sys());}
static void settle_album(void)
{for(unsigned i=0;i<18;i++){lv_tick_inc(101);lv_timer_handler();}drain();}
static void sample(int x,int y,bool down)
{pointer=(lv_point_t){x,y};pressed=down;lv_tick_inc(20);lv_indev_read(input);}
static void swipe(unsigned side)
{
  int x=side==0?8:side==1?1016:512,y=side==2?592:300;
  lv_obj_update_layout(lv_layer_top());lv_obj_update_layout(lv_layer_sys());lv_obj_update_layout(g_desktop.screen);
  sample(x,y,true);sample(x+(side==0?96:side==1?-96:0),y-(side==2?108:0),true);
  assert(g_edge_back_pending);
  // The owner is not deleted from inside the pointer callback.
  assert(g_desktop.current_card==g_edge_page);
  sample(x+(side==0?160:side==1?-160:0),y-(side==2?160:0),true);
  sample(x,y,false);drain();assert(!g_edge_back_pending);
}
static void launch_stack(void)
{
  glass_apps(NULL);
  const char *js="globalThis.depth=2;ui.onBack(function(){if(depth>0){depth--;return true;}return false;});";
  lv_obj_t *p=qapp_page_prepare("Navigation fixture");assert(p);
  assert(!qpk_runtime_launch(p,"Fixture","gesture.fixture","1","builtin:/gesture.js",js,strlen(js),zh_font,show_toast,qpk_show_dialog,qpk_show_unresponsive));
  qapp_page_commit(p);g_desktop.qapp_return_apps=true;drain();
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
  g_desktop.screen=glass_box(lv_screen_active(),0,GLASS_STATUS_HEIGHT,1024,GLASS_CONTENT_HEIGHT,0,0);
  lv_obj_remove_flag(g_desktop.screen,LV_OBJ_FLAG_SCROLLABLE);
  g_desktop.panel=glass_box(g_desktop.screen,0,0,1024,GLASS_CONTENT_HEIGHT,0,0);
  lv_obj_add_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN);
  char path[1024];snprintf(path,sizeof(path),"%s/screens.json",review_out);
  review_manifest=fopen(path,"w");assert(review_manifest);fputs("[\n",review_manifest);
  glass_status_create();
  glass_edge_back_create();
  input=lv_indev_create();
  lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(input,read_pointer);
  lv_obj_update_layout(lv_layer_top());lv_obj_update_layout(lv_layer_sys());

  for(unsigned theme=0;theme<2;theme++){
    g_desktop.light_theme=theme!=0;glass_rebuild();drain();
    for(unsigned side=0;side<2;side++){
      review_open_settings_entry();review_click("Wi-Fi 网络");drain();
      assert(g_desktop.page_back==glass_settings);
      swipe(side);assert(g_desktop.current_card==g_control_card&&g_control_card);
      swipe(2);assert(!g_desktop.current_card&&lv_obj_has_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN));
    }
    review_open_settings_entry();about_clicked(NULL);drain();
    assert(g_desktop.page_back==glass_settings);
    swipe(1);assert(g_desktop.current_card==g_control_card);
    swipe(2);assert(!g_desktop.current_card);
    review_open_settings_entry();review_click("蓝牙设备");drain();
    assert(!review_find(g_desktop.current_card,"无线触摸板"));swipe(1);
    assert(g_desktop.current_card==g_control_card);swipe(2);
    glass_apps(NULL);glass_mouse_open(NULL);drain();
    swipe(0);assert(g_desktop.apps_page&&!g_mouse_status);
    glass_files_open(NULL);settle_album();assert(g_files_return_apps);
    swipe(1);assert(g_desktop.apps_page&&!g_files_shell);swipe(2);
    for(unsigned n=0;n<20;n++){
      launch_stack();lv_obj_t *owner=g_desktop.current_card;
      swipe(0);assert(g_desktop.current_card==owner&&qpk_runtime_running());
      assert(!qpk_review_eval("if(depth!==1)throw Error('back skipped a layer')"));
      swipe(1);assert(g_desktop.current_card==owner);
      assert(!qpk_review_eval("if(depth!==0)throw Error('right skipped a layer')"));
      swipe(0);assert(g_desktop.apps_page&&!qpk_runtime_running());
      launch_stack();
      assert(!qpk_review_eval("prompt.input({title:'input',value:'draft'},function(){});"));
      swipe(1);assert(qpk_runtime_running());
      assert(!qpk_review_eval("if(depth!==2)throw Error('input back also popped app')"));
      swipe(2);assert(!g_desktop.current_card&&!qpk_runtime_running());
    }
    launch_stack();
    assert(!qpk_review_eval("ui.onBack(function(){return true;});ui.onBack(function(){depth--;return true;});"));
    swipe(0);assert(!qpk_review_eval("if(depth!==1)throw Error('replaced callback ran twice')"));
    swipe(2);
    // A dialog is one level; home bypasses every transient.
    about_clicked(NULL);lv_obj_t *owner=g_desktop.current_card;
    qpk_show_dialog("dialog");assert(g_desktop.dialog);swipe(0);
    assert(g_desktop.current_card==owner&&!g_desktop.dialog);
    qpk_show_dialog("dialog");swipe(2);assert(!g_desktop.current_card&&!g_desktop.dialog);
    // Full-screen music must remain below a dismissed dialog.
    glass_music(NULL);owner=g_desktop.current_card;music_full_open(NULL);drain();
    assert(g_music_ui.full);qpk_show_dialog("music dialog");swipe(1);
    assert(!g_desktop.dialog&&g_music_ui.full&&g_desktop.current_card==owner);
    swipe(0);assert(!g_music_ui.full&&g_desktop.current_card==owner);
    music_full_open(NULL);drain();swipe(2);assert(!g_music_ui.full&&!g_desktop.current_card);
    // Native album overlays, viewers and folders each consume one back.
    extern void review_album_fixture(int);
    review_album_fixture(1);launch_local_video(NULL);settle_album();owner=g_desktop.current_card;
    review_click("晨光.jpg");settle_album();review_click("全屏");drain();
    assert(review_find(lv_layer_top(),"退出全屏"));swipe(0);
    assert(!review_find(lv_layer_top(),"退出全屏")&&g_desktop.current_card==owner);
    assert(review_find(g_desktop.panel,"返回相册"));swipe(1);
    assert(g_desktop.current_card==owner&&!review_find(g_desktop.panel,"返回相册"));
    review_click("相机");settle_album();swipe(0);settle_album();
    assert(g_desktop.current_card==owner&&review_find(g_desktop.panel,"相机"));
    review_click("视频");settle_album();review_click("海边短片.mp4");drain();
    review_click("全屏");drain();assert(review_find(lv_screen_active(),"退出全屏"));swipe(0);
    assert(!review_find(lv_screen_active(),"退出全屏")&&review_find(g_desktop.panel,"返回相册"));
    swipe(1);
    assert(g_desktop.current_card==owner&&!review_find(g_desktop.panel,"返回相册"));
    swipe(2);assert(!g_desktop.current_card);
    // A short, outward or cross-axis stroke cannot navigate.
    about_clicked(NULL);owner=g_desktop.current_card;
    lv_obj_set_style_translate_y(owner,7,0);drain();
    sample(512,592,true);sample(512,565,true);sample(512,565,false);drain();
    assert(g_desktop.current_card==owner&&lv_obj_get_style_translate_y(owner,0)==7);
    sample(512,592,true);sample(612,580,true);sample(612,470,true);sample(612,470,false);drain();
    assert(g_desktop.current_card==owner&&!g_edge_back_pending);
    sample(8,300,true);sample(0,300,true);sample(0,300,false);drain();assert(g_desktop.current_card==owner);
    // Replacing the watched page cancels its queued home operation.
    sample(512,592,true);sample(512,480,true);assert(g_edge_back_pending);
    glass_wifi_page(NULL);lv_obj_t *replacement=g_desktop.current_card;
    sample(512,480,false);drain();assert(g_desktop.current_card==replacement&&!g_edge_back_pending);
    swipe(2);assert(!g_desktop.current_card);
    // System strips disappear while locked and return after unlock.
    about_clicked(NULL);glass_lock_open(NULL);drain();
    for(unsigned i=0;i<3;i++)assert(lv_obj_has_flag(g_edge_strips[i],LV_OBJ_FLAG_HIDDEN));
    lock_unlock(NULL);drain();
    for(unsigned i=0;i<3;i++)assert(!lv_obj_has_flag(g_edge_strips[i],LV_OBJ_FLAG_HIDDEN));
    swipe(2);assert(!g_desktop.current_card);
    review_capture(display,"system-gestures","左右返回一级 · 底部上滑回桌面");
  }
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("SYSTEM_GESTURES_OK: real pointer/async dispatch; both themes; left/right pop one view; bottom home; 40 runtime cycles; input/dialog priority; music fullscreen; album fullscreen/photo/video/folders; callback replacement; cancellation and stale-owner rejection");
}
