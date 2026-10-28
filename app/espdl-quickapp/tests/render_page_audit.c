/* Primary page audit; production layouts with offline service fixtures. */
#define GLASS_LOCAL_SPEECH 1
#define main all_ui_reference_main
#include "render_all_ui.c"
#undef main

static unsigned audit_callbacks(lv_obj_t *root,lv_event_cb_t callback)
{
  unsigned count=0;
  for(unsigned i=0;i<lv_obj_get_event_count(root);i++)
    count+=lv_event_dsc_get_cb(lv_obj_get_event_dsc(root,i))==callback;
  for(unsigned i=0;i<lv_obj_get_child_count(root);i++)count+=audit_callbacks(lv_obj_get_child(root,i),callback);
  return count;
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
  lv_obj_remove_flag(g_desktop.screen,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_style_text_font(g_desktop.screen,zh_font(20),0);
  g_desktop.panel=glass_box(g_desktop.screen,0,0,1024,GLASS_CONTENT_HEIGHT,0,0);
  lv_obj_add_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN);
  char path[1024];snprintf(path,sizeof(path),"%s/screens.json",review_out);
  review_manifest=fopen(path,"w");assert(review_manifest);fputs("[\n",review_manifest);
  glass_status_create();
  for(unsigned theme=0;theme<2;theme++){
    review_music_fixture(0);review_chat_fixture(1);review_weather_fixture(0);review_ble_fixture(0);review_qpk_fixture(NULL);
    g_desktop.light_theme=theme!=0;glass_rebuild();review_capture(display,"home","桌面");
    SHOT("weather","天气",glass_weather_open(NULL));
    review_weather_fixture(1);struct glass_weather_snapshot weather;glass_weather_get(&weather);glass_weather_detail_update(&weather,false);
    review_capture(display,"weather-loaded","天气数据");
    SHOT("environment","环境趋势",glass_environment(NULL));
    SHOT("music","音乐",glass_music(NULL));
    review_music_fixture(1);music_ui_refresh(NULL);review_capture(display,"music-results","音乐搜索结果");
    g_music_ui.favorites=true;music_ui_list();review_capture(display,"music-favorites","音乐收藏");
    music_ui_input_open(NULL);review_capture(display,"music-input","音乐搜索输入");music_ui_input_close(NULL);
    music_full_open(NULL);assert(g_music_ui.full);review_capture(display,"music-fullscreen","音乐全屏");music_full_close(NULL);
    SHOT("chat","AI 工作台",glass_chat_open(NULL));
    if(g_chat_ui.snapshot->configured)assert(audit_callbacks(g_chat_ui.root,chat_ui_config)==1);
    assert(!review_find(g_chat_ui.root,"AI 设置"));assert(review_find(g_chat_ui.root,"能力设置"));
    glass_speech_open(NULL);assert(g_speech_ui.return_chat);review_capture(display,"speech","语音服务");
    g_desktop.page_back(NULL);assert(g_desktop.chat_page&&!g_chat_ui.ball_mode&&!g_speech_ui.timer);
    chat_workspace_inspect(NULL);review_capture(display,"chat-tools","AI 工具面板");glass_chat_back(NULL);
    chat_ui_editor_open(NULL);review_capture(display,"chat-input","AI 输入");glass_chat_back(NULL);assert(g_desktop.chat_page);
    chat_ui_config(NULL);review_capture(display,"chat-config","AI 配置");portal_go_back(NULL);assert(g_desktop.chat_page);
    SHOT("ball","Bot",glass_chat_ball_open(NULL));
    glass_speech_open(NULL);g_desktop.page_back(NULL);assert(g_desktop.chat_page&&g_chat_ui.ball_mode&&!g_speech_ui.timer);
    SHOT("apps","全部应用",glass_apps(NULL));glass_apps_page(NULL,1);review_capture(display,"apps-page2","全部应用第二页");
    qpk_review_hass_fixture(true);
    SHOT("homeassistant","智能家居",glass_homeassistant(NULL);assert(!qpk_review_eval("for(let i=0;i<60;i++)__reviewTick()")));
    assert(!qpk_review_eval("ESPHomeHA.openDevice(ESPHomeHA.snapshot().url,ESPHomeHA.snapshot().entities[0].entity_id);for(let i=0;i<40;i++)__reviewTick();if(!ESPHomeHA.snapshot().detailOpen)throw Error('detail missing')"));
    review_capture(display,"homeassistant-detail","家居设备详情");assert(qpk_runtime_back());
    assert(!qpk_review_eval("ESPHomeHA.settings();for(let i=0;i<30;i++)__reviewTick()"));
    review_capture(display,"homeassistant-settings","家居连接设置");assert(qpk_runtime_back());
    SHOT("camera","相机",launch_builtin_camera_qapp(NULL));
    assert(!qpk_review_eval("setSettingsOpen(true)"));review_capture(display,"camera-settings","相机设置");
    assert(qpk_runtime_back());assert(!qpk_review_eval("if(CameraPreview.settings())throw Error('settings stayed open')"));
    assert(!qpk_review_eval("setFiltersOpen(true)"));review_capture(display,"camera-filters","相机滤镜");
    assert(qpk_runtime_back());assert(!qpk_runtime_back());
    SHOT("espdl-face","离线识别：人脸跟随",launch_builtin_espdl_qapp(NULL));
    assert(!qpk_review_eval("EspDlApp.selectMode(0)"));review_capture(display,"espdl-classify","离线识别：分类");
    SHOT("recorder","录音机",launch_builtin_recorder_qapp(NULL));
    assert(!qpk_review_eval("__reviewTick()"));review_capture(display,"recorder-list","录音列表");
    assert(!qpk_review_eval("RecorderApp.select(0);RecorderApp.remove()"));review_capture(display,"recorder-delete","录音删除确认");
    assert(qpk_runtime_back());assert(qpk_runtime_running());assert(!qpk_runtime_back());
    review_capture(display,"recorder-delete-cancelled","返回取消录音删除确认");
    SHOT("live","网络直播",launch_bilibili(NULL));SHOT("album","相册",launch_local_video(NULL));
    SHOT("hello","你好快应用",launch_builtin_qapp(NULL));
    review_click("对话框");review_capture(display,"hello-dialog","快应用弹窗");dialog_cancel();
    SHOT("2048","2048",launch_builtin_2048_qapp(NULL));
    SHOT("expression","表情页",review_grok_expression_fixture());
    assert(!qpk_review_eval("for(let i=0;i<12;i++)__reviewTick()"));review_capture(display,"expression-frame","表情动画");
    SHOT("pet","桌宠",launch_builtin_dafeiyu_qapp(NULL));
    review_click("聊天");review_capture(display,"pet-input","桌宠聊天");
    panel_hide(NULL);glass_lock_open(NULL);review_capture(display,"lock","锁屏");lock_unlock(NULL);
    assert(!g_desktop.current_card&&!g_music_ui.timer&&!g_chat_ui.timer&&!qpk_runtime_running());
  }
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("PAGE_AUDIT_OK: both themes; desktop/weather/music/AI/Bot/apps/HA/camera/recognition/recorder/live/album/examples/game/pet/lock; unique AI settings; camera and HA back layers; cleanup");
}
