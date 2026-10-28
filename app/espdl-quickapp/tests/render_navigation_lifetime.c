/* SPDX-License-Identifier: Apache-2.0 */
#define main all_ui_reference_main
#include "render_all_ui.c"
#undef main

static void check_full_page(void)
{
  lv_obj_t *card=g_desktop.current_card;
  assert(card);
  lv_obj_update_layout(g_desktop.screen);
  assert(lv_obj_get_x(card)==0 && lv_obj_get_y(card)==0);
  assert(lv_obj_get_width(card)==1024);
  assert(lv_obj_get_height(card)==GLASS_CONTENT_HEIGHT);
  assert(lv_obj_get_style_bg_opa(g_desktop.panel,0)==LV_OPA_TRANSP);
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
  lv_obj_update_layout(lv_layer_top());
  for(unsigned theme=0;theme<2;theme++){
    g_desktop.light_theme=theme!=0;glass_rebuild();
    review_open_settings_entry();review_click("Wi-Fi 网络");assert(g_system_page_pending);
    about_clicked(NULL);lv_obj_t *replacement=g_desktop.current_card;
    lv_tick_inc(1);lv_timer_handler();lv_obj_update_layout(g_desktop.screen);lv_obj_update_layout(lv_layer_top());
    assert(g_desktop.current_card==replacement);
    assert(!g_system_page_pending);
    assert(review_label_find(g_desktop.panel,"关于设备"));
    review_capture(display,"cancel-navigation","切换页面取消旧导航");
    panel_hide(NULL);
    const char *names[]={"Wi-Fi 网络","蓝牙设备","系统性能"};
    for(unsigned i=0;i<3;i++){
      review_open_settings_entry();review_click(names[i]);assert(g_system_page_pending);
      lv_tick_inc(1);lv_timer_handler();lv_obj_update_layout(g_desktop.screen);lv_obj_update_layout(lv_layer_top());
      assert(!g_system_page_pending && !g_control_card && g_desktop.current_card);
      panel_hide(NULL);
      assert(!g_wifi_timer && !g_mouse_timer && !g_files_timer && !g_resources.timer);
    }
    for(unsigned i=0;i<30;i++){
      review_open_settings_entry();review_click(names[i%3]);
      about_clicked(NULL);replacement=g_desktop.current_card;
      lv_tick_inc(1);lv_timer_handler();lv_obj_update_layout(g_desktop.screen);lv_obj_update_layout(lv_layer_top());
      assert(g_desktop.current_card==replacement && !g_system_page_pending);
      assert(review_label_find(g_desktop.panel,"关于设备"));
      panel_hide(NULL);
    }
    /* Pending navigation from the home footer also belongs to that view. */
    lv_obj_update_layout(g_desktop.screen);
    lv_obj_t *files=review_callback_find(g_glass.home,glass_files_open);assert(files);
    lv_obj_send_event(files,LV_EVENT_CLICKED,NULL);assert(g_system_page_pending);
    glass_chat_show(false);replacement=g_desktop.current_card;
    lv_tick_inc(1);lv_timer_handler();lv_obj_update_layout(g_desktop.screen);lv_obj_update_layout(lv_layer_top());
    assert(g_desktop.current_card==replacement && chat_display.active && !g_system_page_pending);
    panel_hide(NULL);
    /* Cancel a file-browser refresh when its owner is replaced. */
    glass_storage_page(NULL);glass_files_refresh(NULL);assert(g_files_pending);
    about_clicked(NULL);replacement=g_desktop.current_card;
    lv_tick_inc(1);lv_timer_handler();lv_obj_update_layout(g_desktop.screen);lv_obj_update_layout(lv_layer_top());
    assert(g_desktop.current_card==replacement && !g_files_pending && !g_files_timer);
    /* Old settings-close callbacks must not close the replacement page. */
    review_open_settings_entry();review_click("返回桌面");assert(g_control_close_pending);
    about_clicked(NULL);replacement=g_desktop.current_card;
    lv_tick_inc(1);lv_timer_handler();lv_obj_update_layout(g_desktop.screen);lv_obj_update_layout(lv_layer_top());
    assert(g_desktop.current_card==replacement && !g_control_close_pending);
    glass_portal_open(NULL);assert(portal_timer);
    about_clicked(NULL);replacement=g_desktop.current_card;
    assert(!portal_timer && !portal_page && !portal_pixels);
    lv_tick_inc(1100);lv_timer_handler();assert(g_desktop.current_card==replacement);
    review_capture(display,"released-page-timers","退出页面释放定时器");panel_hide(NULL);
    /* Locking cancels both settings and system-page requests underneath it. */
    lv_obj_send_event(g_status_ui.island,LV_EVENT_CLICKED,NULL);
    assert(g_desktop.settings_rebuild_pending);
    glass_lock_open(NULL);assert(!g_desktop.settings_rebuild_pending);
    lv_tick_inc(1);lv_timer_handler();lv_obj_update_layout(g_desktop.screen);lv_obj_update_layout(lv_layer_top());assert(g_lock_ui.root && !g_desktop.current_card);
    lock_unlock(NULL);
    review_open_settings_entry();review_click("Wi-Fi 网络");replacement=g_desktop.current_card;
    glass_lock_open(NULL);assert(!g_system_page_pending);
    lv_tick_inc(1);lv_timer_handler();lv_obj_update_layout(g_desktop.screen);lv_obj_update_layout(lv_layer_top());assert(g_desktop.current_card==replacement && g_lock_ui.root);
    lock_page_set((g_lock_page+1)%4);assert(g_lock_page_pending);
    lv_obj_delete(g_lock_ui.root);assert(!g_lock_page_pending && !g_lock_ui.timer);
    unsigned previous_page=g_lock_page;glass_lock_open(NULL);
    lv_tick_inc(1);lv_timer_handler();lv_obj_update_layout(g_desktop.screen);lv_obj_update_layout(lv_layer_top());assert(g_lock_ui.root && g_lock_page==previous_page);
    review_capture(display,"lock-navigation","锁屏取消过期导航");
    lock_unlock(NULL);panel_hide(NULL);
  }
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("NAVIGATION_LIFETIME_OK: both themes; 60 cancelled switches; valid async entries; home/file refresh cancellation; page timer release; lock and settings-close ownership");
}
