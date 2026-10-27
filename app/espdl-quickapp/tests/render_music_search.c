/* SPDX-License-Identifier: Apache-2.0 */
#define MUSIC_FULL_NO_MAIN
#include "render_music_full.c"
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
  g_desktop.screen=glass_box(lv_screen_active(),0,32,1024,568,0,0);lv_obj_remove_flag(g_desktop.screen,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_text_font(g_desktop.screen,zh_font(20),0);
  g_desktop.panel=glass_box(g_desktop.screen,0,0,1024,568,0x000000,0);lv_obj_add_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN);
  char path[1024];snprintf(path,sizeof(path),"%s/screens.json",review_out);review_manifest=fopen(path,"w");assert(review_manifest);fputs("[\n",review_manifest);




  g_glass.font72=lv_tiny_ttf_create_data(font,size,72);
  lv_indev_t *input=lv_indev_create();lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(input,touch_read);lv_indev_set_display(input,display);
  glass_edge_back_create();glass_status_create();

  for(unsigned theme=0;theme<2;theme++){
    g_desktop.light_theme=theme!=0;glass_rebuild();glass_status_tick();review_music_fixture(2);
    glass_music(NULL);lv_timer_pause(g_music_ui.timer);
    g_music_ui.favorites=false;g_music_ui.snapshot->has_result=false;g_music_ui.snapshot->searching=false;
    g_music_ui.snapshot->search_error=MUSIC_ERROR_KEY_REQUIRED;music_ui_list();lv_obj_update_layout(g_desktop.screen);
    assert(review_label_find(g_music_ui.list,"先配置音乐搜索服务"));
    assert(!review_label_find(g_music_ui.list,"暂时没有找到"));
    lv_obj_t *configure=review_callback_find(g_music_ui.list,music_ui_configure);assert(configure);
    review_capture(display,"music-search-key-required","音乐搜索 · 缺少密钥时显示配置入口");
    lv_obj_send_event(configure,LV_EVENT_CLICKED,NULL);lv_obj_update_layout(g_desktop.screen);
    assert(portal_page&&portal_return==glass_music);
    assert(review_label_find(portal_page,"返回音乐"));
    portal_go_back(NULL);assert(!portal_page&&g_music_ui.root);
    struct music_now_playing now;glass_music_now_playing(&now);assert(now.state==MUSIC_PLAYING);
    panel_hide(NULL);
  }
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("MUSIC_SEARCH_UI_OK: both themes; missing key distinguished from not found; explicit configuration and return to music; background playback preserved");return 0;
}
