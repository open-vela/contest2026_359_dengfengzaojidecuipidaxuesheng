#define main all_ui_reference_main
#include "render_all_ui.c"
#undef main
static unsigned verify_pager_below(lv_obj_t *o,int bottom){
 unsigned found=0;
 if(!lv_obj_has_flag(o,LV_OBJ_FLAG_HIDDEN)&&lv_obj_check_type(o,&lv_button_class)&&lv_obj_get_width(o)==24&&lv_obj_get_height(o)==30){lv_area_t a;lv_obj_get_coords(o,&a);assert(a.y1>=bottom+8);found++;}
 for(unsigned i=0;i<lv_obj_get_child_count(o);i++)found+=verify_pager_below(lv_obj_get_child(o,i),bottom);
 return found;
}
int main(int argc,char **argv){
  assert(argc==3);review_out=argv[2];
  FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);size_t size=ftell(f);rewind(f);
  void *font=malloc(size);assert(font);fread(font,1,size,f);fclose(f);
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
  lv_obj_set_style_text_font(g_desktop.screen,zh_font(20),0);
  g_desktop.panel=glass_box(g_desktop.screen,0,0,1024,GLASS_CONTENT_HEIGHT,0x000000,0);lv_obj_add_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN);
  char path[1024];snprintf(path,sizeof(path),"%s/screens.json",review_out);review_manifest=fopen(path,"w");assert(review_manifest);fputs("[\n",review_manifest);

 for(unsigned theme=0;theme<2;theme++){
  panel_hide(NULL);g_desktop.light_theme=theme!=0;glass_rebuild();
  launch_builtin_recorder_qapp(NULL);assert(qpk_review_eval("globalThis.__clips=Array.from({length:9},(_,i)=>({clip:'rec_'+String(i+1).padStart(6,'0'),durationMs:32000,fileBytes:1024044,valid:true}));__reviewTick();RecorderApp.select(0);RecorderApp.transcribe();__reviewTick();if(RecorderApp.state().captionCount!==3)throw Error('missing captions')")==0);
  lv_obj_update_layout(g_desktop.panel);
  lv_obj_t *convert=review_label_find(g_desktop.panel,"重新转写");assert(convert);lv_area_t button;lv_obj_get_coords(lv_obj_get_parent(convert),&button);
  assert(verify_pager_below(g_desktop.panel,button.y2+1)>=2);
  review_capture(display,"recorder-text","录音机本地转文字与下移分页");
  assert(qpk_review_eval("RecorderApp.play();__reviewTick();globalThis.__audioElapsed=1000;globalThis.__audioPeak=180;__reviewTick()")==0);
  assert(review_label_find(g_desktop.panel,"你好"));review_capture(display,"recorder-quiet","弱声音波形与同步字幕");
  assert(qpk_review_eval("globalThis.__audioElapsed=3000;globalThis.__audioPeak=15000;__reviewTick()")==0);
  assert(review_label_find(g_desktop.panel,"你好今天"));review_capture(display,"recorder-playing","回放波形与同步文字流");
  lv_obj_t *subtitle=review_label_find(g_desktop.panel,"你好今天"),*record=review_label_find(g_desktop.panel,"开始录音");assert(subtitle&&record);
  lv_area_t a,b;lv_obj_get_coords(subtitle,&a);lv_obj_get_coords(lv_obj_get_parent(record),&b);assert(a.y2<b.y1);
  panel_hide(NULL);
 }
 fputs("\n]\n",review_manifest);fclose(review_manifest);puts("RECORDER_NATIVE_TEXT_UI_OK themes, actual QuickJS layout, weak/loud PCM, audio-position captions, no overlap");return 0;
}
