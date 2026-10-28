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
  for(unsigned theme=0;theme<2;theme++){
    g_desktop.light_theme=theme!=0;glass_rebuild();
    review_capture(display,"home","当前桌面");
    about_clicked(NULL);check_full_page();
    assert(review_find(g_desktop.panel,"返回桌面"));
    lv_obj_t *info=lv_obj_get_child(g_desktop.current_card,-1);
    lv_obj_t *body=lv_obj_get_child(info,0);
    assert(lv_label_get_long_mode(body)==LV_LABEL_LONG_WRAP);
    assert(strstr(lv_label_get_text(body),"中文字体"));
    assert(lv_obj_get_height(body)>160 && lv_obj_get_y(body)+lv_obj_get_height(body)<lv_obj_get_height(info));
    review_capture(display,"about","关于设备");
    review_click("返回桌面");assert(lv_obj_has_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN));
    review_open_settings_entry();check_full_page();
    review_capture(display,"settings","设置");
    review_click("返回桌面");assert(g_control_close_pending);
    lv_async_call_cancel(glass_control_close_async,NULL);glass_control_close_async(NULL);
    assert(!g_control_card && lv_obj_has_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN));
    const char *entries[]={"Wi-Fi 网络","蓝牙设备","系统性能"};
    for(unsigned i=0;i<3;i++){
      review_open_settings_entry();review_click(entries[i]);assert(g_system_page_pending);
      lv_async_call_cancel(glass_system_page_async,NULL);glass_system_page_async(NULL);
      check_full_page();assert(!g_control_card);panel_hide(NULL);
    }
    for(unsigned i=0;i<30;i++){
      about_clicked(NULL);check_full_page();panel_hide(NULL);
      review_open_settings_entry();check_full_page();panel_hide(NULL);
    }
    g_glass.samples=0;glass_environment(NULL);check_full_page();
    assert(lv_obj_has_flag(g_glass.chart,LV_OBJ_FLAG_HIDDEN));
    review_capture(display,"environment-empty","环境趋势 · 暂无数据");
    glass_dashboard_publish(23,50,10,1,1);
    assert(!lv_obj_has_flag(g_glass.chart,LV_OBJ_FLAG_HIDDEN));
    assert(lv_chart_get_point_count(g_glass.chart)==1);
    glass_dashboard_publish(24,50,10,1,1);
    assert(lv_chart_get_point_count(g_glass.chart)==2);
    review_capture(display,"environment","环境趋势 · 有效样本");
    for(unsigned i=0;i<60;i++)glass_dashboard_publish(20+i%8,50,10,1,1);
    assert(lv_chart_get_point_count(g_glass.chart)==GLASS_HISTORY);
    panel_hide(NULL);
    glass_bluetooth_page(NULL);check_full_page();
    review_capture(display,"bluetooth","蓝牙");panel_hide(NULL);
    glass_apps(NULL);check_full_page();
    review_capture(display,"apps","全部应用");panel_hide(NULL);
  }
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("DESKTOP_CLEANUP_OK: both themes; full-page geometry; settings navigation; 60 about/settings roundtrips");
}
