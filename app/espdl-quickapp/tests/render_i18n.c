/* SPDX-License-Identifier: Apache-2.0 */
#define UNIFIED_DEVICE_NO_MAIN
#include "render_unified_devices.c"
#include <unistd.h>

static unsigned text_count(lv_obj_t *root,const char *text)
{
  unsigned count=lv_obj_has_class(root,&lv_label_class)&&!strcmp(lv_label_get_text(root),text);
  for(unsigned i=0;i<lv_obj_get_child_count(root);i++)count+=text_count(lv_obj_get_child(root,i),text);
  return count;
}
static lv_obj_t *find_text(lv_obj_t *root,const char *text)
{
  if(lv_obj_has_class(root,&lv_label_class)&&!strcmp(lv_label_get_text(root),text))return root;
  for(unsigned i=0;i<lv_obj_get_child_count(root);i++){
    lv_obj_t *found=find_text(lv_obj_get_child(root,i),text);if(found)return found;
  }
  return NULL;
}
static void select_language(unsigned index)
{
  assert(g_glass.language_dropdown);
  lv_obj_t *before=g_desktop.current_card;
  lv_dropdown_set_selected(g_glass.language_dropdown,index);
  lv_obj_send_event(g_glass.language_dropdown,LV_EVENT_VALUE_CHANGED,NULL);
  assert(g_desktop.current_card==before); /* No deletion during event dispatch. */
  for(unsigned i=0;i<3;i++){lv_tick_inc(20);lv_timer_handler();}
  assert(glass_i18n_language()==index&&g_glass.language_dropdown);
}
static void check_button_labels(lv_obj_t *root)
{
  if(!lv_obj_is_visible(root))return;
  if(lv_obj_has_class(root,&lv_button_class)){
    for(unsigned i=0;i<lv_obj_get_child_count(root);i++){
      lv_obj_t *label=lv_obj_get_child(root,i);
      if(!lv_obj_has_class(label,&lv_label_class))continue;
      if(lv_obj_get_height(label)>lv_obj_get_height(root)){
        fprintf(stderr,"BUTTON_OVERFLOW: %s (%d > %d)\n",lv_label_get_text(label),lv_obj_get_height(label),lv_obj_get_height(root));
        assert(!"button text wrapped outside its control");
      }
    }
  }
  for(unsigned i=0;i<lv_obj_get_child_count(root);i++)check_button_labels(lv_obj_get_child(root,i));
}
static void capture_i18n(lv_display_t *display,const char *id,const char *title)
{
  lv_obj_update_layout(g_desktop.screen);check_button_labels(g_desktop.screen);
  review_capture(display,id,title);
}
int main(int argc,char **argv)
{
  assert(argc==3);review_out=argv[2];
  FILE *file=fopen(argv[1],"rb");assert(file);fseek(file,0,SEEK_END);size_t size=ftell(file);rewind(file);
  void *base=malloc(size);assert(base&&fread(base,1,size,file)==size);fclose(file);
  lv_init();lv_display_t *display=lv_display_create(1024,600);
  lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(display,review_fb,NULL,sizeof(review_fb),LV_DISPLAY_RENDER_MODE_FULL);
  lv_display_set_flush_cb(display,review_flush);
  g_desktop.font16=lv_tiny_ttf_create_data(base,size,16);
  g_desktop.font20=lv_tiny_ttf_create_data(base,size,20);
  g_desktop.font28=lv_tiny_ttf_create_data(base,size,28);
  g_desktop.font16->fallback=glass_icon_font(16);
  g_desktop.font20->fallback=glass_icon_font(20);
  g_desktop.font28->fallback=glass_icon_font(24);
  g_glass.font48=lv_tiny_ttf_create_data(base,size,48);
  g_glass.font72=lv_tiny_ttf_create_data(base,size,128);
  g_glass.music_font72=lv_tiny_ttf_create_data(base,size,72);
  glass_home_fonts_init(base,size);glass_global_fonts_init();
  g_glass.reduced_motion=true;g_glass.temperature_c=-999;g_glass.humidity_pct=-1;g_glass.pm25=-1;
  g_desktop.screen=glass_box(lv_screen_active(),0,32,1024,568,0,0);
  g_desktop.panel=glass_box(g_desktop.screen,0,0,1024,568,0,0);
  lv_obj_add_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN);
  glass_rebuild();
  lv_obj_t *user_label=glass_text(g_desktop.screen,"设置",0,0,100,16,0);
  lv_obj_add_flag(user_label,LV_OBJ_FLAG_HIDDEN);
  assert(!strcmp(glass_tr("设置"),"设置"));
  char path[1024];snprintf(path,sizeof(path),"%s/screens.json",review_out);
  review_manifest=fopen(path,"w");assert(review_manifest);fputs("[\n",review_manifest);
  for(unsigned theme=0;theme<2;theme++){
    g_desktop.light_theme=theme!=0;glass_rebuild();glass_settings(NULL);
    select_language(GLASS_LANGUAGE_ZH_CN);
    assert(text_count(g_desktop.current_card,"控制面板")>0);
    capture_i18n(display,"i18n-zh-settings","简体中文设置");
    select_language(GLASS_LANGUAGE_EN);
    assert(text_count(g_desktop.current_card,"Control panel")>0);
    assert(text_count(g_desktop.current_card,"System font")==1);
    assert(text_count(g_desktop.current_card,"Language")==1);
    assert(!strcmp(lv_label_get_text(user_label),"设置"));
    assert(!strcmp(glass_tr("unknown-key"),"unknown-key"));
    assert(chat_ball_end_phrase("结束对话")&&chat_ball_end_phrase("stop"));
    capture_i18n(display,"i18n-en-settings","English settings");
    panel_hide(NULL);capture_i18n(display,"i18n-en-home","English home");
    glass_apps(NULL);capture_i18n(display,"i18n-en-apps","English app launcher");
    glass_music(NULL);capture_i18n(display,"i18n-en-music","English music");
    glass_chat_open(NULL);capture_i18n(display,"i18n-en-chat","English chat");
    glass_wifi_network_open(NULL);capture_i18n(display,"i18n-en-wifi","English Wi-Fi");
    glass_bluetooth_page(NULL);capture_i18n(display,"i18n-en-bluetooth","English Bluetooth");
    about_clicked(NULL);capture_i18n(display,"i18n-en-about","English about");
    launch_builtin_camera_qapp(NULL);capture_i18n(display,"i18n-en-camera","English camera");
    assert(qpk_review_eval("if(system.language!=='en'||system.translate('设置')!=='Settings')throw Error('i18n bridge')")==0);
    assert(qpk_review_eval("globalThis.__localeProbe=ui.button('Probe',24,24,180,48,()=>{});ui.setStyle(__localeProbe,{fontSize:28});ui.setOpacity(__localeProbe,0);ui.setStyle(__localeProbe,{enabled:false})")==0);
    lv_obj_update_layout(g_desktop.current_card);
    lv_obj_t *probe=find_text(g_desktop.current_card,"Probe");assert(probe);
    assert(lv_obj_get_height(probe)==lv_obj_get_style_text_font(probe,0)->line_height);
    lv_obj_t *target=lv_obj_get_parent(probe);
    assert(lv_obj_get_style_opa(target,0)==LV_OPA_TRANSP);
    lv_obj_add_state(target,LV_STATE_PRESSED);
    assert(lv_obj_get_style_opa(target,0)==LV_OPA_TRANSP);
    assert(qpk_review_eval("ui.remove(__localeProbe)")==0);
    launch_builtin_recorder_qapp(NULL);capture_i18n(display,"i18n-en-recorder","English recorder");
    qpk_review_hass_fixture(true);glass_homeassistant(NULL);
    assert(qpk_review_eval("for(let i=0;i<50;i++)__reviewTick()")==0);
    capture_i18n(display,"i18n-en-ha","English Home Assistant");
    glass_settings(NULL);
    for(unsigned i=0;i<40;i++)select_language(i%2);
    assert(global_font_selected==0);
  }
  panel_hide(NULL);fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("I18N_UI_OK: both themes; deferred language switch; 80 repeated switches; user text retained; font retained; Chinese and English stop commands");
  return 0;
}
