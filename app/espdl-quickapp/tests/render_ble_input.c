/* SPDX-License-Identifier: Apache-2.0 */
#define main all_ui_reference_main
#include "render_all_ui.c"
#undef main
extern void review_wifi_snapshot_fixture(const struct c6_desktop_result *);
extern bool review_wifi_request_matches(enum c6_desktop_operation,const char *,const char *);
extern void review_hid_snapshot_fixture(const struct glass_ble_hid_state *);

extern unsigned review_input_reports,review_input_ends,review_input_leases,review_input_clicks;
extern int review_input_buttons,review_input_x,review_input_y;
extern bool review_input_stale;
static lv_point_t test_point;
static void input_read(lv_indev_t *i,lv_indev_data_t *d)
{(void)i;d->point=test_point;d->state=LV_INDEV_STATE_RELEASED;}
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

  lv_indev_t *indev=lv_indev_create();lv_indev_set_type(indev,LV_INDEV_TYPE_POINTER);
  lv_indev_set_display(indev,display);lv_indev_set_read_cb(indev,input_read);
  for(unsigned theme=0;theme<2;theme++){
    g_desktop.light_theme=theme!=0;glass_rebuild();
    struct glass_ble_hid_state hid={.enabled=true,.connected=true,.encrypted=true,
      .mouse_notify=true,.host_ready=true,.epoch=17+theme};
    review_hid_snapshot_fixture(&hid);glass_mouse_open(NULL);check_full_page();
    assert(g_mouse_input_ready);test_point=(lv_point_t){600,300};lv_indev_read(indev);
    lv_obj_send_event(g_mouse_controls[0],LV_EVENT_PRESSED,indev);
    lv_obj_send_event(g_mouse_controls[0],LV_EVENT_LONG_PRESSED,indev);
    assert(g_mouse_dragging&&g_mouse_input_session&&review_input_buttons==1);
    unsigned leases=review_input_leases;
    for(unsigned i=0;i<60;i++){lv_tick_inc(200);glass_mouse_refresh(NULL);}
    assert(review_input_leases==leases+60&&g_mouse_dragging);
    test_point.x+=10;test_point.y-=5;lv_indev_read(indev);
    lv_obj_send_event(g_mouse_controls[0],LV_EVENT_PRESSING,indev);
    assert(review_input_buttons==1&&review_input_x==20&&review_input_y==-10);
    lv_obj_send_event(g_mouse_controls[0],LV_EVENT_RELEASED,indev);
    assert(!g_mouse_dragging&&!g_mouse_touching&&review_input_buttons==0);
    unsigned clicks=review_input_clicks;
    lv_obj_send_event(g_mouse_controls[0],LV_EVENT_PRESSED,indev);
    lv_obj_send_event(g_mouse_controls[0],LV_EVENT_RELEASED,indev);
    assert(review_input_clicks==clicks+1);
    unsigned ended=review_input_ends;
    lv_obj_send_event(g_mouse_controls[0],LV_EVENT_PRESS_LOST,NULL);
    assert(!g_mouse_input_session&&review_input_ends==ended+1);
    assert(!glass_mouse_submit(GLASS_MOUSE_MOVE,4,6,0));
    hid.epoch++;review_hid_snapshot_fixture(&hid);
    assert(!glass_mouse_submit(GLASS_MOUSE_MOVE,8,10,0));
    assert(g_mouse_input_epoch==hid.epoch&&review_input_ends==ended+2);
    unsigned reports=review_input_reports;review_input_stale=true;
    assert(glass_mouse_submit(GLASS_MOUSE_MOVE,12,14,0)==-ESTALE);
    assert(!g_mouse_input_session&&review_input_reports==reports);
    review_input_stale=false;assert(!glass_mouse_submit(GLASS_MOUSE_MOVE,16,18,0));
    assert(review_input_reports==reports+1); /* next gesture, no replay */
    review_capture(display,"mouse-input-ready","蓝牙触摸板 · 拖拽与生命周期回归");
    ended=review_input_ends;panel_hide(NULL);
    assert(!g_mouse_input_session&&!g_mouse_timer&&review_input_ends==ended+1);
    for(unsigned i=0;i<30;i++){
      glass_mouse_open(NULL);assert(!glass_mouse_submit(GLASS_MOUSE_MOVE,2,3,0));
      panel_hide(NULL);assert(!g_mouse_input_session&&!g_mouse_timer);
    }
  }
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("BLE_INPUT_UI_OK: native LVGL both themes; actual press/long-press/move/release callbacks; 12s heartbeat; tap; null-indev press-lost; epoch replacement; stale action not replayed; page deletion and 60 reopen cycles");
}
