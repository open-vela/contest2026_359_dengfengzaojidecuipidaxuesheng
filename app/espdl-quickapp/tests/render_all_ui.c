/* SPDX-License-Identifier: Apache-2.0 */
/* Offline renderer: every page uses its actual production C/JS layout. */
#include <assert.h>
#include <math.h>
#include "mijia_card_fixture.h"
#include "production_desktop.inc"
#include "media_pages.inc"

static uint8_t review_fb[1024*600*2];
static void review_flush(lv_display_t *display,const lv_area_t *area,uint8_t *pixels)
{
  for(int y=area->y1;y<=area->y2;y++){
    memcpy(review_fb+(y*1024+area->x1)*2,pixels,(area->x2-area->x1+1)*2);
    pixels+=(area->x2-area->x1+1)*2;
  }
  lv_display_flush_ready(display);
}
/* Keep original expression-SDK regression cases separate from the upgraded
 * launcher entry, which now opens the shared native conversation view. */
static void review_grok_expression_fixture(void)
{
  extern const char *ouo_get_app_js(unsigned int *);
  unsigned size;const char *source=ouo_get_app_js(&size);
  const struct qpk_entry_s expression_manifest={.name="Grok Ball",.package="ouo",.version="2.0.0",.entry="builtin:/ouo/app.js"};
  const struct qpk_entry_s *manifest=&expression_manifest;
  lv_obj_t *card=qapp_page_prepare(manifest->name);assert(card&&source&&size);
  assert(qpk_runtime_launch(card,manifest->name,manifest->package,manifest->version,manifest->entry,
    source,size,zh_font,show_toast,qpk_show_dialog,qpk_show_unresponsive)==0);
  qapp_page_commit(card);
}
static const char *review_out;
static FILE *review_manifest;
static unsigned review_count;
extern int qpk_review_eval(const char *source);
extern int qpk_review_click_at(int x,int y);
extern void qpk_review_hass_fixture(bool ready);
extern void review_music_fixture(int mode);
extern void review_music_progress_fixture(unsigned position,unsigned duration);
extern void review_chat_fixture(int mode);
extern void review_weather_fixture(int mode);
extern void review_ble_fixture(int mode);
extern void review_qpk_fixture(const char *source);
static lv_obj_t *review_find(lv_obj_t *object,const char *text)
{
  if(!object||!lv_obj_is_visible(object))return NULL;
  if(lv_obj_has_flag(object,LV_OBJ_FLAG_CLICKABLE))
    for(unsigned i=0;i<lv_obj_get_child_count(object);i++){
      lv_obj_t *child=lv_obj_get_child(object,i);
      if(lv_obj_has_class(child,&lv_label_class)&&!strcmp(lv_label_get_text(child),text))return object;
    }
  for(unsigned i=0;i<lv_obj_get_child_count(object);i++){
    lv_obj_t *found=review_find(lv_obj_get_child(object,i),text);if(found)return found;
  }
  return NULL;
}
static void review_click(const char *text){lv_obj_update_layout(g_desktop.screen);lv_obj_t *button=review_find(g_desktop.panel,text);assert(button);lv_obj_send_event(button,LV_EVENT_CLICKED,NULL);}
static lv_obj_t *review_label_find(lv_obj_t *object,const char *text)
{
  if(!object||!lv_obj_is_visible(object))return NULL;
  if(lv_obj_has_class(object,&lv_label_class)&&!strcmp(lv_label_get_text(object),text))return object;
  for(unsigned i=0;i<lv_obj_get_child_count(object);i++){
    lv_obj_t *found=review_label_find(lv_obj_get_child(object,i),text);if(found)return found;
  }
  return NULL;
}
static lv_obj_t *review_callback_find(lv_obj_t *object,lv_event_cb_t callback)
{
  if(!object||!lv_obj_is_visible(object))return NULL;
  for(unsigned i=0;i<lv_obj_get_event_count(object);i++)
    if(lv_event_dsc_get_cb(lv_obj_get_event_dsc(object,i))==callback)return object;
  for(unsigned i=0;i<lv_obj_get_child_count(object);i++){
    lv_obj_t *found=review_callback_find(lv_obj_get_child(object,i),callback);if(found)return found;
  }
  return NULL;
}
static void review_open_settings_entry(void)
{
  panel_hide(NULL);lv_obj_update_layout(g_desktop.screen);
  if(!g_status_ui.bar)glass_status_create();
  lv_obj_update_layout(lv_layer_top());
  assert(g_status_ui.island && lv_obj_is_visible(g_status_ui.island));
  lv_obj_send_event(g_status_ui.island,LV_EVENT_CLICKED,NULL);
  assert(g_desktop.settings_rebuild_pending);
  lv_async_call_cancel(settings_rebuild_async,NULL);settings_rebuild_async(NULL);
  assert(g_control_card==g_desktop.current_card&&!lv_obj_has_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN));
}
static void review_navigation_entries(void)
{
  /* Real button events plus the real deferred dispatcher, with offline services. */
  const char *names[]={"Wi-Fi 网络","蓝牙设备","系统性能"};
  const unsigned kinds[]={0,2,3};
  for(unsigned i=0;i<3;i++){
    review_open_settings_entry();review_click(names[i]);
    assert(g_system_page_pending&&g_system_page_kind==kinds[i]);
    lv_async_call_cancel(glass_system_page_async,NULL);glass_system_page_async(NULL);
    assert(!g_system_page_pending&&g_desktop.current_card&&g_control_card==NULL);
    if(i==0)assert(g_wifi_ssid!=NULL);
    if(i==2){
      assert(g_resources.card==g_desktop.current_card);
      lv_obj_send_event(g_resources.tiles[1],LV_EVENT_CLICKED,NULL);assert(g_resources.selected==1);
      lv_obj_send_event(g_resources.tiles[0],LV_EVENT_CLICKED,NULL);assert(g_resources.selected==0);
    }
  }
  review_open_settings_entry();review_click("扫码配置");
  lv_async_call_cancel(glass_system_page_async,NULL);glass_system_page_async(NULL);
  assert(portal_page==g_desktop.current_card);
  review_open_settings_entry();review_click("返回桌面");assert(g_control_close_pending);
  lv_async_call_cancel(glass_control_close_async,NULL);glass_control_close_async(NULL);
  assert(lv_obj_has_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN));
  lv_obj_update_layout(g_desktop.screen);
  lv_obj_t *weather=review_callback_find(g_glass.home,glass_weather_open);assert(weather);
  lv_obj_send_event(weather,LV_EVENT_CLICKED,NULL);assert(g_weather_detail.location&&g_weather_detail.temperature);
  review_click("刷新天气");assert(strstr(lv_label_get_text(g_weather_detail.status),"已请求刷新"));
  panel_hide(NULL);lv_obj_update_layout(g_desktop.screen);
  lv_obj_t *widget=review_callback_find(g_glass.home,glass_widget_open);assert(widget);
  lv_obj_send_event(widget,LV_EVENT_CLICKED,NULL);assert(g_weather_detail.location);
  panel_hide(NULL);
  printf("ENTRY_NAVIGATION_OK %s: settings, wifi, files, bluetooth, resources, CPU/memory, portal, weather, widget, back\n",g_desktop.light_theme?"light":"dark");
}
static void review_capture(lv_display_t *d,const char *id,const char *name)
{
  /* No LVGL timer_handler: service callbacks cannot start workers. */
  lv_obj_update_layout(g_desktop.screen);lv_refr_now(d);
  char path[1024];snprintf(path,sizeof(path),"%s/%s-%s.rgb565",review_out,
    g_desktop.light_theme?"light":"dark",id);
  FILE *f=fopen(path,"wb");assert(f);fwrite(review_fb,1,sizeof(review_fb),f);fclose(f);
  fprintf(review_manifest,"%s{\"id\":\"%s\",\"title\":\"%s\",\"theme\":\"%s\",\"file\":\"%s-%s.png\"}",
    review_count++?",\n":"",id,name,g_desktop.light_theme?"浅色":"深色",g_desktop.light_theme?"light":"dark",id);
  printf("CAPTURE %s %s\n",g_desktop.light_theme?"light":"dark",id);fflush(stdout);
}
#define SHOT(id,title,call) do{panel_hide(NULL);call;review_capture(display,id,title);}while(0)
static void review_grok_frame(lv_display_t *display,const char *kind,unsigned frame)
{
  lv_obj_update_layout(g_desktop.screen);lv_refr_now(display);
  char path[1024];snprintf(path,sizeof(path),"%s/%s-grok-%s-%03u.rgb565",review_out,g_desktop.light_theme?"light":"dark",kind,frame);
  FILE *animation=fopen(path,"wb");assert(animation);
  assert(fwrite(review_fb,1,sizeof(review_fb),animation)==sizeof(review_fb));fclose(animation);
}
int main(int argc,char **argv)
{
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
    review_music_fixture(0);review_chat_fixture(0);review_weather_fixture(0);
    review_ble_fixture(0);
    review_qpk_fixture(NULL);strcpy(g_files_path,"/data");g_files_clipboard[0]=0;g_files_clipboard_move=false;
    g_glass.samples=0;g_glass.has_sample=false;g_glass.temperature_c=-999;
    g_glass.humidity_pct=-1;g_glass.pm25=-1;g_glass.available=g_glass.total=0;
    qpk_review_hass_fixture(false);
    panel_hide(NULL);g_desktop.light_theme=theme==0;glass_rebuild();
    review_navigation_entries();
    review_capture(display,"home","首页");
    SHOT("settings","控制面板",glass_settings(NULL));
    /* A style reset must not move the two controls or collapse their hit targets. */
    lv_area_t theme_bounds,motion_bounds;
    lv_obj_get_coords(g_glass.settings_switch,&theme_bounds);
    lv_obj_get_coords(g_glass.motion_switch,&motion_bounds);
    assert(theme_bounds.x1>500&&theme_bounds.y1>350);
    assert(theme_bounds.x1==motion_bounds.x1&&theme_bounds.y2+8<motion_bounds.y1);
    assert(lv_obj_get_width(g_glass.settings_switch)>=48&&lv_obj_get_height(g_glass.settings_switch)>=28);
    assert(lv_obj_has_state(g_glass.settings_switch,LV_STATE_CHECKED)==g_desktop.light_theme);
    assert(lv_obj_has_state(g_glass.motion_switch,LV_STATE_CHECKED)==g_glass.reduced_motion);
    lv_dropdown_open(g_glass.widget_dropdown);review_capture(display,"settings-dropdown","桌面组件选择");lv_dropdown_close(g_glass.widget_dropdown);
    SHOT("wifi","Wi-Fi 网络",glass_wifi_page(NULL));
    g_wifi_result.count=3;
    const char *networks[]={"审核示例网络","Studio WiFi","Guest Network"};
    for(unsigned i=0;i<3;i++){g_wifi_result.records[i].ssid_length=strlen(networks[i]);memcpy(g_wifi_result.records[i].ssid,networks[i],strlen(networks[i]));g_wifi_result.records[i].rssi=-42-i*12;}
    glass_wifi_render_list();glass_wifi_status_set(GLASS_WIFI_COUNT,3);review_capture(display,"wifi-scan","Wi-Fi 扫描结果");
    lv_textarea_set_text(g_wifi_ssid,networks[0]);glass_wifi_edit_view(NULL);review_capture(display,"wifi-password","Wi-Fi 密码输入");
    SHOT("bluetooth","蓝牙设备",glass_bluetooth_page(NULL));
    SHOT("mouse","蓝牙鼠标",glass_mouse_open(NULL));
    SHOT("files","文件管理",glass_storage_page(NULL));
    glass_files_tick(NULL);review_capture(display,"files-loaded","文件列表");
    g_files_operation=1;glass_files_dialog_async(NULL);review_capture(display,"files-new","新建目录");
    SHOT("files-details-loading","文件详情读取",strcpy(g_files_selected,"/data/notes.txt");glass_files_details(NULL));
    glass_files_tick(NULL);review_capture(display,"files-details","文件详情与文本预览");
    g_files_operation=2;glass_files_dialog_async(NULL);review_capture(display,"files-rename","重命名文件");
    g_files_operation=3;glass_files_dialog_async(NULL);review_capture(display,"files-delete","删除文件确认");
    SHOT("files-copy","复制文件进度",strcpy(g_files_clipboard,"/data/notes.txt");strcpy(g_files_path,"/data/recordings");g_files_clipboard_move=false;glass_files_copy_start(NULL));
    SHOT("files-move","移动文件进度",strcpy(g_files_clipboard,"/data/notes.txt");g_files_clipboard_move=true;glass_files_copy_start(NULL));
    SHOT("files-image","图片查看（文件不可用）",strcpy(g_files_selected,"/offline-fixtures/review.jpg");glass_files_image(NULL));
    SHOT("resources","系统性能",glass_resources_page(NULL);glass_resources_update(NULL));
    for(unsigned i=0;i<65;i++){
      lv_tick_inc(1000);struct glass_system_sample sample={.cpu_tenths=90+(i%11)*22,.heap_total=32ULL*1024*1024,.heap_free=(22ULL*1024-i%9*192)*1024,.heap_largest=14ULL*1024*1024,.heap_peak=13ULL*1024*1024};
      if(i>24&&i<30)sample.cpu_tenths=610+(i%3)*75;
      if(i==64){sample.cpu_tenths=125;sample.heap_free=22ULL*1024*1024;}
      glass_resources_show(&sample);
    }
    assert(g_resources.count[0]==60&&g_resources.count[1]==60);
    review_capture(display,"resources-history","CPU 占用趋势");
    lv_obj_send_event(g_resources.tiles[1],LV_EVENT_CLICKED,NULL);
    assert(!strcmp(lv_label_get_text(g_resources.labels[6]),"31.2%"));
    review_capture(display,"resources-memory","内存占用趋势");
    struct glass_system_sample unavailable={.cpu_error=ENOTSUP,.heap_error=EIO};
    lv_tick_inc(1000);
    glass_resources_show(&unavailable);assert(g_resources.count[0]==60&&g_resources.count[1]==60);
    assert(g_resources.history[0][59]==125&&g_resources.history[1][59]==312);
    assert(!strcmp(lv_label_get_text(g_resources.labels[6]),"暂不可用"));
    SHOT("weather","天气详情",glass_weather_open(NULL));
    review_weather_fixture(1);struct glass_weather_snapshot weather;glass_weather_get(&weather);glass_weather_detail_update(&weather,false);
    review_capture(display,"weather-loaded","天气详情（数据示例）");
    for(int i=0;i<24;i++)glass_dashboard_publish(22+(i%7)/2,48,12,6,8);
    SHOT("environment","环境趋势",glass_environment(NULL));
    SHOT("portal","扫码配置",glass_portal_open(NULL));
    SHOT("music","音乐空间",glass_music(NULL));
    assert(lv_bar_get_value(g_music_ui.progress)==0);
    review_music_fixture(1);music_ui_refresh(NULL);review_capture(display,"music-result","音乐搜索结果");
    assert(lv_bar_get_value(g_music_ui.progress)==205);
    assert(!strcmp(lv_label_get_text(g_music_ui.duration),"03:24"));
    g_music_ui.favorites=true;music_ui_list();review_capture(display,"music-favorites","音乐收藏");
    music_ui_input_open(NULL);review_capture(display,"music-search","音乐搜索输入");
    music_ui_input_close(NULL);review_music_fixture(2);music_ui_refresh(NULL);review_capture(display,"music-playing","音乐正在播放");
    review_music_progress_fixture(102000,204000);music_ui_refresh(NULL);
    assert(lv_bar_get_value(g_music_ui.progress)==500);
    review_music_progress_fixture(250000,204000);music_ui_refresh(NULL);
    assert(lv_bar_get_value(g_music_ui.progress)==1000);
    review_music_progress_fixture(42000,0);music_ui_refresh(NULL);
    assert(lv_bar_get_value(g_music_ui.progress)==0);
    assert(!strcmp(lv_label_get_text(g_music_ui.duration),"--:--"));
    review_capture(display,"music-duration-unknown","音乐播放（时长未知）");
    review_music_fixture(2);music_ui_refresh(NULL);
    SHOT("chat","AI 助手",glass_chat_open(NULL));
    chat_ui_editor_open(NULL);review_capture(display,"chat-input","AI 对话输入");
    chat_ui_editor_close(NULL);review_chat_fixture(1);chat_ui_refresh(NULL);review_capture(display,"chat-conversation","AI 对话内容");
    review_click("阅读完整回复");chat_ui_refresh(NULL);review_capture(display,"chat-full-response","AI 完整回复与分页");
    chat_ui_archive_close(NULL);chat_ui_new(NULL);review_capture(display,"chat-new-confirm","开始新对话确认");
    chat_ui_confirm_close(NULL);review_qpk_fixture(g_hello_qpk_js);chat_ui_refresh(NULL);review_capture(display,"chat-draft","AI 应用草稿就绪");
    chat_ui_preview_app(NULL);review_capture(display,"chat-app-preview","AI 快应用草稿预览");
    chat_qpk_preview_back();review_qpk_fixture(NULL);chat_ui_refresh(NULL);
    chat_ui_confirm_close(NULL);chat_ui_discard(NULL);review_capture(display,"chat-discard-confirm","放弃应用创意确认");
    chat_ui_confirm_close(NULL);chat_ui_config(NULL);review_capture(display,"chat-config","AI 服务扫码配置");
    SHOT("hass","家居首次连接配置",glass_homeassistant(NULL);qpk_review_eval("for(let i=0;i<40;i++)__reviewTick()"));
    qpk_review_hass_fixture(true);
    SHOT("hass-overview","家居总览",glass_homeassistant(NULL);assert(qpk_review_eval("for(let i=0;i<50;i++)__reviewTick();if(ESPHomeHA.snapshot().entities.length!==8)throw Error('HA fixture not loaded')")==0));
    assert(qpk_review_click_at(982,508)==0);assert(qpk_review_eval("for(let i=0;i<30;i++)__reviewTick();if(ESPHomeHA.snapshot().roomPage!==1)throw Error('HA pagination failed')")==0);
    review_capture(display,"hass-overview-page2","家居总览（第二页）");
    assert(qpk_review_click_at(938,508)==0);assert(qpk_review_eval("for(let i=0;i<30;i++)__reviewTick()")==0);
    assert(qpk_review_click_at(82,135)==0);assert(qpk_review_eval("for(let i=0;i<30;i++)__reviewTick()")==0);
    review_capture(display,"hass-devices","家居设备卡片");
    assert(qpk_review_click_at(245,210)==0);assert(qpk_review_eval("for(let i=0;i<30;i++)__reviewTick();if(!ESPHomeHA.snapshot().detailOpen)throw Error('HA detail not open')")==0);
    review_capture(display,"hass-device-detail","家居灯光展开卡片");
    assert(qpk_review_click_at(760,337)==0);assert(qpk_review_eval("for(let i=0;i<30;i++)__reviewTick();if(!ESPHomeHA.snapshot().detailAdvanced)throw Error('HA more operations not open')")==0);
    review_capture(display,"hass-device-more","家居设备更多操作与亮度");
    assert(qpk_review_click_at(190,31)==0);assert(qpk_review_eval("for(let i=0;i<30;i++)__reviewTick()")==0);
    assert(qpk_review_click_at(190,31)==0);assert(qpk_review_eval("for(let i=0;i<30;i++)__reviewTick()")==0);
    assert(qpk_review_click_at(82,181)==0);assert(qpk_review_eval("for(let i=0;i<30;i++)__reviewTick()")==0);
    review_capture(display,"hass-environment","家居环境数据");
    assert(qpk_review_click_at(82,227)==0);assert(qpk_review_eval("for(let i=0;i<30;i++)__reviewTick()")==0);
    review_capture(display,"hass-system","家居系统信息");
    assert(qpk_review_eval("ESPHomeHA.settings();for(let i=0;i<30;i++)__reviewTick()")==0);
    review_capture(display,"hass-settings","家居连接设置");
    assert(qpk_review_click_at(190,31)==0);assert(qpk_review_eval("for(let i=0;i<35;i++)__reviewTick()")==0);assert(qpk_review_click_at(82,89)==0);
    assert(qpk_review_eval("for(let i=0;i<35;i++)__reviewTick()")==0);
    assert(qpk_review_click_at(900,30)==0);assert(qpk_review_eval("for(let i=0;i<30;i++)__reviewTick();if(!ESPHomeHA.snapshot().layoutEditing)throw Error('Layout editor failed')")==0);
    assert(qpk_review_click_at(245,210)==0);assert(qpk_review_eval("for(let i=0;i<60;i++)__reviewTick();if(ESPHomeHA.snapshot().entities[0].cardSize!=='large')throw Error('Large card selection failed')")==0);
    review_capture(display,"hass-layout-editor","设备卡片大小编辑");
    assert(qpk_review_click_at(900,30)==0);assert(qpk_review_eval("for(let i=0;i<35;i++)__reviewTick()")==0);
    review_capture(display,"hass-mixed","灯光大卡与设备小卡组合");
    const char *readings[]={"12.0","24.5","48.0"};
    const char *units[]={NULL,"°C","%"};
    for(unsigned ri=0;ri<3;ri++) {
      lv_obj_t *label=review_label_find(g_desktop.screen,readings[ri]);assert(label);
      const lv_font_t *face=lv_obj_get_style_text_font(label,0);
      assert(g_home_font40&&face==g_home_font40);
      lv_point_t extent;
      lv_text_get_size(&extent,readings[ri],face,0,0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
      fprintf(stderr,"HA_READING_EXTENT %s %ldx%ld / %ldx%ld\n",readings[ri],(long)extent.x,(long)extent.y,(long)lv_obj_get_width(label),(long)lv_obj_get_height(label));
      assert(extent.x<=lv_obj_get_width(label)&&extent.y<=lv_obj_get_height(label));
      if(units[ri]){
        lv_obj_t *unit=review_label_find(g_desktop.screen,units[ri]);assert(unit);
        const lv_font_t *uf=lv_obj_get_style_text_font(unit,0);
        assert(uf==hass_qpk_font(20));
        assert(lv_obj_get_x(unit)-lv_obj_get_x(label)-extent.x==6);
        assert(lv_obj_get_y(label)+face->line_height-face->base_line==
               lv_obj_get_y(unit)+uf->line_height-uf->base_line);
      }
    }
    printf("HA_READING_FONT_OK %s: native 40px, units fit without clipping\n",g_desktop.light_theme?"light":"dark");
    assert(qpk_review_click_at(450,246)==0);assert(qpk_review_eval("for(let i=0;i<35;i++)__reviewTick();if(!ESPHomeHA.snapshot().detailAdvanced)throw Error('Mixed card More failed')")==0);
    review_capture(display,"hass-mixed-more","组合大卡的更多操作");
    assert(qpk_review_click_at(190,31)==0);assert(qpk_review_eval("for(let i=0;i<35;i++)__reviewTick();if(ESPHomeHA.snapshot().detailOpen)throw Error('More did not return to mixed layout')")==0);
    assert(qpk_review_click_at(900,30)==0);assert(qpk_review_eval("for(let i=0;i<35;i++)__reviewTick()")==0);
    assert(qpk_review_click_at(245,350)==0);assert(qpk_review_eval("for(let i=0;i<60;i++)__reviewTick();if(ESPHomeHA.snapshot().entities[2].cardSize!=='large')throw Error('Second large card selection failed')")==0);
    assert(qpk_review_click_at(900,30)==0);assert(qpk_review_eval("for(let i=0;i<35;i++)__reviewTick()")==0);
    assert(qpk_review_click_at(938,508)==0);assert(qpk_review_eval("for(let i=0;i<35;i++)__reviewTick();if(ESPHomeHA.snapshot().roomPage!==0)throw Error('Mixed first page failed')")==0);
    review_capture(display,"hass-mixed-two","两台设备各自选择大卡（第一页）");
    assert(qpk_review_click_at(982,508)==0);assert(qpk_review_eval("for(let i=0;i<40;i++)__reviewTick()")==0);
    review_capture(display,"hass-mixed-two-page2","两台设备各自选择大卡（第二页）");
    for(unsigned mi=0;mi<127;mi++) {
      glass_homeassistant(NULL);
      char command[500];snprintf(command,sizeof(command),"__haStates=[__miExamples[%u]];ESPHomeHA.refresh();for(let i=0;i<50;i++)__reviewTick();if(ESPHomeHA.snapshot().registeredCardVariants!==127)throw Error('Missing Mi variants')",mi);
      assert(qpk_review_eval(command)==0);assert(qpk_review_click_at(250,220)==0);
      assert(qpk_review_eval("for(let i=0;i<35;i++)__reviewTick();if(!ESPHomeHA.snapshot().detailOpen)throw Error('Mi card entry failed')")==0);
      char key[60],title[80];snprintf(key,sizeof(key),"hass-quick-%03u",mi+1);snprintf(title,sizeof(title),"米家展开卡片 · 属性组合 %03u",mi+1);
      review_capture(display,key,title);
      double cardw=440,cardh=round(cardw*mi_card_sizes[mi][1]/mi_card_sizes[mi][0]);
      if(cardh>394){cardh=394;cardw=round(cardh*mi_card_sizes[mi][0]/mi_card_sizes[mi][1]);}
      int morex=(int)round(1022-16-cardw/2),morey=(int)round(76+cardh-36*cardw/524);
      assert(qpk_review_click_at(morex,morey)==0);
      assert(qpk_review_eval("for(let i=0;i<35;i++)__reviewTick();if(!ESPHomeHA.snapshot().detailAdvanced)throw Error('Mi more operations entry failed')")==0);
      snprintf(key,sizeof(key),"hass-quick-%03u-more",mi+1);snprintf(title,sizeof(title),"米家更多操作 · 属性组合 %03u",mi+1);
      review_capture(display,key,title);
    }
    SHOT("apps","全部应用（第一页）",glass_apps(NULL));
    g_apps_page_index=1;glass_apps_render();review_capture(display,"apps-page2","全部应用（第二页）");
    struct qpk_entry_s *review_app=calloc(1,sizeof(*review_app));assert(review_app);
    strcpy(review_app->name,"示例快应用");strcpy(review_app->package,"review.fixture");strcpy(review_app->dir,"/offline-fixtures/qpk/example");
    g_desktop.qpk=review_app;g_desktop.nqpk=1;glass_apps_render();review_click("删除");review_capture(display,"apps-delete-confirm","删除应用确认");
    dialog_cancel();g_desktop.nqpk=0;g_desktop.qpk=NULL;free(review_app);
    SHOT("about","关于设备",about_clicked(NULL));
    SHOT("dialog","通用确认弹窗",qpk_show_dialog("操作提示\n这是离线审核示例，设备未连接。\n请确认页面排版与按钮样式。"));
    SHOT("live","网络直播",launch_bilibili(NULL));
    SHOT("local-video","本地视频",launch_local_video(NULL));
    SHOT("camera","相机（未连接设备）",launch_builtin_camera_qapp(NULL));
    assert(qpk_review_eval("__devices=[{id:'usb:0',generation:1,name:'USB 相机（审核示例）',transport:'usb',previewSupported:true}];CameraPreview.refresh();")==0);
    review_capture(display,"camera-device","相机设备与格式");
    SHOT("espdl-face","离线识别：人脸跟随",launch_builtin_espdl_qapp(NULL));
    assert(qpk_review_eval("EspDlApp.selectMode(0)")==0);review_capture(display,"espdl-classify","离线识别：物体分类");
    SHOT("recorder","录音机",launch_builtin_recorder_qapp(NULL));
    assert(qpk_review_eval("__reviewTick()")==0);review_capture(display,"recorder-list","录音文件列表");
    assert(qpk_review_eval("RecorderApp.select(0);RecorderApp.remove()")==0);review_capture(display,"recorder-delete","录音删除确认");
    assert(qpk_review_eval("RecorderApp.record();__reviewTick()")==0);review_capture(display,"recorder-recording","录音进行中");
    SHOT("hello","你好快应用",launch_builtin_qapp(NULL));
    review_click("Toast 提示");review_capture(display,"hello-toast","快应用消息提示");toast_cancel();
    review_click("对话框");review_capture(display,"hello-dialog","快应用对话框");dialog_cancel();
    SHOT("game2048","2048",launch_builtin_2048_qapp(NULL));
    SHOT("ouo","Grok Ball · 表情兼容检查",review_grok_expression_fixture());
    assert(qpk_review_eval("for(var i=0;i<12;i++)__reviewTick()")==0);review_capture(display,"ouo-animation","Grok Ball 动画帧");
    assert(qpk_review_eval("VelaMood.setEmotion('30');for(var i=0;i<84;i++)__reviewTick()")==0);
    review_capture(display,"ouo-thinking","Grok Ball 思考环带");
    for(unsigned frame=0;frame<60;frame++) {
      assert(qpk_review_eval("__reviewTick();__reviewTick()")==0);
      review_grok_frame(display,"orbit",frame);
    }
    assert(qpk_review_eval("VelaMood.setEmotion('02');VelaMood.engine.spin(2)")==0);
    for(unsigned frame=0;frame<60;frame++) {
      assert(qpk_review_eval("__reviewTick();__reviewTick()")==0);
      review_grok_frame(display,"spin",frame);
    }
    assert(qpk_review_eval("VelaMood.setEmotion('33')")==0);
    for(unsigned frame=0;frame<40;frame++) {
      assert(qpk_review_eval("__reviewTick();__reviewTick()")==0);
      review_grok_frame(display,"confetti",frame);
    }
    assert(qpk_review_eval("VelaMood.setEmotion('00')")==0);
    for(unsigned frame=0;frame<60;frame++) {
      assert(qpk_review_eval("__reviewTick();__reviewTick()")==0);
      review_grok_frame(display,"sleep",frame);
    }
    assert(qpk_review_eval("VelaMood.setEmotion('02')")==0);
    for(unsigned frame=0;frame<40;frame++) {
      char gaze[160];
      snprintf(gaze,sizeof(gaze),"VelaMood.setGaze(%.3f,%.3f);__reviewTick();__reviewTick()",sin(frame*0.1571)*0.8,cos(frame*0.1571)*0.55);
      assert(qpk_review_eval(gaze)==0);review_grok_frame(display,"gaze",frame);
    }
    assert(qpk_review_eval("VelaMood.setGaze(0,0);VelaMood.showPicker(0)")==0);
    review_capture(display,"ouo-picker-life","Grok Ball · 生命周期选择");
    assert(qpk_review_eval("__reviewTick();VelaMood.showPicker(1)")==0);
    review_capture(display,"ouo-picker-emotion","Grok Ball · 情绪选择");
    assert(qpk_review_eval("VelaMood.showPicker(2)")==0);
    review_capture(display,"ouo-picker-agent","Grok Ball · 工作状态选择");
    assert(qpk_review_click_at(260,210)==0);
    assert(qpk_review_eval("if(VelaMood.engine.emotionId!=='30')throw Error('picker event failed')")==0);
    assert(qpk_review_eval("VelaMood.handleAIMessage({emotionId:'30',tips:'正在分析任务 · 等待结果'});for(var i=0;i<16;i++)__reviewTick()")==0);
    review_capture(display,"ouo-ai-tips","Grok Ball · AI 状态提示");
    assert(qpk_review_eval("VelaMood.setEmotion('00');VelaMood.renderStatic()")==0);
    review_capture(display,"ouo-sleep","Grok Ball 睡眠");
    assert(qpk_review_eval("VelaMood.setEmotion('10');VelaMood.renderStatic()")==0);
    review_capture(display,"ouo-happy","Grok Ball 开心");
    const char *grok_ids[]={"00","01","02","03","04","05","06","07","10","11","12","13","14","15","16","17","18","19","20","21","30","31","32","33","34","35","36","37","38","39","40","41"};
    const char *grok_names[]={"睡眠","唤醒","待机放空","好奇","发呆","加载苏醒","休眠","抖动唤醒","开心","疑惑","失落","惊讶","害羞","疲惫","专注","慌张","无奈","满意","困惑","生气","思考中","接收任务","处理中忙碌","任务完成","出错","等待输入","联网加载","复述回忆","拒绝/受限","输出回复","检索资料","停止终止"};
    for(unsigned i=0;i<32;i++) {
      char script[160],key[40],title[96];
      snprintf(script,sizeof(script),"VelaMood.setEmotion('%s',true);for(var settle=0;settle<24;settle++)__reviewTick();VelaMood.renderStatic()",grok_ids[i]);
      assert(qpk_review_eval(script)==0);
      snprintf(key,sizeof(key),"ouo-state-%s",grok_ids[i]);snprintf(title,sizeof(title),"Grok Ball · %s",grok_names[i]);
      review_capture(display,key,title);
      for(unsigned frame=0;frame<16;frame++) {
        assert(qpk_review_eval("for(var tick=0;tick<4;tick++)__reviewTick()")==0);
        review_grok_frame(display,grok_ids[i],frame);
      }
    }
    assert(qpk_review_eval("var gp=ui.polygon([[0,0],[5,0],[5,5]],0);var pts=[[0,0],[5,0],[5,5]];Object.defineProperty(pts[1],0,{get:function(){ui.remove(gp);return 5}});var caught=false;try{ui.polygonSet(gp,pts,0)}catch(e){caught=true}if(!caught)throw Error('deleted polygon accepted')")==0);
    assert(qpk_review_eval("var badGradient=false;try{ui.polygon([[0,0],[5,0],[5,5]],0,255,0,[1,2,0,0,0,0,-1,0,0,1,16777215])}catch(e){badGradient=true}if(!badGradient)throw Error('negative gradient radius accepted');var gp=ui.polygon([[0,0],[5,0],[5,5]],0);var grad=[1,2,0,0,0,0,5,0,0,1,16777215];Object.defineProperty(grad,8,{get:function(){ui.remove(gp);return 0}});var caught=false;try{ui.polygonSet(gp,[[0,0],[5,0],[5,5]],0,255,0,grad)}catch(e){caught=true}if(!caught)throw Error('gradient deleted polygon accepted')")==0);
    SHOT("pet","大肥鱼桌宠设置",launch_builtin_dafeiyu_qapp(NULL));
    review_click("聊天");review_capture(display,"pet-input","桌宠聊天输入");
    panel_hide(NULL);review_music_fixture(2);
    glass_rebuild();glass_update(NULL);review_capture(display,"home-sample","首页（数据示例）");
  }
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("ALL_NATIVE_UI_OK");return 0;
}
