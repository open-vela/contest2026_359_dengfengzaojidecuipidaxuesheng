/* SPDX-License-Identifier: Apache-2.0 */
#define UNIFIED_DEVICE_NO_MAIN
#include "render_unified_devices.c"
extern void review_music_now_fixture(const struct music_now_playing *);
extern void review_lock_photo_fixture(int);
extern void review_cover_fixture(const char *);
extern void review_status_fixture(int,bool,bool);
static lv_point_t touch_point;
static lv_indev_state_t touch_state;
static unsigned leaked_touches;
static unsigned status_draws;
static void status_count_draw(lv_event_t *e){LV_UNUSED(e);status_draws++;}
static void touch_read(lv_indev_t *input,lv_indev_data_t *data)
{(void)input;data->point=touch_point;data->state=touch_state;}
static void touch(lv_indev_t *input,int x,int y,bool pressed)
{touch_point=(lv_point_t){x,y};touch_state=pressed?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;lv_tick_inc(20);lv_indev_read(input);}
static void underlying_click(lv_event_t *e){(void)e;leaked_touches++;}
static unsigned lock_object_count(lv_obj_t *root)
{unsigned count=1;for(unsigned i=0;i<lv_obj_get_child_count(root);i++)count+=lock_object_count(lv_obj_get_child(root,i));return count;}
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
  g_desktop.screen=glass_box(lv_screen_active(),0,GLASS_STATUS_HEIGHT,1024,GLASS_CONTENT_HEIGHT,0,0);lv_obj_remove_flag(g_desktop.screen,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_text_font(g_desktop.screen,zh_font(20),0);
  g_desktop.panel=glass_box(g_desktop.screen,0,0,1024,GLASS_CONTENT_HEIGHT,0x000000,0);lv_obj_add_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN);
  char path[1024];snprintf(path,sizeof(path),"%s/screens.json",review_out);review_manifest=fopen(path,"w");assert(review_manifest);fputs("[\n",review_manifest);




  g_glass.font72=lv_tiny_ttf_create_data(font,size,72);
  lv_indev_t *input=lv_indev_create();lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(input,touch_read);lv_indev_set_display(input,display);

  glass_edge_back_create();glass_status_create();
  lv_obj_add_event_cb(g_status_ui.bar,status_count_draw,LV_EVENT_DRAW_MAIN,NULL);
  for(unsigned theme=0;theme<2;theme++) {
    g_desktop.light_theme=theme!=0;glass_rebuild();glass_status_tick();
    review_net=(struct glass_status_network){0};glass_status_tick();
    assert(g_status_ui.network_state==0);
    glass_status_tick();review_capture(display,"home","桌面 · 顶部独立状态栏");
    review_net.joining=true;glass_status_tick();assert(g_status_ui.network_state==1);
    review_net.wifi=true;glass_status_tick();assert(g_status_ui.network_state==2);
    review_net.wired=true;glass_status_tick();assert(g_status_ui.network_state==3);
    review_net=(struct glass_status_network){.wifi=true};glass_status_tick();
    review_status_fixture(76,false,true);
    review_net.rssi_valid=true;review_net.rssi_dbm=-48;glass_status_tick();
    assert(g_status_ui.signal_level==4&&g_status_ui.bluetooth_count==1&&g_status_ui.battery_percent==76);
    review_capture(display,"island-strong","首页 · 合并状态岛");
    glass_status_tick();lv_refr_now(display);
    status_draws=0;
    for(unsigned idle=0;idle<120;idle++){glass_status_tick();lv_refr_now(display);}
    assert(status_draws==0);
    printf("STATUS_IDLE_OK theme=%u unchanged ticks=120 redraws=0\n",theme);
    review_status_fixture(76,true,true);glass_status_tick();
    assert(!lv_obj_has_flag(g_status_ui.charging,LV_OBJ_FLAG_HIDDEN));
    assert(strcmp(lv_label_get_text(g_status_ui.battery),LV_SYMBOL_CHARGE));
    review_capture(display,"island-charging","状态岛 · 电池与充电闪电同时显示");
    review_status_fixture(76,false,true);glass_status_tick();
    assert(lv_obj_has_flag(g_status_ui.charging,LV_OBJ_FLAG_HIDDEN));
    review_net.rssi_dbm=-87;glass_status_tick();assert(g_status_ui.signal_level==1);
    review_capture(display,"island-weak","首页 · 弱信号");
    struct music_now_playing now={.revision=41,.position_ms=42000,.duration_ms=204000,.volume=40,
      .state=MUSIC_PLAYING,.lyrics_state=MUSIC_LYRICS_READY,.can_skip=true,.lyrics_started=true};
    strcpy(now.name,"城市与晚风");strcpy(now.artist,"声动 · 原创演示");strcpy(now.lyrics[1],"把今天交给温柔的晚风");
    review_music_fixture(2);review_music_now_fixture(&now);glass_status_tick();
    const char *label=lv_label_get_text(g_status_ui.music);for(unsigned i=0;i<60;i++)glass_status_tick();
    assert(label==lv_label_get_text(g_status_ui.music));
    glass_music(NULL);glass_status_tick();lv_obj_t *original=g_desktop.current_card;
    lv_obj_update_layout(g_desktop.screen);lv_area_t a;lv_obj_get_coords(original,&a);
    assert(a.y1==32&&a.y2==599);
    glass_status_tick();review_capture(display,"music","音乐 · 播放器完整可见");
    unsigned top_children=lv_obj_get_child_count(lv_layer_top());
    assert(!lv_obj_has_flag(g_status_ui.bar,LV_OBJ_FLAG_CLICKABLE));
    assert(lv_obj_get_style_border_width(g_status_ui.island,0)==0);
    touch(input,980,16,true);
    assert(lv_obj_has_state(g_status_ui.island,LV_STATE_PRESSED));
    assert(lv_obj_get_style_border_width(g_status_ui.island,0)==1);
    review_capture(display,"island-pressed","状态岛 · 仅按下显示按钮圈");
    touch(input,980,16,false);
    lv_timer_handler(); /* Settings opens asynchronously after the input callback. */
    assert(!g_desktop.settings_rebuild_pending);
    assert(review_label_find(g_desktop.current_card,"控制面板"));
    assert(lv_obj_get_child_count(lv_layer_top())==top_children);
    original=g_desktop.current_card;
    review_capture(display,"island-settings","状态岛 · 点击进入设置");
    glass_lock_open(NULL);
    assert(g_lock_ui.root&&lv_obj_has_flag(g_status_ui.bar,LV_OBJ_FLAG_HIDDEN));
    glass_status_tick();review_capture(display,"lock","锁屏 · 保留完整 600 像素");lock_unlock(NULL);
    assert(!lv_obj_has_flag(g_status_ui.bar,LV_OBJ_FLAG_HIDDEN)&&original==g_desktop.current_card);
    panel_hide(NULL);struct music_now_playing after;glass_music_now_playing(&after);assert(after.state==MUSIC_PLAYING);
    glass_settings(NULL);glass_status_tick();review_capture(display,"settings","设置 · 独立内容区域");
    glass_resources_page(NULL);glass_status_tick();review_capture(display,"performance","性能 · 底部数据不裁切");
    glass_weather_open(NULL);glass_status_tick();review_capture(display,"weather","天气 · 顶栏布局");
    glass_chat_show(false);glass_status_tick();review_capture(display,"chat","聊天 · 输入入口保留");
    chat_ui_editor_open(NULL);glass_status_tick();review_capture(display,"chat-input","聊天 · 键盘完整显示");
    panel_hide(NULL);launch_bilibili(NULL);glass_status_tick();review_capture(display,"live","直播 · 可用区域");
    panel_hide(NULL);launch_local_video(NULL);glass_status_tick();review_capture(display,"video","视频 · 可用区域");
    panel_hide(NULL);qpk_review_hass_fixture(true);glass_homeassistant(NULL);
    assert(qpk_review_eval("for(let i=0;i<40;i++)__reviewTick()")==0);
    lv_area_t root;qpk_review_root_area(&root);assert(root.y1>=32&&root.y2<=599);
    glass_status_tick();review_capture(display,"homeassistant","米家 HA · 按实际内容高度排版");
    panel_hide(NULL);
    for(unsigned i=0;i<20;i++){touch(input,980,16,true);touch(input,980,16,false);glass_lock_open(NULL);lock_unlock(NULL);}
    assert(!g_lock_ui.root);
  }
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("STATUS_NATIVE_OK: both themes; real RSSI levels; Bluetooth count; battery capacity; pressed-only island outline; click settings; background playback; lock visibility; 40 reopen cycles");return 0;
}
