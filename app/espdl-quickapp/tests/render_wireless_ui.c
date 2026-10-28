/* SPDX-License-Identifier: Apache-2.0 */
#define main all_ui_reference_main
#include "render_all_ui.c"
#undef main
extern void review_wifi_snapshot_fixture(const struct c6_desktop_result *);
extern bool review_wifi_request_matches(enum c6_desktop_operation,const char *,const char *);
extern void review_hid_snapshot_fixture(const struct glass_ble_hid_state *);

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
  for(unsigned theme=0;theme<2;theme++){
    g_desktop.light_theme=theme!=0;glass_rebuild();
    struct c6_desktop_result wifi={.sequence=2,.operation=C6_DESKTOP_SCAN,.count=3};
    const char *names[]={"工作室网络", "Studio WiFi", "Guest Network"};
    for(unsigned i=0;i<3;i++){
      wifi.records[i].ssid_length=strlen(names[i]);
      memcpy(wifi.records[i].ssid,names[i],strlen(names[i]));
      wifi.records[i].rssi=-42-i*12;
    }
    review_wifi_snapshot_fixture(&wifi);glass_wifi_page(NULL);check_full_page();
    assert(review_wifi_request_matches(C6_DESKTOP_SCAN,NULL,NULL));
    assert(lv_obj_is_visible(g_wifi_list)&&!lv_obj_is_visible(g_wifi_form));
    review_capture(display,"wifi-list","Wi-Fi · 附近网络");
    lv_obj_t *row=lv_obj_get_child(g_wifi_list,0);
    lv_obj_send_event(row,LV_EVENT_CLICKED,NULL);lv_obj_update_layout(g_desktop.screen);
    assert(lv_obj_is_visible(g_wifi_form)&&!lv_obj_is_visible(g_wifi_list));
    assert(!strcmp(lv_textarea_get_text(g_wifi_ssid),names[0]));
    assert(lv_keyboard_get_textarea(g_wifi_keyboard)==g_wifi_password);
    assert(lv_textarea_get_password_mode(g_wifi_password));
    /* Candidate bar, fields and keyboard must occupy separate bands. */
    lv_obj_t *ime_bar=lv_obj_get_child(g_wifi_form,-1);
    assert(lv_obj_get_y(ime_bar)>lv_obj_get_y(g_wifi_password)+lv_obj_get_height(g_wifi_password));
    assert(lv_obj_get_y(ime_bar)+lv_obj_get_height(ime_bar)<lv_obj_get_y(g_wifi_keyboard));
    assert(lv_obj_get_y(g_wifi_keyboard)+lv_obj_get_height(g_wifi_keyboard)<lv_obj_get_height(g_wifi_form));
    review_capture(display,"wifi-password","Wi-Fi · 连接网络");
    lv_textarea_set_text(g_wifi_password,"offline-secret");
    lv_obj_send_event(g_wifi_connect,LV_EVENT_CLICKED,NULL);
    assert(review_wifi_request_matches(C6_DESKTOP_CONNECT,names[0],"offline-secret"));
    assert(!lv_textarea_get_text(g_wifi_password)[0]);
    wifi.sequence++;wifi.busy=true;wifi.operation=C6_DESKTOP_CONNECT;
    review_wifi_snapshot_fixture(&wifi);glass_wifi_update(NULL);
    assert(lv_obj_has_state(g_wifi_connect,LV_STATE_DISABLED));
    assert(lv_obj_has_state(g_wifi_ssid,LV_STATE_DISABLED));
    review_capture(display,"wifi-busy","Wi-Fi · 正在连接");
    wifi.sequence++;wifi.busy=false;wifi.error=-5;
    review_wifi_snapshot_fixture(&wifi);glass_wifi_update(NULL);
    assert(!lv_obj_has_state(g_wifi_connect,LV_STATE_DISABLED));
    assert(strstr(lv_label_get_text(g_wifi_status),"-5"));
    review_capture(display,"wifi-error","Wi-Fi · 连接失败");
    lv_textarea_set_text(g_wifi_password,"discard-me");review_click("返回列表");
    assert(!lv_textarea_get_text(g_wifi_password)[0]);
    review_click("手动添加");assert(!lv_textarea_get_text(g_wifi_ssid)[0]);
    assert(lv_keyboard_get_textarea(g_wifi_keyboard)==g_wifi_ssid);
    review_capture(display,"wifi-manual","Wi-Fi · 手动添加");
    review_click("返回列表");
    wifi.sequence++;wifi.error=0;wifi.operation=C6_DESKTOP_STATUS;
    wifi.link.ipv4_ready=true;wifi.link.ipv4[0]=192;wifi.link.ipv4[1]=168;wifi.link.ipv4[2]=1;wifi.link.ipv4[3]=9;
    review_wifi_snapshot_fixture(&wifi);glass_wifi_update(NULL);
    assert(!strcmp(lv_label_get_text(g_wifi_status),"连接成功 · IP: 192.168.1.9"));
    review_capture(display,"wifi-connected","Wi-Fi · 已获取地址");

    /* Accepted commands must progress automatically through association and
     * a DHCP lease belonging to this connection, without clicking query. */
    review_click("手动添加");
    lv_textarea_set_text(g_wifi_ssid,"Studio WiFi");
    lv_textarea_set_text(g_wifi_password,"offline-secret");
    review_click("连接网络");
    assert(g_wifi_pending==C6_DESKTOP_CONNECT);
    wifi.sequence++;wifi.operation=C6_DESKTOP_CONNECT;
    memset(&wifi.link,0,sizeof(wifi.link));
    review_wifi_snapshot_fixture(&wifi);glass_wifi_update(NULL);
    assert(review_wifi_request_matches(C6_DESKTOP_STATUS,NULL,NULL));
    assert(g_wifi_pending&&strstr(lv_label_get_text(g_wifi_status),"等待关联"));
    assert(lv_obj_has_state(g_wifi_scan,LV_STATE_DISABLED));
    wifi.sequence++;wifi.operation=C6_DESKTOP_STATUS;wifi.error=-EAGAIN;
    review_wifi_snapshot_fixture(&wifi);glass_wifi_update(NULL);
    assert(g_wifi_pending&&g_wifi_monitor);
    wifi.error=0;
    wifi.sequence++;wifi.operation=C6_DESKTOP_STATUS;
    wifi.link.initialized=wifi.link.associated=wifi.link.carrier_ready=true;
    wifi.link.generation=42;wifi.link.ipv4_ready=true;
    wifi.link.ipv4[0]=192;wifi.link.ipv4[1]=168;wifi.link.ipv4[2]=1;wifi.link.ipv4[3]=9;
    review_wifi_lease_ready=false;
    review_wifi_snapshot_fixture(&wifi);glass_wifi_update(NULL);
    assert(g_wifi_pending&&g_wifi_waiting_ip);
    assert(strstr(lv_label_get_text(g_wifi_status),"获取 IP"));
    review_capture(display,"wifi-dhcp","Wi-Fi · 等待本次连接的 IP");
    lv_tick_inc(1000);glass_wifi_update(NULL);
    assert(review_wifi_request_matches(C6_DESKTOP_STATUS,NULL,NULL));
    review_wifi_lease_ready=true;wifi.sequence++;
    review_wifi_snapshot_fixture(&wifi);glass_wifi_update(NULL);
    assert(!g_wifi_pending&&!lv_obj_has_state(g_wifi_connect,LV_STATE_DISABLED));
    assert(lv_obj_is_visible(g_wifi_list)&&!lv_obj_is_visible(g_wifi_form));
    assert(strstr(lv_label_get_text(g_wifi_status),"连接成功"));
    review_capture(display,"wifi-success","Wi-Fi · 自动连接成功");

    review_click("断开连接");
    wifi.sequence++;wifi.operation=C6_DESKTOP_DISCONNECT;
    review_wifi_snapshot_fixture(&wifi);glass_wifi_update(NULL);
    assert(g_wifi_pending==C6_DESKTOP_DISCONNECT);
    wifi.sequence++;wifi.operation=C6_DESKTOP_STATUS;
    memset(&wifi.link,0,sizeof(wifi.link));
    review_wifi_snapshot_fixture(&wifi);glass_wifi_update(NULL);
    assert(!g_wifi_pending&&!strcmp(lv_label_get_text(g_wifi_status),"Wi-Fi 已断开"));
    review_capture(display,"wifi-disconnected","Wi-Fi · 已确认断开");

    for(unsigned stage=0;stage<2;stage++){
      review_click("手动添加");lv_textarea_set_text(g_wifi_ssid,"Studio WiFi");
      review_click("连接网络");
      wifi.sequence++;wifi.operation=C6_DESKTOP_CONNECT;
      review_wifi_snapshot_fixture(&wifi);glass_wifi_update(NULL);
      wifi.sequence++;wifi.operation=C6_DESKTOP_STATUS;
      wifi.link.initialized=true;wifi.link.associated=stage!=0;
      review_wifi_snapshot_fixture(&wifi);glass_wifi_update(NULL);
      lv_tick_inc(stage?GLASS_WIFI_IP_MS:GLASS_WIFI_JOIN_MS);
      glass_wifi_update(NULL);
      assert(!g_wifi_pending&&!g_wifi_monitor);
      assert(!lv_obj_has_state(g_wifi_connect,LV_STATE_DISABLED));
      assert(strstr(lv_label_get_text(g_wifi_status),stage?"获取 IP 超时":"连接超时"));
      review_capture(display,stage?"wifi-ip-timeout":"wifi-join-timeout","Wi-Fi · 超时可重试");
      review_click("返回列表");
    }
    /* Navigation drops widgets, never the radio request or its deadline. */
    review_click("手动添加");lv_textarea_set_text(g_wifi_ssid,"Studio WiFi");
    review_click("连接网络");
    wifi.sequence++;wifi.operation=C6_DESKTOP_CONNECT;
    review_wifi_snapshot_fixture(&wifi);glass_wifi_update(NULL);
    review_click("返回桌面");
    assert(!g_wifi_timer&&!g_wifi_password&&g_wifi_pending);
    wifi.sequence++;wifi.operation=C6_DESKTOP_STATUS;
    wifi.link=(struct c6_desktop_link){.generation=43,.initialized=true,
      .associated=true,.carrier_ready=true,.ipv4_ready=true,.ipv4={192,168,1,9}};
    review_wifi_snapshot_fixture(&wifi);glass_wifi_page(NULL);
    assert(!g_wifi_pending&&strstr(lv_label_get_text(g_wifi_status),"连接成功"));
    review_click("返回桌面");assert(!g_wifi_timer&&!g_wifi_form&&!g_wifi_password);

    struct glass_ble_hid_state hid={.enabled=true,.advertising=true,.host_ready=true};
    review_hid_snapshot_fixture(&hid);glass_bluetooth_page(NULL);check_full_page();
    assert(g_ble_list);assert(!review_find(g_desktop.panel,"无线触摸板"));
    glass_mouse_open(NULL);assert(!g_ble_list);
    assert(!g_mouse_input_ready&&lv_obj_has_state(g_mouse_controls[0],LV_STATE_DISABLED));
    review_capture(display,"bluetooth-pairing","蓝牙 · 等待配对");
    hid.connected=true;hid.advertising=false;review_hid_snapshot_fixture(&hid);glass_mouse_refresh(NULL);
    assert(!g_mouse_input_ready&&lv_obj_has_state(g_mouse_controls[2],LV_STATE_DISABLED));
    review_capture(display,"bluetooth-securing","蓝牙 · 安全连接");
    hid.encrypted=true;hid.mouse_notify=true;review_hid_snapshot_fixture(&hid);glass_mouse_refresh(NULL);
    assert(g_mouse_input_ready&&!lv_obj_has_state(g_mouse_controls[0],LV_STATE_DISABLED));
    review_capture(display,"bluetooth-ready","蓝牙 · 触摸板就绪");
    hid.boot_mode=true;review_hid_snapshot_fixture(&hid);glass_mouse_refresh(NULL);
    assert(lv_obj_has_state(g_mouse_controls[1],LV_STATE_DISABLED));
    assert(!lv_obj_has_state(g_mouse_controls[0],LV_STATE_DISABLED));
    hid.suspended=true;review_hid_snapshot_fixture(&hid);glass_mouse_refresh(NULL);
    assert(!g_mouse_input_ready&&lv_obj_has_state(g_mouse_controls[0],LV_STATE_DISABLED));
    review_capture(display,"bluetooth-suspended","蓝牙 · 主机挂起");
    review_click("返回桌面");assert(!g_mouse_timer&&!g_mouse_status);
    /* The app-grid entry opens the identical production UI. */
    glass_mouse_open(NULL);check_full_page();assert(review_label_find(g_desktop.panel,"无线触摸板"));
    panel_hide(NULL);assert(!g_mouse_timer);
    for(unsigned i=0;i<20;i++){
      glass_wifi_page(NULL);panel_hide(NULL);glass_bluetooth_page(NULL);panel_hide(NULL);
      assert(!g_wifi_timer&&!g_mouse_timer&&!g_wifi_form&&!g_mouse_status&&!g_ble_list);
    }
  }
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("WIRELESS_UI_OK: both themes; automatic scan/select/manual/connect; accepted/association/current DHCP lease/success; polling/stale lease/EAGAIN; join and DHCP timeout/retry; confirmed disconnect; navigation in flight; password clearing; HID regression; 40 page roundtrips");
}
