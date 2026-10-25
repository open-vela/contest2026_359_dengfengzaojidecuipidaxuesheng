/* SPDX-License-Identifier: Apache-2.0 */
/* All launcher/system pages, actual LVGL trees, deterministic service fixtures. */
#define GLASS_LOCAL_SPEECH 1
#define main all_ui_reference_main
#include "render_all_ui.c"
#undef main
static void audit_click(const char *text)
{
  lv_obj_update_layout(g_desktop.screen);lv_obj_t *button=review_find(g_desktop.panel,text);
  if(!button)fprintf(stderr,"MISSING_PAGE_BUTTON %s\n",text);
  assert(button);lv_obj_send_event(button,LV_EVENT_CLICKED,NULL);
}
#define review_click audit_click
static unsigned objects(lv_obj_t *root)
{unsigned n=1;for(unsigned i=0;i<lv_obj_get_child_count(root);i++)n+=objects(lv_obj_get_child(root,i));return n;}
static unsigned timers(void)
{unsigned n=0;for(lv_timer_t *t=lv_timer_get_next(NULL);t;t=lv_timer_get_next(t))n++;return n;}
#define PAGE(name,body) static void page_##name(void){body;}
PAGE(about,about_clicked(NULL))
PAGE(settings,review_open_settings_entry())
PAGE(apps,glass_apps(NULL))
PAGE(wifi,glass_wifi_page(NULL))
PAGE(bluetooth,glass_bluetooth_page(NULL))
PAGE(mouse,glass_mouse_open(NULL))
PAGE(files,glass_storage_page(NULL);glass_files_tick(NULL))
PAGE(resources,glass_resources_page(NULL);glass_resources_update(NULL))
PAGE(weather,glass_weather_open(NULL))
PAGE(environment,glass_environment(NULL))
PAGE(portal,glass_portal_open(NULL))
PAGE(music,glass_music(NULL))
PAGE(lyrics,glass_music(NULL);music_full_open(NULL))
PAGE(pv,glass_music(NULL);music_full_open(NULL);music_full_mode(NULL))
PAGE(chat,chat_interface_modes[0]=AGENT_MODE_CHAT;glass_chat_show(false))
PAGE(create,chat_interface_modes[0]=AGENT_MODE_CREATE;glass_chat_show(false))
PAGE(assistant,chat_interface_modes[0]=AGENT_MODE_ASSISTANT;glass_chat_show(false))
PAGE(ball,chat_interface_modes[1]=AGENT_MODE_CHAT;glass_chat_show_mode(false,true))
PAGE(chat_input,chat_interface_modes[0]=AGENT_MODE_CHAT;glass_chat_show(false);chat_ui_editor_open(NULL))
PAGE(chat_modes,glass_chat_show(false);chat_ui_modes(NULL))
PAGE(voice,glass_speech_open(NULL))
PAGE(camera,launch_builtin_camera_qapp(NULL))
PAGE(recorder,launch_builtin_recorder_qapp(NULL))
PAGE(homeassistant,qpk_review_hass_fixture(true);launch_builtin_homeassistant_qapp(NULL))
PAGE(espdl,launch_builtin_espdl_qapp(NULL))
PAGE(pet,launch_builtin_dafeiyu_qapp(NULL))
PAGE(game,launch_builtin_2048_qapp(NULL))
PAGE(hello,launch_builtin_qapp(NULL))
PAGE(album,launch_local_video(NULL))
PAGE(live,launch_bilibili(NULL))
PAGE(files_detail,glass_storage_page(NULL);strcpy(g_files_selected,"/data/notes.txt");glass_files_details(NULL);glass_files_tick(NULL))
PAGE(files_dialog,glass_storage_page(NULL);g_files_operation=1;glass_files_dialog_async(NULL))
PAGE(music_search,page_music();music_ui_input_open(NULL);assert(g_music_ui.input_shade))
PAGE(chat_archive,review_chat_fixture(1);page_create();chat_ui_refresh(NULL);review_click("阅读完整回复");assert(g_chat_ui.archive_panel))
PAGE(chat_new_confirm,review_chat_fixture(1);page_create();chat_ui_refresh(NULL);chat_ui_new(NULL);assert(g_chat_ui.confirm))
PAGE(chat_discard,page_create();chat_ui_discard(NULL);assert(g_chat_ui.confirm))
PAGE(chat_config,page_chat();chat_ui_config(NULL))
PAGE(hass_settings,page_homeassistant();assert(!qpk_review_eval("for(let i=0;i<40;i++)__reviewTick();ESPHomeHA.settings();for(let i=0;i<20;i++)__reviewTick();if(!ESPHomeHA.snapshot().settingsOpen)throw Error('settings')")))
PAGE(recorder_delete,page_recorder();assert(!qpk_review_eval("__reviewTick();RecorderApp.select(0);RecorderApp.remove()")))
PAGE(recorder_recording,page_recorder();assert(!qpk_review_eval("__reviewTick();RecorderApp.record();__reviewTick()")))
PAGE(pet_input,page_pet();review_click("聊天"))
PAGE(hello_dialog,page_hello();review_click("对话框"))
PAGE(hello_toast,page_hello();review_click("Toast 提示"))
PAGE(files_image,page_files();strcpy(g_files_selected,"/offline-fixtures/review.jpg");glass_files_image(NULL))
PAGE(files_copy,page_files();strcpy(g_files_clipboard,"/data/notes.txt");strcpy(g_files_path,"/data/recordings");g_files_clipboard_move=false;glass_files_copy_start(NULL))
PAGE(files_delete,page_files();g_files_operation=3;glass_files_dialog_async(NULL))
#undef PAGE
struct page_case {const char *name;void (*open)(void);};
static const struct page_case cases[]={
#define ITEM(name) {#name,page_##name}
  ITEM(about),ITEM(settings),ITEM(apps),ITEM(wifi),ITEM(bluetooth),ITEM(mouse),
  ITEM(files),ITEM(resources),ITEM(weather),ITEM(environment),ITEM(portal),
  ITEM(music),ITEM(lyrics),ITEM(pv),ITEM(chat),ITEM(create),ITEM(assistant),ITEM(ball),ITEM(chat_input),ITEM(chat_modes),
  ITEM(voice),ITEM(camera),ITEM(recorder),ITEM(homeassistant),ITEM(espdl),ITEM(pet),
  ITEM(game),ITEM(hello),ITEM(album),ITEM(live),ITEM(files_detail),ITEM(files_dialog),
  ITEM(music_search),ITEM(chat_archive),ITEM(chat_new_confirm),ITEM(chat_discard),ITEM(chat_config),
  ITEM(hass_settings),ITEM(recorder_delete),ITEM(recorder_recording),ITEM(pet_input),ITEM(hello_dialog),ITEM(hello_toast),
  ITEM(files_image),ITEM(files_copy),ITEM(files_delete)
#undef ITEM
};
static void settle(void)
{lv_tick_inc(1);lv_timer_handler();lv_obj_update_layout(lv_screen_active());lv_obj_update_layout(lv_layer_top());}
static lv_obj_t *opaque_cover(void)
{
  lv_obj_t *cover=lv_obj_create(lv_layer_top());lv_obj_remove_style_all(cover);
  lv_obj_set_size(cover,1024,600);lv_obj_set_style_bg_opa(cover,LV_OPA_COVER,0);
  lv_obj_update_layout(lv_screen_active());lv_obj_update_layout(lv_layer_top());return cover;
}
static void occlusion_checks(void)
{
  lv_obj_t *initial=lv_obj_create(lv_screen_active());assert(glass_ui_can_paint(initial));
  lv_obj_add_flag(initial,LV_OBJ_FLAG_HIDDEN);assert(!glass_ui_can_paint(initial));lv_obj_delete(initial);
  lv_obj_t *page=lv_obj_create(lv_screen_active()),*cover=opaque_cover();
  assert(lv_obj_is_visible(page)&&!glass_ui_can_paint(page));
  assert(glass_ui_can_paint(cover));
  lv_obj_t *above=lv_obj_create(lv_layer_top());
  lv_obj_update_layout(lv_layer_top());
  assert(glass_ui_can_paint(above));
  lv_obj_move_to_index(above,0);assert(!glass_ui_can_paint(above));
  lv_obj_delete(above);
  lv_obj_set_width(cover,1023);lv_obj_update_layout(lv_layer_top());assert(glass_ui_can_paint(page));
  lv_obj_set_width(cover,1024);lv_obj_update_layout(lv_layer_top());
  lv_obj_set_style_bg_opa(cover,128,0);assert(glass_ui_can_paint(page));
  lv_obj_set_style_bg_opa(cover,LV_OPA_COVER,0);
  lv_obj_set_style_radius(cover,10,0);assert(glass_ui_can_paint(page));lv_obj_set_style_radius(cover,0,0);
  lv_obj_set_style_opa(cover,128,0);assert(glass_ui_can_paint(page));lv_obj_set_style_opa(cover,255,0);
  lv_obj_set_style_transform_rotation(cover,10,0);assert(glass_ui_can_paint(page));lv_obj_set_style_transform_rotation(cover,0,0);
  lv_obj_set_style_transform_scale_x(cover,128,0);assert(glass_ui_can_paint(page));lv_obj_set_style_transform_scale_x(cover,256,0);
  lv_obj_set_style_transform_skew_x(cover,10,0);assert(glass_ui_can_paint(page));lv_obj_set_style_transform_skew_x(cover,0,0);
  lv_obj_set_style_translate_x(cover,1,0);assert(glass_ui_can_paint(page));lv_obj_set_style_translate_x(cover,0,0);
  lv_obj_set_style_opa_layered(cover,128,0);assert(glass_ui_can_paint(page));lv_obj_set_style_opa_layered(cover,255,0);
  lv_obj_set_style_blend_mode(cover,LV_BLEND_MODE_ADDITIVE,0);assert(glass_ui_can_paint(page));lv_obj_set_style_blend_mode(cover,LV_BLEND_MODE_NORMAL,0);
  lv_obj_add_flag(cover,LV_OBJ_FLAG_HIDDEN);assert(glass_ui_can_paint(page));lv_obj_remove_flag(cover,LV_OBJ_FLAG_HIDDEN);
  lv_obj_delete(cover);lv_obj_delete(page);
  /* Real native chart: sample history survives while presentation is deferred. */
  glass_resources_page(NULL);settle();
  unsigned count=g_resources.count[0];char shown[96];strlcpy(shown,lv_label_get_text(g_resources.labels[0]),sizeof(shown));
  cover=opaque_cover();struct glass_system_sample sample=g_resources.latest;sample.cpu_error=0;sample.cpu_tenths=731;
  lv_tick_inc(1000);glass_resources_show(&sample);
  assert(g_resources.count[0]==count+1&&!strcmp(shown,lv_label_get_text(g_resources.labels[0])));
  lv_obj_delete(cover);glass_resources_show(&sample);
  assert(!strcmp(lv_label_get_text(g_resources.labels[0]),"73.1%"));panel_hide(NULL);settle();
  /* Real QuickJS binding sees top-layer coverage and immediate reveal. */
  page_homeassistant();settle();
  assert(!qpk_review_eval("if(!ui.isVisible())throw Error('visible app')"));
  cover=opaque_cover();
  assert(!qpk_review_eval("if(ui.isVisible())throw Error('covered app')"));
  lv_obj_delete(cover);
  assert(!qpk_review_eval("if(!ui.isVisible())throw Error('revealed app')"));
  panel_hide(NULL);settle();
  /* Reader/frame/timer work stops beneath a solid cover; drawing resumes it. */
  struct glass_grok *ball=glass_grok_create_avatar(lv_screen_active(),zh_font);assert(ball);
  unsigned stats[3],paused[3];
  for(unsigned i=0;i<500;i++){
    lv_tick_inc(20);lv_timer_handler();usleep(1000);glass_grok_test_stats(ball,stats);
    if(stats[0]>=3&&stats[1]>=2)break;
  }
  assert(stats[0]>=3&&stats[1]>=2);cover=opaque_cover();
  lv_tick_inc(200);lv_timer_handler();usleep(10000);glass_grok_test_stats(ball,paused);
  for(unsigned i=0;i<100;i++){lv_tick_inc(20);lv_timer_handler();usleep(1000);}
  glass_grok_test_stats(ball,stats);assert(!memcmp(stats,paused,sizeof(stats)));
  printf("COVERED_SPRITE simulated_ms=2000 reads=0 frames=0 timer_callbacks=0\n");
  lv_obj_delete(cover);
  for(unsigned i=0;i<200;i++){
    lv_tick_inc(20);lv_timer_handler();usleep(1000);glass_grok_test_stats(ball,stats);
    if(stats[0]>paused[0]&&stats[1]>paused[1])break;
  }
  assert(stats[0]>paused[0]&&stats[1]>paused[1]);
  lv_obj_delete(glass_grok_root(ball));
  printf("OCCLUSION_OK native-history, QuickJS, sprite reads/frames/ticks paused and resumed\n");
}
static void retired(const char *name)
{
  if(g_lock_ui.root)lock_unlock(NULL);
  panel_hide(NULL);settle();
  if(lv_obj_get_child_count(g_desktop.panel)||g_chat_ui.root||g_music_ui.root||
     g_music_ui.full||portal_timer||g_speech_ui.root||g_wifi_timer||g_mouse_timer||
     g_files_timer||g_resources.timer||g_camera_page.page||qpk_runtime_running()){
    fprintf(stderr,"RETIRED_PAGE_LEAK: %s\n",name);abort();
  }
}
int main(int argc,char **argv)
{
  assert(argc==3);FILE *file=fopen(argv[1],"rb");assert(file);
  fseek(file,0,SEEK_END);size_t bytes=ftell(file);rewind(file);
  void *font=malloc(bytes);assert(font&&fread(font,1,bytes,file)==bytes);fclose(file);
  lv_init();lv_display_t *display=lv_display_create(1024,600);
  lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(display,review_fb,NULL,sizeof(review_fb),LV_DISPLAY_RENDER_MODE_FULL);
  lv_display_set_flush_cb(display,review_flush);
  g_desktop.font16=lv_tiny_ttf_create_data(font,bytes,16);
  g_desktop.font20=lv_tiny_ttf_create_data(font,bytes,20);
  g_desktop.font28=lv_tiny_ttf_create_data(font,bytes,28);
  g_desktop.font16->fallback=glass_icon_font(16);g_desktop.font20->fallback=glass_icon_font(20);
  g_desktop.font28->fallback=glass_icon_font(24);
  g_glass.font48=lv_tiny_ttf_create_data(font,bytes,48);glass_home_fonts_init(font,bytes);
  g_glass.reduced_motion=getenv("PAGE_AUDIT_MOTION")==NULL;g_glass.temperature_c=-999;
  g_desktop.screen=glass_box(lv_screen_active(),0,32,1024,568,0,0);
  g_desktop.panel=glass_box(g_desktop.screen,0,0,1024,568,0,0);
  lv_obj_add_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN);glass_status_create();
  occlusion_checks();
  review_out=argv[2];
  char manifest_path[1024];snprintf(manifest_path,sizeof(manifest_path),"%s/screens.json",review_out);
  review_manifest=fopen(manifest_path,"w");assert(review_manifest);fputs("[\n",review_manifest);
  char path[1024];snprintf(path,sizeof(path),"%s/lifetime.jsonl",argv[2]);FILE *report=fopen(path,"w");assert(report);
  unsigned cycles=0;
  for(unsigned theme=0;theme<2;theme++){
    g_desktop.light_theme=theme==0;glass_rebuild();settle();
    /* Warm one-time shared caches before measuring recurring retention. */
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++){cases[i].open();settle();retired(cases[i].name);}
    unsigned base_screen=objects(lv_screen_active()),base_top=objects(lv_layer_top()),base_timers=timers(),base_anims=lv_anim_count_running();
    for(unsigned round=0;round<3;round++)for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++){
      cases[i].open();settle();
      if(qpk_runtime_running())assert(!qpk_review_eval("for(let i=0;i<20;i++)__reviewTick()"));
      if(round==0){
        review_capture(display,cases[i].name,cases[i].name);
        if(!strcmp(cases[i].name,"apps")){
          lv_area_t title,close,status,next_title,next_close;
          lv_obj_get_coords(lv_obj_get_child(g_apps_card,0),&title);
          lv_obj_get_coords(lv_obj_get_child(g_apps_card,1),&close);
          lv_obj_get_coords(g_status_ui.bar,&status);
          assert(title.y1>status.y2&&close.y1>status.y2);
          glass_apps_page(NULL,1);settle();
          lv_obj_get_coords(lv_obj_get_child(g_apps_card,0),&next_title);
          lv_obj_get_coords(lv_obj_get_child(g_apps_card,1),&next_close);
          assert(!memcmp(&title,&next_title,sizeof(title))&&!memcmp(&close,&next_close,sizeof(close)));
          review_capture(display,"apps-page2","应用第二页");
          glass_apps_page(NULL,0);settle();
        }
      }
      unsigned live_objects=objects(lv_screen_active())+objects(lv_layer_top()),live_timers=timers();
      /* An opaque overlay retains a page; removing it must restore that page. */
      lv_obj_t *owner=g_desktop.current_card;g_lock_page=(i+round)%4;
      glass_lock_open(NULL);settle();lock_unlock(NULL);settle();
      assert(g_desktop.current_card==owner);retired(cases[i].name);
      unsigned screen=objects(lv_screen_active()),top=objects(lv_layer_top()),count=timers(),anims=lv_anim_count_running();
      fprintf(report,"{\"theme\":%u,\"round\":%u,\"page\":\"%s\",\"live_objects\":%u,\"live_timers\":%u,\"screen\":%u,\"top\":%u,\"timers\":%u,\"animations\":%u}\n",theme,round,cases[i].name,live_objects,live_timers,screen,top,count,anims);fflush(report);
      if(screen!=base_screen||top!=base_top||count!=base_timers||anims!=base_anims){
        fprintf(stderr,"PAGE_RETENTION %s screen=%u/%u top=%u/%u timers=%u/%u animations=%u/%u\n",cases[i].name,screen,base_screen,top,base_top,count,base_timers,anims,base_anims);abort();
      }
      cycles++;
    }
  }
  fclose(report);fputs("\n]\n",review_manifest);fclose(review_manifest);
  printf("ALL_PAGE_LIFETIME_OK pages=%zu themes=2 cycles=%u reduced_motion=%d; no retained page nodes/timers/animations\n",sizeof(cases)/sizeof(cases[0]),cycles,g_glass.reduced_motion);
  return 0;
}
