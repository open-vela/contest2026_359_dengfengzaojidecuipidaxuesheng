/* SPDX-License-Identifier: Apache-2.0 */
/* Real production LVGL pages with synthetic local-only hotspot credentials. */
#define main all_ui_reference_main
#include "render_all_ui.c"
#undef main
#include "production_board_ui_labels.inc"

extern void review_recovery_fixture(const struct glass_recovery_snapshot_s *,
                                     const char *);
extern unsigned review_recovery_call_count(unsigned);
extern void review_wifi_ip_fixture(bool);
extern void review_wifi_snapshot_fixture(const struct c6_desktop_result *);

static void recovery_bounds(void)
{
  lv_obj_update_layout(g_desktop.screen);
  lv_area_t card, controls, qr, secret;
  lv_obj_get_coords(g_desktop.current_card,&card);
  lv_obj_get_coords(g_recovery_start,&controls);
  lv_obj_get_coords(g_recovery_qr_box,&qr);
  lv_obj_get_coords(g_recovery_password,&secret);
  assert(controls.y2<=card.y2&&controls.y1>secret.y2);
  assert(qr.x1>secret.x2&&qr.x2<=card.x2);
  assert(lv_obj_get_width(g_recovery_password)==410);
  assert(!lv_obj_has_flag(g_recovery_password,LV_OBJ_FLAG_CLICKABLE));
}

static void recovery_hidden_from_tools(void)
{
  cJSON *labels=cJSON_CreateArray();bool truncated=false;
  board_ui_labels(g_desktop.current_card,labels,&truncated);
  char *text=cJSON_PrintUnformatted(labels);assert(text);
  assert(!strstr(text,"Demo-7391-5930"));
  free(text);cJSON_Delete(labels);
}

int main(int argc,char **argv)
{
  assert(argc==3);review_out=argv[2];
  FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);
  size_t size=ftell(f);rewind(f);void *font=malloc(size);assert(font);
  assert(fread(font,1,size,f)==size);fclose(f);
  lv_init();lv_display_t *display=lv_display_create(1024,600);
  lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(display,review_fb,NULL,sizeof(review_fb),LV_DISPLAY_RENDER_MODE_FULL);
  lv_display_set_flush_cb(display,review_flush);
  g_desktop.font16=lv_tiny_ttf_create_data(font,size,16);
  g_desktop.font20=lv_tiny_ttf_create_data(font,size,20);
  g_desktop.font28=lv_tiny_ttf_create_data(font,size,28);
  g_desktop.font16->fallback=glass_icon_font(16);
  g_desktop.font20->fallback=glass_icon_font(20);
  g_desktop.font28->fallback=glass_icon_font(24);
  g_glass.font48=lv_tiny_ttf_create_data(font,size,48);glass_home_fonts_init(font,size);
  g_glass.reduced_motion=true;g_glass.temperature_c=-999;
  g_glass.humidity_pct=-1;g_glass.pm25=-1;
  g_desktop.screen=glass_box(lv_screen_active(),0,GLASS_STATUS_HEIGHT,
                            1024,GLASS_CONTENT_HEIGHT,0,0);
  lv_obj_remove_flag(g_desktop.screen,LV_OBJ_FLAG_SCROLLABLE);
  g_desktop.panel=glass_box(g_desktop.screen,0,0,1024,GLASS_CONTENT_HEIGHT,0,0);
  lv_obj_add_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN);
  char path[1024];snprintf(path,sizeof(path),"%s/screens.json",review_out);
  review_manifest=fopen(path,"w");assert(review_manifest);fputs("[\n",review_manifest);
  glass_status_create();
  for(unsigned theme=0;theme<2;theme++)
    {
      g_desktop.light_theme=theme==0;glass_rebuild();
      review_wifi_ip_fixture(false);
      struct glass_recovery_snapshot_s state={.state=GLASS_RECOVERY_OFF};
      review_recovery_fixture(&state,NULL);glass_wifi_page(NULL);
      review_click("热点配网");assert(g_recovery_timer&&!g_wifi_timer);
      recovery_bounds();
      assert(!lv_obj_has_state(g_recovery_start,LV_STATE_DISABLED));
      assert(lv_obj_has_state(g_recovery_stop,LV_STATE_DISABLED));
      assert(lv_obj_has_state(g_recovery_portal,LV_STATE_DISABLED));
      assert(!lv_textarea_get_text(g_recovery_password)[0]);
      review_capture(display,"recovery-off","热点配网 · 手动开启");
      unsigned starts=review_recovery_call_count(1);
      unsigned stops=review_recovery_call_count(2);
      review_click("开启热点");assert(review_recovery_call_count(1)==starts+1);
      assert(strstr(lv_label_get_text(g_recovery_status),"正在开启"));
      assert(lv_obj_has_state(g_recovery_start,LV_STATE_DISABLED));
      review_capture(display,"recovery-starting","热点配网 · 开启中");
      panel_hide(NULL);assert(!g_recovery_timer&&!g_recovery_password&&!g_recovery_pixels);
      assert(review_recovery_call_count(2)==stops);
      state=(struct glass_recovery_snapshot_s){.state=GLASS_RECOVERY_READY,
        .ap_running=true,.dhcp_ready=true,.ap_ipv4={172,31,240,1}};
      strcpy(state.ssid,"ESPClaw-Long-Hotspot-Name-123456");
      assert(strlen(state.ssid)==32);
      review_recovery_fixture(&state,"Demo-7391-5930");glass_wifi_recovery_page(NULL);
      recovery_bounds();assert(g_recovery_qr&&g_recovery_pixels);
      assert(!strcmp(lv_label_get_text(g_recovery_ssid),state.ssid));
      assert(!strcmp(lv_textarea_get_text(g_recovery_password),"Demo-7391-5930"));
      assert(strstr(lv_label_get_text(g_recovery_address),"172.31.240.1:8080"));
      assert(lv_obj_has_state(g_recovery_start,LV_STATE_DISABLED));
      assert(!lv_obj_has_state(g_recovery_stop,LV_STATE_DISABLED));
      assert(!lv_obj_has_state(g_recovery_portal,LV_STATE_DISABLED));
      recovery_hidden_from_tools();review_capture(display,"recovery-ready","热点配网 · 临时凭据和二维码");
      state.ethernet.sharing=true;review_recovery_fixture(&state,"Demo-7391-5930");
      glass_recovery_update(NULL);assert(strstr(lv_label_get_text(g_recovery_status),"有线共享已开启"));
      review_capture(display,"recovery-sharing","热点配网 · 有线共享");
      state.ethernet.sharing=false;state.ethernet.error=-EADDRINUSE;
      review_recovery_fixture(&state,"Demo-7391-5930");glass_recovery_update(NULL);
      assert(strstr(lv_label_get_text(g_recovery_status),"网段冲突"));
      state.ethernet.error=0;review_recovery_fixture(&state,"Demo-7391-5930");glass_recovery_update(NULL);
      uint16_t *pixels=g_recovery_pixels;glass_recovery_update(NULL);
      assert(g_recovery_pixels==pixels);
      /* AP alone is sufficient to open the existing authenticated portal. */
      review_click("打开网页配置");assert(portal_timer&&portal_hotspot);
      lv_obj_update_layout(g_desktop.screen);
      assert(review_label_find(g_desktop.panel,"http://172.31.240.1:8080"));
      assert(review_label_find(g_desktop.panel,"返回配网热点"));
      assert(!g_recovery_timer&&!g_recovery_pixels);
      review_capture(display,"recovery-portal","热点配网 · 无路由器也能打开网页");
      review_click("返回配网热点");assert(g_recovery_timer&&!portal_timer);
      state.busy=state.joining=true;review_recovery_fixture(&state,NULL);
      glass_recovery_update(NULL);assert(strstr(lv_label_get_text(g_recovery_status),"正在试连"));
      assert(lv_obj_has_state(g_recovery_stop,LV_STATE_DISABLED));
      assert(!lv_obj_has_state(g_recovery_portal,LV_STATE_DISABLED));
      review_capture(display,"recovery-joining","热点配网 · 等待试连与保存");
      state.busy=state.joining=false;state.associated=state.ipv4_ready=true;
      memcpy(state.ipv4,(uint8_t[]){192,168,9,42},4);
      review_recovery_fixture(&state,NULL);glass_recovery_update(NULL);
      assert(strstr(lv_label_get_text(g_recovery_status),"已连接并保存"));
      assert(review_recovery_call_count(2)==stops);
      review_capture(display,"recovery-connected","热点配网 · 配网成功保留热点");
      state.error=-ENOSPC;state.ipv4_ready=false;review_recovery_fixture(&state,NULL);
      glass_recovery_update(NULL);assert(strstr(lv_label_get_text(g_recovery_status),"试连或保存失败"));
      assert(!lv_obj_has_state(g_recovery_stop,LV_STATE_DISABLED));
      review_capture(display,"recovery-trial-error","热点配网 · 失败可重试");
      state.error=0;state.ap_error=-EIO;state.cleanup_error=-EBUSY;
      state.state=GLASS_RECOVERY_ERROR;review_recovery_fixture(&state,NULL);
      glass_recovery_update(NULL);assert(strstr(lv_label_get_text(g_recovery_status),"清理未完成"));
      assert(!lv_textarea_get_text(g_recovery_password)[0]);
      assert(lv_obj_has_flag(g_recovery_qr,LV_OBJ_FLAG_HIDDEN));
      review_capture(display,"recovery-cleanup-error","热点配网 · 清理失败可显式重试");
      review_click("关闭热点");assert(review_recovery_call_count(2)==stops+1);
      assert(strstr(lv_label_get_text(g_recovery_status),"正在关闭"));
      state=(struct glass_recovery_snapshot_s){.state=GLASS_RECOVERY_OFF};
      review_recovery_fixture(&state,NULL);glass_recovery_update(NULL);
      assert(!lv_obj_has_state(g_recovery_start,LV_STATE_DISABLED));
      panel_hide(NULL);assert(!g_recovery_timer&&!g_recovery_password&&!g_recovery_pixels);
      /* An AP worker result must not replace STA scan rows with an empty list. */
      struct c6_desktop_result wifi={.sequence=50,.operation=C6_DESKTOP_SCAN,.count=1};
      wifi.records[0].ssid_length=6;memcpy(wifi.records[0].ssid,"Router",6);
      review_wifi_snapshot_fixture(&wifi);glass_wifi_page(NULL);
      assert(g_wifi_result.count==1);
      wifi.sequence++;wifi.operation=C6_DESKTOP_SOFTAP_START;wifi.count=0;
      review_wifi_snapshot_fixture(&wifi);glass_wifi_update(NULL);
      assert(g_wifi_result.count==1);
      wifi.sequence++;wifi.operation=C6_DESKTOP_SOFTAP_STOP;
      review_wifi_snapshot_fixture(&wifi);glass_wifi_update(NULL);
      assert(g_wifi_result.count==1);panel_hide(NULL);
      for(unsigned i=0;i<30;i++)
        {
          glass_wifi_recovery_page(NULL);panel_hide(NULL);
          assert(!g_recovery_timer&&!g_recovery_password&&!g_recovery_pixels);
        }
      assert(review_recovery_call_count(0)==1);
      assert(review_recovery_call_count(2)==stops+1);
    }
  review_wifi_ip_fixture(true);fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("NETWORK_RECOVERY_UI_OK: both themes; manual start/stop; dynamic AP IP; local-only credentials; Wi-Fi QR; portal without STA; result ownership; no auto-stop; 60 reopen cycles");
  return 0;
}
