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
    glass_chat_show(false);assert(chat_display.active);
    launch_builtin_qapp(NULL);
    lv_obj_t *page=g_desktop.current_card,*content=lv_obj_get_child(page,0);
    lv_obj_update_layout(g_desktop.screen);
    assert(lv_obj_get_y(content)==0 && lv_obj_get_x(content)==0);
    assert(lv_obj_get_height(content)==lv_obj_get_height(page)-56);
    assert(!chat_display.active);
    assert(!g_desktop.chat_page && g_desktop.qapp_page);
    assert(lv_obj_get_y(g_desktop.screen)==GLASS_STATUS_HEIGHT);
    assert(lv_obj_get_height(page)==GLASS_CONTENT_HEIGHT);
    review_capture(display,"chat-to-app","ESPClaw 切换快应用");
    panel_hide(NULL);
    /* Reject a staged app that creates no widgets without orphaning pages
     * or shrinking the surviving native conversation. */
    glass_chat_show(false);
    lv_obj_t *chat=g_desktop.current_card;
    unsigned children=lv_obj_get_child_count(g_desktop.panel);
    for(unsigned i=0;i<30;i++){
      content=qapp_page_prepare("取消启动");assert(content && !chat_display.active);
      const char *bad="globalThis.emptyAppFixture=true;";
      assert(qpk_runtime_launch(content,"fixture","fixture","1","app.js",bad,strlen(bad),
        zh_font,show_toast,qpk_show_dialog,qpk_show_unresponsive)==0);
      assert(qpk_runtime_widget_count()==0);
      qapp_page_discard(lv_obj_get_parent(content));
      lv_obj_update_layout(g_desktop.screen);
      assert(g_desktop.current_card==chat && g_chat_ui.root==chat && chat_display.active);
      assert(lv_obj_get_y(g_desktop.screen)==0 && lv_obj_get_height(g_desktop.screen)==600);
      assert(lv_obj_get_child_count(g_desktop.panel)==children && !qpk_runtime_running());
    }
    review_capture(display,"launch-rollback","启动失败保留 ESPClaw");
    panel_hide(NULL);
    content=qapp_page_prepare("取消首页启动");assert(content);
    qapp_page_discard(lv_obj_get_parent(content));
    assert(!g_desktop.current_card && lv_obj_has_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN));
    glass_chat_show(true);launch_builtin_qapp(NULL);
    assert(g_desktop.qapp_return_apps && !g_desktop.chat_page);
    qapp_back(NULL);assert(g_desktop.apps_page);panel_hide(NULL);
    glass_chat_show(false);launch_builtin_homeassistant_qapp(NULL);
    lv_obj_update_layout(g_desktop.screen);
    assert(lv_obj_get_height(g_desktop.current_card)==GLASS_CONTENT_HEIGHT);
    assert(!chat_display.active && !g_desktop.chat_page);
    assert(lv_obj_get_height(lv_obj_get_child(g_desktop.current_card,0))==GLASS_CONTENT_HEIGHT);
    review_capture(display,"chat-to-ha","ESPClaw 切换家居");panel_hide(NULL);
    glass_chat_show(false);launch_builtin_camera_qapp(NULL);
    lv_obj_update_layout(g_desktop.screen);
    assert(g_camera_page.page && !chat_display.active);
    assert(lv_obj_get_height(g_desktop.current_card)==600);
    assert(lv_obj_get_y(g_desktop.screen)==0);
    qapp_back(NULL);lv_obj_update_layout(g_desktop.screen);
    assert(!g_camera_page.page && lv_obj_get_y(g_desktop.screen)==GLASS_STATUS_HEIGHT);
    assert(lv_obj_get_height(g_desktop.screen)==GLASS_CONTENT_HEIGHT);
    assert(!lv_obj_has_flag(g_desktop.statusbar,LV_OBJ_FLAG_HIDDEN));
  }
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("SHELL_TRANSITIONS_OK: both themes; chat/app/HA/camera geometry; 60 empty-app rollbacks; home abort; return origin");
}
