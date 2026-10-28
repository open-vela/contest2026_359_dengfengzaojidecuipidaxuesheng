#define FILE_GESTURES_NO_MAIN
#include "render_file_gestures.c"
static void check_footer(lv_event_cb_t callback)
{
  settle();lv_obj_t *back=review_callback_find(g_desktop.current_card,callback);assert(back);
  lv_area_t a;lv_obj_get_coords(back,&a);
  assert(a.x1==12&&a.y1==544&&lv_obj_get_width(back)==140&&lv_obj_get_height(back)==44);
}
int main(int argc,char **argv)
{
  lv_display_t *display=review_pointer_init(argc,argv);
  lv_event_cb_t apps[]={glass_music,glass_files_open,glass_mouse_open,glass_chat_open,
    glass_chat_ball_open,launch_builtin_qapp,launch_builtin_2048_qapp,
    launch_builtin_recorder_qapp,launch_builtin_homeassistant_qapp,
    launch_builtin_camera_qapp,launch_builtin_espdl_qapp,launch_builtin_dafeiyu_qapp,
    launch_local_video,launch_bilibili};
  for(unsigned theme=0;theme<2;theme++){
    g_desktop.light_theme=theme!=0;glass_rebuild();
    for(unsigned origin=0;origin<2;origin++)for(unsigned i=0;i<sizeof(apps)/sizeof(apps[0]);i++){
      panel_hide(NULL);if(origin)glass_apps(NULL);apps[i](NULL);settle();
      lv_event_cb_t expected=g_desktop.app_open;assert(expected);
      assert(g_desktop.app_from_apps==(origin!=0));
      settings_clicked(NULL);settle();assert(glass_navigation_has_origin());check_footer(glass_settings_back);
      review_click("Wi-Fi 网络");settle();check_footer(glass_settings);
      g_desktop.page_back(NULL);settle();assert(g_desktop.settings_page&&glass_navigation_has_origin());
      review_click("系统性能");settle();g_desktop.page_back(NULL);settle();
      review_click("返回应用");settle();
      assert(g_desktop.current_card&&g_desktop.app_open==expected&&g_desktop.app_from_apps==(origin!=0));
      assert(!glass_navigation_has_origin());
    }
    panel_hide(NULL);lv_obj_t *content=qapp_page_prepare("框架");assert(content);settle();
    assert(lv_obj_get_x(content)==0&&lv_obj_get_y(content)==0);
    assert(lv_obj_get_height(content)==GLASS_CONTENT_HEIGHT-56&&lv_obj_get_width(content)==1024);
    qapp_page_commit(content);check_footer(qapp_back);panel_hide(NULL);
    launch_builtin_recorder_qapp(NULL);check_footer(qapp_back);panel_hide(NULL);
    launch_builtin_homeassistant_qapp(NULL);check_footer(qapp_back);panel_hide(NULL);
    launch_builtin_dafeiyu_qapp(NULL);check_footer(qapp_back);panel_hide(NULL);
    panel_hide(NULL);glass_music(NULL);check_footer(music_ui_back);
    music_ui_configure(NULL);check_footer(portal_go_back);portal_go_back(NULL);assert(g_music_ui.root);
    panel_hide(NULL);glass_chat_show(false);chat_ui_config(NULL);check_footer(portal_go_back);
    portal_go_back(NULL);assert(g_desktop.chat_page);check_footer(glass_chat_back);
    review_capture(display,"uniform-back","统一左下返回位置");
    panel_hide(NULL);glass_settings(NULL);check_footer(glass_settings_back);
    review_click("返回桌面");settle();assert(!g_desktop.current_card);
  }
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("NAVIGATION_CONTRACT_OK: two themes; 14 apps from home/apps; 56 settings return roundtrips through Wi-Fi/performance; common bottom-left footer and portal source restoration");
}
