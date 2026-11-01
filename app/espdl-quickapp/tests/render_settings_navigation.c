/* Shared production settings navigation, page lifetime and layout coverage. */
#define main all_ui_reference_main
#include "render_all_ui.c"
#undef main

static void settle(void)
{for(unsigned i=0;i<8;i++){lv_tick_inc(101);lv_timer_handler();}lv_obj_update_layout(g_desktop.screen);}
static const char *sections[]={"控制面板","Wi-Fi 网络","热点配网","蓝牙设备","系统性能","扫码配置","关于设备"};
static unsigned nav_count(lv_obj_t *root,const char *text)
{
  if(!root||!lv_obj_is_visible(root))return 0;
  unsigned count=0;
  bool action=lv_obj_has_state(root,LV_STATE_CHECKED);
  for(unsigned i=0;i<lv_obj_get_event_count(root);i++)
    action|=lv_event_dsc_get_cb(lv_obj_get_event_dsc(root,i))==glass_system_page_open;
  if(action){
    for(unsigned i=0;i<lv_obj_get_child_count(root);i++){
      lv_obj_t *child=lv_obj_get_child(root,i);
      if(lv_obj_has_class(child,&lv_label_class)&&!strcmp(lv_label_get_text(child),text)){count++;break;}
    }
  }
  for(unsigned i=0;i<lv_obj_get_child_count(root);i++)count+=nav_count(lv_obj_get_child(root,i),text);
  return count;
}
static void check_sidebar(unsigned selected)
{
  settle();unsigned checked=0;
  for(unsigned i=0;i<7;i++){
    assert(nav_count(g_desktop.current_card,sections[i])==1);
    lv_obj_t *row=NULL;
    for(unsigned n=0;n<lv_obj_get_child_count(g_desktop.current_card);n++){
      lv_obj_t *child=lv_obj_get_child(g_desktop.current_card,n);
      if(lv_obj_get_style_width(child,0)==140&&review_find(child,sections[i])){row=child;break;}
    }
    assert(row);
    assert(lv_obj_get_x(row)==12&&lv_obj_get_y(row)==80+(int)i*48);
    assert(lv_obj_get_width(row)==140&&lv_obj_get_height(row)==44);
    assert(lv_obj_has_state(row,LV_STATE_CHECKED)==(i==selected));
    checked+=lv_obj_has_state(row,LV_STATE_CHECKED);
  }
  assert(checked==1);
  assert(!review_find(g_desktop.current_card,"存储与文件"));
  assert(!review_find(g_desktop.current_card,"无线触摸板"));
}
static bool feedback_active(const char *glyph)
{
  lv_obj_t *icon=glass_settings_find_symbol(g_desktop.current_card,glyph);
  assert(icon);return lv_anim_get(icon,NULL)!=NULL;
}
static void feedback_step(void)
{lv_tick_inc(40);lv_timer_handler();lv_obj_update_layout(g_desktop.screen);}
static void check_action_feedback(lv_display_t *display)
{
  g_glass.reduced_motion=false;glass_motion_set_reduced(false);
  glass_settings(NULL);feedback_step();
  assert(!feedback_active(GLASS_ICON_SLIDERS)&&!feedback_active(GLASS_ICON_SUN));
  assert(!feedback_active(LV_SYMBOL_VOLUME_MAX)&&!feedback_active(GLASS_ICON_SPARKLES));
  settle();
  lv_obj_send_event(g_glass.speaker_slider,LV_EVENT_HOVER_OVER,NULL);
  assert(!feedback_active(LV_SYMBOL_VOLUME_MAX));
  lv_slider_set_value(g_glass.speaker_slider,65,LV_ANIM_OFF);
  lv_obj_send_event(g_glass.speaker_slider,LV_EVENT_VALUE_CHANGED,NULL);
  assert(!feedback_active(LV_SYMBOL_VOLUME_MAX)&&!feedback_active(GLASS_ICON_SUN));
  feedback_step();review_capture(display,"settings-volume-feedback","调整音量触发声波");settle();
  lv_slider_set_value(g_glass.brightness_slider,70,LV_ANIM_OFF);
  lv_obj_send_event(g_glass.brightness_slider,LV_EVENT_VALUE_CHANGED,NULL);
  assert(!feedback_active(GLASS_ICON_SUN)&&!feedback_active(LV_SYMBOL_VOLUME_MAX));
  feedback_step();review_capture(display,"settings-brightness-feedback","调整亮度触发光芒");settle();
  lv_obj_send_event(g_glass.widget_dropdown,LV_EVENT_VALUE_CHANGED,NULL);
  assert(!feedback_active(GLASS_ICON_SPARKLES)&&!feedback_active(GLASS_ICON_SUN));settle();
  lv_obj_t *old=g_desktop.current_card;
  lv_obj_send_event(g_glass.settings_switch,LV_EVENT_VALUE_CHANGED,NULL);
  assert(g_desktop.settings_rebuild_pending);feedback_step();
  assert(old!=g_desktop.current_card&&!feedback_active(GLASS_ICON_SPARKLES));
  assert(!feedback_active(GLASS_ICON_SLIDERS)&&!feedback_active(GLASS_ICON_SUN));settle();
  review_click("蓝牙设备");assert(g_system_page_pending);feedback_step();
  assert(!feedback_active(LV_SYMBOL_BLUETOOTH)&&!feedback_active(GLASS_ICON_SLIDERS));settle();
  settings_clicked(NULL);feedback_step();assert(!feedback_active(GLASS_ICON_SLIDERS));settle();
  g_glass.reduced_motion=true;glass_motion_set_reduced(true);
  lv_obj_send_event(g_glass.brightness_slider,LV_EVENT_VALUE_CHANGED,NULL);
  lv_obj_send_event(g_glass.speaker_slider,LV_EVENT_VALUE_CHANGED,NULL);
  assert(!feedback_active(GLASS_ICON_SUN)&&!feedback_active(LV_SYMBOL_VOLUME_MAX));
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
  check_action_feedback(display);
  for(unsigned theme=0;theme<2;theme++){
    g_desktop.light_theme=theme!=0;glass_rebuild();settle();glass_settings(NULL);
    for(unsigned i=0;i<7;i++){
      if(i){review_click(sections[i]);assert(g_system_page_pending);settle();}
      check_sidebar(i);
      if(i!=0)assert(!g_control_card);
      char id[64];snprintf(id,sizeof(id),"settings-section-%u",i);review_capture(display,id,sections[i]);
      if(i==4){
        lv_obj_send_event(g_resources.tiles[1],LV_EVENT_CLICKED,NULL);assert(g_resources.selected==1);
        lv_obj_send_event(g_resources.tiles[0],LV_EVENT_CLICKED,NULL);assert(g_resources.selected==0);
      }
      lv_obj_t *owner=g_desktop.current_card;
      for(unsigned n=0;n<lv_obj_get_child_count(owner);n++){
        lv_obj_t *child=lv_obj_get_child(owner,n);
        if(lv_obj_has_state(child,LV_STATE_CHECKED)){lv_obj_send_event(child,LV_EVENT_CLICKED,NULL);break;}
      }
      settle();
      assert(owner==g_desktop.current_card&&!g_system_page_pending);
    }
    // Repeated sibling navigation retires all previous page widgets/timers.
    for(unsigned n=0;n<10;n++){
      review_click("Wi-Fi 网络");settle();assert(g_wifi_timer&&!g_mouse_timer&&!portal_timer&&!g_resources.timer);
      review_click("蓝牙设备");settle();assert(g_ble_list&&!g_wifi_timer&&!g_files_shell);
      review_click("控制面板");settle();check_sidebar(0);assert(!g_ble_list);
    }
    review_click("返回桌面");settle();assert(!g_desktop.current_card);
    for(unsigned origin=0;origin<2;origin++){
      if(origin){glass_apps(NULL);glass_apps_page(NULL,1);settle();review_click("文件管理");}
      else glass_files_open(NULL);
      settle();assert(g_files_shell&&g_files_return_apps==(origin!=0));
      assert(!review_find(g_files_shell,"Wi-Fi 网络"));
      assert(review_find(g_files_shell,"设备存储")&&review_find(g_files_shell,"SD 卡"));
      assert(lv_obj_get_width(g_files_browser.list)==784);
      for(unsigned n=0;n<GLASS_FILE_PAGE_SIZE;n++)if(g_files_browser.tiles[n].button){
        lv_area_t bounds;lv_obj_get_coords(g_files_browser.tiles[n].button,&bounds);
        assert(bounds.x1>=216&&bounds.x2<1000);
      }
      review_capture(display,origin?"standalone-files-apps":"standalone-files-home","独立文件管理");
      g_desktop.page_back(NULL);settle();assert(!g_files_shell);
      assert(origin?g_desktop.apps_page:!g_desktop.current_card);
      if(origin){g_apps_page_index=0;glass_apps(NULL);settle();review_click("无线触摸板");}
      else glass_mouse_open(NULL);
      settle();assert(g_mouse_status&&g_mouse_return_apps==(origin!=0));
      assert(!review_find(g_desktop.current_card,"Wi-Fi 网络"));
      assert(review_find(g_desktop.current_card,"触摸操作"));
      review_capture(display,origin?"standalone-touchpad-apps":"standalone-touchpad-home","独立无线触摸板");
      g_desktop.page_back(NULL);settle();assert(!g_mouse_status);
      assert(origin?g_desktop.apps_page:!g_desktop.current_card);
    }
    panel_hide(NULL);
  }
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("SETTINGS_NAVIGATION_OK: static system icons after sliders, clicks, rebuild/navigation and reduced motion; both themes; seven sections; 20 sibling cycles and cleanup");
}
