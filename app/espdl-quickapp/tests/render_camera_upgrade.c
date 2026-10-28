/* SPDX-License-Identifier: Apache-2.0 */
#define main review_full_pages_main
#include "render_all_ui.c"
#undef main
extern double qpk_review_number(const char *source);
extern void qpk_review_root_area(lv_area_t *area);
static lv_point_t camera_point;
static lv_indev_state_t camera_pressed;
static unsigned camera_shutter_red(void)
{
  const uint8_t *p=review_fb+(300*1024+892)*2;
  return ((p[0]|(unsigned)p[1]<<8)>>11)&31;
}
static void camera_assert_frame(unsigned x,unsigned y,unsigned w,unsigned h)
{
  const unsigned corners[][2]={{x,y},{x+w-1,y},{x,y+h-1},{x+w-1,y+h-1}};
  for(unsigned i=0;i<4;i++){
    const uint8_t *p=review_fb+(corners[i][1]*1024+corners[i][0])*2;
    assert((p[0]|(unsigned)p[1]<<8)==0x630d); /* RGB565 #676268. */
  }
}
static void camera_assert_menu_icons(lv_obj_t *object)
{
  if(!object||!lv_obj_is_visible(object)||lv_obj_get_style_opa(object,0)==LV_OPA_TRANSP)return;
  if(lv_obj_has_class(object,&lv_label_class)){
    const unsigned char *text=(const unsigned char *)lv_label_get_text(object);
    assert(strlen((const char *)text)==3);
    unsigned code=(text[0]&15)<<12|(text[1]&63)<<6|(text[2]&63);
    assert(code>=0xe000&&code<=0xf8ff);
    lv_area_t a;lv_obj_get_coords(object,&a);assert(a.x1>=800);
  }
  for(unsigned i=0;i<lv_obj_get_child_count(object);i++)camera_assert_menu_icons(lv_obj_get_child(object,i));
}
static void camera_read(lv_indev_t *input,lv_indev_data_t *data)
{LV_UNUSED(input);data->point=camera_point;data->state=camera_pressed;}
static void camera_tick(lv_display_t *display)
{for(unsigned i=0;i<12;i++){if(qpk_runtime_running())assert(qpk_review_eval("__reviewTick()")==0);lv_tick_inc(20);lv_timer_handler();}lv_refr_now(display);}
static void camera_zoom_swipe(lv_indev_t *input,lv_display_t *display,int from,int to)
{
  camera_point=(lv_point_t){732,from};camera_pressed=LV_INDEV_STATE_PRESSED;lv_indev_read(input);lv_tick_inc(25);
  for(int i=1;i<=6;i++){camera_point.y=from+(to-from)*i/6;lv_indev_read(input);lv_tick_inc(25);}
  camera_pressed=LV_INDEV_STATE_RELEASED;lv_indev_read(input);camera_tick(display);
}
static void camera_tap(lv_indev_t *input,lv_display_t *display,int x,int y)
{
  camera_point=(lv_point_t){x,y};camera_pressed=LV_INDEV_STATE_PRESSED;lv_indev_read(input);lv_tick_inc(25);
  camera_pressed=LV_INDEV_STATE_RELEASED;lv_indev_read(input);camera_tick(display);
}
/* Address icon-only controls by their real QuickJS widget handles. LVGL may
 * temporarily replace long hidden captions with dots during layout. */
extern lv_obj_t *qpk_review_widget(int handle);
static lv_obj_t *camera_widget(const char *expression)
{
  lv_obj_t *widget=qpk_review_widget((int)qpk_review_number(expression));
  assert(widget&&!lv_obj_has_flag(widget,LV_OBJ_FLAG_HIDDEN));return widget;
}
static void camera_click(const char *expression)
{lv_obj_send_event(camera_widget(expression),LV_EVENT_CLICKED,NULL);}
static void camera_press_widget(lv_indev_t *input,const char *expression,lv_display_t *display)
{
  lv_obj_update_layout(g_desktop.screen);lv_area_t area;lv_obj_get_coords(camera_widget(expression),&area);
  camera_tap(input,display,(area.x1+area.x2)/2,(area.y1+area.y2)/2);
}
static void camera_press(lv_indev_t *input,const char *caption,lv_display_t *display)
{
  lv_obj_update_layout(g_desktop.screen);lv_obj_t *label=review_label_find(g_desktop.current_card,caption);assert(label);
  lv_area_t area;lv_obj_get_coords(label,&area);
  camera_tap(input,display,(area.x1+area.x2)/2,(area.y1+area.y2)/2);
}
int main(int argc,char **argv)
{
  assert(argc==3);review_out=argv[2];FILE *f=fopen(argv[1],"rb");assert(f);
  fseek(f,0,SEEK_END);size_t size=ftell(f);rewind(f);void *font=malloc(size);assert(font&&fread(font,1,size,f)==size);fclose(f);
  lv_init();lv_display_t *display=lv_display_create(1024,600);
  lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(display,review_fb,NULL,sizeof(review_fb),LV_DISPLAY_RENDER_MODE_FULL);lv_display_set_flush_cb(display,review_flush);
  g_desktop.font16=lv_tiny_ttf_create_data(font,size,16);g_desktop.font20=lv_tiny_ttf_create_data(font,size,20);g_desktop.font28=lv_tiny_ttf_create_data(font,size,28);
  g_desktop.font16->fallback=glass_icon_font(16);g_desktop.font20->fallback=glass_icon_font(20);g_desktop.font28->fallback=glass_icon_font(24);
  g_glass.font48=lv_tiny_ttf_create_data(font,size,48);glass_home_fonts_init(font,size);
  g_glass.reduced_motion=true;g_desktop.screen=glass_box(lv_screen_active(),0,32,1024,568,0,0);lv_obj_remove_flag(g_desktop.screen,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_text_font(g_desktop.screen,zh_font(20),0);
  g_desktop.panel=glass_box(g_desktop.screen,0,0,1024,600,0,0);lv_obj_add_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN);
  char path[1024];snprintf(path,sizeof(path),"%s/screens.json",review_out);review_manifest=fopen(path,"w");assert(review_manifest);fputs("[\n",review_manifest);
  for(unsigned theme=0;theme<2;theme++){
    panel_hide(NULL);g_desktop.light_theme=theme==0;glass_rebuild();
    lv_obj_set_pos(g_desktop.panel,0,0);lv_obj_set_height(g_desktop.panel,568);
    launch_builtin_camera_qapp(NULL);camera_tick(display);
    lv_area_t root;qpk_review_root_area(&root);assert(root.y1==0&&lv_area_get_height(&root)==600);
    assert(!g_desktop.statusbar||lv_obj_has_flag(g_desktop.statusbar,LV_OBJ_FLAG_HIDDEN));
    assert(!review_label_find(g_desktop.current_card,"相机"));
    review_capture(display,"camera-empty","相机 · 未连接设备");
    assert(camera_shutter_red()>0&&camera_shutter_red()<14);
    camera_click("deviceButton");camera_tick(display);
    camera_assert_menu_icons(g_desktop.current_card);
    review_capture(display,"camera-empty-menu","相机 · 无设备菜单");
    camera_click("doneButton");camera_tick(display);
    assert(qpk_review_eval("__devices=[{id:'csi:0',generation:1,name:'CSI 相机',transport:'csi',previewSupported:true}];CameraPreview.refresh()")==0);
    camera_click("deviceButton");camera_tick(display);
    assert(qpk_review_number("menuBounds.x")>=800);
    camera_assert_menu_icons(g_desktop.current_card);
    review_capture(display,"camera-single-menu","相机 · 单设备精简菜单");
    camera_assert_frame(24,103,672,393);
    assert(camera_shutter_red()>20);
    camera_click("doneButton");camera_tick(display);
    assert(qpk_review_eval("__devices=[];CameraPreview.refresh()")==0);
    assert(qpk_review_eval("__devices=Array.from({length:9},(_,i)=>({id:'usb:'+i,generation:1,name:'USB 相机 '+i,transport:'usb',previewSupported:true}));CameraPreview.refresh();system.camera.photoStatus=()=>({status:0,path:''});globalThis.__starts=0;system.camera.startDevice=()=>{__starts++;__cameraState={busy:true,error:0};return true;}")==0);
    camera_tick(display);review_capture(display,"camera-ready","相机 · 照片取景");
    camera_assert_frame(24,48,672,504);
    const char *aspect_names[]={"camera-aspect-1610","camera-aspect-169","camera-aspect-43","camera-aspect-square"};
    const unsigned aspect_heights[]={420,378,504,556};
    for(unsigned aspect=1;aspect<=4;aspect++){
      camera_click("aspectButton");camera_tick(display);
      assert(qpk_review_number("aspectRatio")==aspect);
      unsigned width=aspect==4?556:672,height=aspect_heights[aspect-1];
      camera_assert_frame(24+(672-width)/2,22+(556-height)/2,width,height);
      assert(camera_shutter_red()>20);
      review_capture(display,aspect_names[aspect-1],"相机 · 实际比例裁切");
    }
    assert(qpk_review_eval("system.camera.setAspect(0)")==0);camera_tick(display);
    assert(camera_shutter_red()>20);
    lv_indev_t *input=lv_indev_create();lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);lv_indev_set_display(input,display);lv_indev_set_read_cb(input,camera_read);
    camera_click("captureMode");camera_tick(display);assert(qpk_review_number("CameraPreview.video()"));
    review_capture(display,"camera-video","相机 · 录像含麦克风声音");
    camera_click("captureMode");camera_tick(display);assert(!qpk_review_number("CameraPreview.video()"));
    camera_tap(input,display,732,64);
    assert(qpk_review_number("CameraPreview.filters()")&&qpk_review_number("CameraPreview.style()")==0);
    assert(camera_shutter_red()>20);
    review_capture(display,"camera-filter-picker","相机 · 迷你色彩预览");
    camera_zoom_swipe(input,display,390,330);
    assert(qpk_review_number("CameraPreview.filters()")&&qpk_review_number("CameraPreview.zoom()")==100);
    camera_tap(input,display,500,300);
    assert(!qpk_review_number("CameraPreview.filters()")&&qpk_review_number("CameraPreview.style()")==0);
    camera_tap(input,display,732,64);camera_tap(input,display,800,192);
    assert(!qpk_review_number("CameraPreview.filters()")&&qpk_review_number("CameraPreview.style()")==1);
    review_capture(display,"camera-vivid","相机 · 鲜艳色彩风格");
    camera_tap(input,display,732,64);
    review_capture(display,"camera-filter-selected","相机 · 当前滤镜卡片");
    camera_tap(input,display,800,240);assert(qpk_review_number("CameraPreview.style()")==2);
    review_capture(display,"camera-mono","相机 · 黑白色彩风格");
    for(unsigned style=3;style<6;style++){
      camera_tap(input,display,732,64);
      camera_tap(input,display,800,144+style*48);
      assert(!qpk_review_number("CameraPreview.filters()")&&qpk_review_number("CameraPreview.style()")==style);
      assert(camera_shutter_red()>20);
    }
    camera_tap(input,display,732,64);camera_tap(input,display,800,144);
    assert(qpk_review_number("CameraPreview.style()")==0);
    camera_click("deviceButton");camera_tick(display);
    assert(qpk_review_number("CameraPreview.settings()"));
    camera_assert_menu_icons(g_desktop.current_card);
    review_capture(display,"camera-devices","相机 · 设备与取景设置");
    assert(camera_shutter_red()>20);
    const char *actions[]={"menuShutter","refreshButton","nextButton"};
    for(unsigned i=0;i<3;i++){
      lv_area_t a;lv_obj_get_coords(camera_widget(actions[i]),&a);
      assert(a.x1>=root.x1&&a.y1>=root.y1&&a.x2<=root.x2&&a.y2<=root.y2);
    }
    camera_zoom_swipe(input,display,390,330);
    assert(qpk_review_number("CameraPreview.settings()")&&qpk_review_number("CameraPreview.zoom()")==100);
    camera_tap(input,display,500,300);
    assert(!qpk_review_number("CameraPreview.settings()")&&qpk_review_number("__starts")==0);
    camera_tap(input,display,892,60);
    assert(qpk_review_number("CameraPreview.settings()"));
    camera_tap(input,display,892,60);
    assert(!qpk_review_number("CameraPreview.settings()")&&qpk_review_number("__starts")==0);
    camera_tap(input,display,892,60);
    assert(qpk_review_number("CameraPreview.settings()"));
    camera_press_widget(input,"fpsButton",display);assert(qpk_review_number("intervalIndex")==1);
    camera_press_widget(input,"fpsButton",display);assert(qpk_review_number("intervalIndex")==0);
    camera_press_widget(input,"nextButton",display);camera_press_widget(input,"nextButton",display);
    review_capture(display,"camera-page3","相机 · 更多设备");assert(qpk_review_number("__starts")==0);
    camera_press_widget(input,"rows[0]",display);
    assert(!qpk_review_number("CameraPreview.settings()"));
    assert(qpk_review_number("__starts")==0);
    review_capture(display,"camera-selected","相机 · 点选设备自动收起");
    camera_zoom_swipe(input,display,390,330);assert(qpk_review_number("CameraPreview.zoom()")==150);
    review_capture(display,"camera-zoom-15","相机 · 上滑选择 1.5 倍取景");
    camera_zoom_swipe(input,display,390,290);assert(qpk_review_number("CameraPreview.zoom()")==300);
    review_capture(display,"camera-zoom-3","相机 · 3 倍取景");
    camera_zoom_swipe(input,display,210,430);assert(qpk_review_number("CameraPreview.zoom()")==100);
    camera_tap(input,display,892,60);assert(qpk_review_number("CameraPreview.settings()"));
    assert(camera_shutter_red()>20);
    review_capture(display,"camera-menu-shutter","相机 · 无字设置保留大快门");
    camera_tap(input,display,892,300);
    assert(!qpk_review_number("CameraPreview.settings()")&&qpk_review_number("__starts")==1);
    assert(qpk_review_eval("CameraPreview.stop()")==0);camera_tick(display);assert(qpk_review_number("!__cameraState.busy"));
    camera_press_widget(input,"albumButton",display);desktop_camera_album_service();camera_tick(display);
    assert(!qpk_runtime_running());assert(review_label_find(g_desktop.current_card,"相册"));
    review_capture(display,"camera-album","相机 · 本机拍摄相册");
    camera_press(input,"camera-00000001.bmp",display);
    review_capture(display,"camera-album-preview","相机 · 预览拍摄内容");
    camera_press(input,"返回相册",display);assert(!qpk_runtime_running());
    camera_press(input,"返回相机",display);assert(qpk_runtime_running());
    camera_point=(lv_point_t){1008,300};camera_pressed=LV_INDEV_STATE_PRESSED;lv_indev_read(input);lv_tick_inc(25);
    camera_pressed=LV_INDEV_STATE_RELEASED;lv_indev_read(input);camera_tick(display);
    assert(!qpk_runtime_running());
    lv_indev_delete(input);
    assert(lv_obj_get_y(g_desktop.panel)==0&&lv_obj_get_height(g_desktop.panel)==568);
    assert(lv_obj_get_y(g_desktop.screen)==32&&lv_obj_get_height(g_desktop.screen)==568);
    assert(!g_desktop.statusbar||!lv_obj_has_flag(g_desktop.statusbar,LV_OBJ_FLAG_HIDDEN));
    puts("CAMERA_NATIVE_OK: fullscreen, removed controls, styles and recording state, paged cameras, pointer album preview and return, safe-area restored; both themes");
  }
  fputs("\n]\n",review_manifest);fclose(review_manifest);return 0;
}
