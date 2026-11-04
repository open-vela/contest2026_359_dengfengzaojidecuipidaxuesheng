/* SPDX-License-Identifier: Apache-2.0 */
#define main all_ui_reference_main
#include "render_all_ui.c"
#undef main

int main(int argc,char **argv)
{
  assert(argc==3);review_out=argv[2];
  FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);size_t size=ftell(f);rewind(f);
  void *font=malloc(size);assert(font&&fread(font,1,size,f)==size);fclose(f);
  lv_init();lv_display_t *display=lv_display_create(1024,600);
  lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(display,review_fb,NULL,sizeof(review_fb),LV_DISPLAY_RENDER_MODE_FULL);
  lv_display_set_flush_cb(display,review_flush);
  g_desktop.font16=lv_tiny_ttf_create_data(font,size,16);
  g_desktop.font20=lv_tiny_ttf_create_data(font,size,20);
  g_desktop.font28=lv_tiny_ttf_create_data(font,size,28);
  g_desktop.font16->fallback=glass_icon_font(16);g_desktop.font20->fallback=glass_icon_font(20);
  g_desktop.font28->fallback=glass_icon_font(24);
  g_glass.font48=lv_tiny_ttf_create_data(font,size,48);glass_home_fonts_init(font,size);
  g_glass.reduced_motion=true;g_glass.temperature_c=-999;g_glass.humidity_pct=-1;g_glass.pm25=-1;
  g_desktop.screen=glass_box(lv_screen_active(),0,32,1024,568,0,0);
  g_desktop.panel=glass_box(g_desktop.screen,0,0,1024,568,0,0);
  lv_obj_add_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN);glass_status_create();
  char path[1024];snprintf(path,sizeof(path),"%s/screens.json",review_out);
  review_manifest=fopen(path,"w");assert(review_manifest);fputs("[\n",review_manifest);
  for(unsigned theme=0;theme<2;theme++){
    g_desktop.light_theme=theme==0;glass_rebuild();
    for(unsigned cycle=0;cycle<30;cycle++){
      launch_builtin_dafeiyu_qapp(NULL);
      assert(!qpk_review_eval("const fixturePet=createPet();let fixtureMaid=false;Object.assign(system.pet,fixturePet,{getState(){return Object.assign(fixturePet.getState(),{appearance:fixtureMaid?'seagull':'fallback'});},loadAppearance(){fixtureMaid=true;return true;}});Dafeiyu.tick()"));
      unsigned widgets=qpk_runtime_widget_count();assert(widgets>20&&widgets<100);
      review_click("跟随");
      assert(!qpk_review_eval("if(Dafeiyu.getState().mode!=='follow')throw Error('mode')"));
      if(!cycle)review_capture(display,"dafeiyu-company","海鸥 · 陪伴");
      review_click("喂食");review_click("薯条");
      assert(!qpk_review_eval("if(Dafeiyu.getState().hunger>=28)throw Error('feed')"));
      if(!cycle)review_capture(display,"dafeiyu-food","海鸥 · 喂食");
      review_click("外观");review_click("恢复海鸥形象");
      assert(!qpk_review_eval("if(Dafeiyu.getState().appearance!=='seagull')throw Error('seagull')"));
      if(!cycle)review_capture(display,"dafeiyu-appearance","海鸥 · 外观（服务夹具）");
      review_click("设置");review_click("大");review_click("隐藏桌宠");
      assert(!qpk_review_eval("if(Dafeiyu.getState().visible||Dafeiyu.getState().size!=='large')throw Error('settings')"));
      if(!cycle)review_capture(display,"dafeiyu-settings","海鸥 · 设置");
      assert(qpk_runtime_widget_count()==widgets);
      panel_hide(NULL);assert(!qpk_runtime_running());
      assert(lv_obj_get_child_count(g_desktop.panel)==0);
    }
  }
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("DAFEIYU_UI_OK: real LVGL/QuickJS, both themes, all four sections, 60 reopen cycles");
}
