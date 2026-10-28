#define FILE_GESTURES_NO_MAIN
#include "render_file_gestures.c"
static unsigned activated;
static void counted(lv_event_t *event){(void)event;activated++;}
static void guarded_drag(lv_obj_t *button)
{
  assert(button);activated=0;lv_obj_add_event_cb(button,counted,LV_EVENT_CLICKED,NULL);
  lv_obj_t *owner=g_desktop.current_card;
  for(unsigned axis=0;axis<2;axis++){
    lv_obj_update_layout(g_desktop.screen);lv_area_t area;lv_obj_get_coords(button,&area);
    stroke_at((area.x1+area.x2)/2,(area.y1+area.y2)/2,axis?0:24,axis?24:0,4);
    assert(!activated&&g_desktop.current_card==owner);
  }
}
int main(int argc,char **argv)
{
  lv_display_t *display=review_pointer_init(argc,argv);
  extern void review_album_fixture(int);
  extern void review_ble_snapshot_fixture(const struct glass_ble_state *);
  extern void review_hid_snapshot_fixture(const struct glass_ble_hid_state *);
  extern unsigned review_ble_call_count(unsigned);
  for(unsigned theme=0;theme<2;theme++){
    g_desktop.light_theme=theme!=0;glass_rebuild();
    glass_apps(NULL);settle();guarded_drag(g_apps_slots[0].tile);assert(g_desktop.apps_page);
    stroke_at(420,190,-60,0,20);assert(g_apps_page_index==1);
    stroke_at(420,190,60,0,20);assert(g_apps_page_index==0);
    review_capture(display,"apps-drag","应用网格拖动不启动应用");
    panel_hide(NULL);review_album_fixture(1);launch_local_video(NULL);settle();
    guarded_drag(review_find(g_desktop.panel,"相机"));review_capture(display,"album-drag","相册拖动不进入文件夹");
    panel_hide(NULL);
    struct glass_ble_state state={.started=true,.ready=true,.scanning=true,.scan_generation=7,.generation=10,.count=1};
    state.devices[0].address[0]=1;state.devices[0].address[5]=0xc2;
    state.devices[0].connectable_known=state.devices[0].connectable=true;
    strcpy(state.devices[0].name,"滑动不连接");
    struct glass_ble_hid_state hid={.host_ready=true};review_hid_snapshot_fixture(&hid);
    review_ble_snapshot_fixture(&state);glass_bluetooth_page(NULL);settle();
    unsigned connects=review_ble_call_count(2);guarded_drag(g_ble_list->rows[0].root);
    assert(review_ble_call_count(2)==connects);review_capture(display,"bluetooth-drag","设备列表拖动不连接");
    panel_hide(NULL);review_music_fixture(1);glass_music(NULL);settle();
    guarded_drag(review_callback_find(g_music_ui.root,music_ui_play));
    // Intentional slider movement remains available.
    lv_area_t a;lv_obj_get_coords(g_music_ui.volume,&a);
    sample(a.x1+4,(a.y1+a.y2)/2,true);sample(a.x2-2,(a.y1+a.y2)/2,true);
    assert(lv_slider_get_value(g_music_ui.volume)>60);
    sample(a.x2-2,(a.y1+a.y2)/2,false);settle();review_capture(display,"music-drag","音乐拖动不播放，音量仍可拖动");
    panel_hide(NULL);lv_obj_t *root=qapp_page_prepare("点击保护");assert(root);
    const char *js="globalThis.hits=0;ui.button('动作',200,120,240,100,function(){hits++;});";
    assert(!qpk_runtime_launch(root,"Guard","guard.fixture","1","builtin:/guard.js",js,strlen(js),zh_font,show_toast,qpk_show_dialog,qpk_show_unresponsive));
    qapp_page_commit(root);settle();guarded_drag(review_find(g_desktop.panel,"动作"));
    assert(!qpk_review_eval("if(hits!==0)throw Error('drag clicked JS button')"));
    lv_obj_t *button=review_find(g_desktop.panel,"动作");lv_obj_get_coords(button,&a);
    stroke_at((a.x1+a.x2)/2,(a.y1+a.y2)/2,3,2,1);
    assert(!qpk_review_eval("if(hits!==1)throw Error('tap blocked')"));
    review_capture(display,"quickapp-drag","快应用区分点击与拖动");
    panel_hide(NULL);glass_settings(NULL);settle();
    stroke_at(64,GLASS_STATUS_HEIGHT+102,0,48,4);
    assert(g_wifi_ssid&&g_desktop.settings_context);
    panel_hide(NULL);
  }
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("SWIPE_SAFETY_OK: both themes; app grid, album folders, BLE rows, music tracks and QuickJS buttons reject drags; taps, volume sliders and sidebar drag selection retained");
}
