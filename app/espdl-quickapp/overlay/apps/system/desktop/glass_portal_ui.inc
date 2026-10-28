/* SPDX-License-Identifier: Apache-2.0 */
#include <lvgl/src/libs/qrcode/qrcodegen.h>
static lv_obj_t *portal_qr,*portal_state,*portal_code;
static lv_timer_t *portal_timer;
static uint16_t *portal_pixels;
static lv_obj_t *portal_page;
static lv_event_cb_t portal_return;
static bool portal_revoked;
static bool portal_hotspot;
static bool portal_local_address(struct in_addr *address)
{
#ifdef CONFIG_SYSTEM_C6_DESKTOP
  struct glass_recovery_snapshot_s recovery;
  if (glass_network_recovery_read(&recovery) == 0 &&
      recovery.state == GLASS_RECOVERY_READY && recovery.ap_running &&
      recovery.dhcp_ready)
    {
      memcpy(&address->s_addr, recovery.ap_ipv4, sizeof(address->s_addr));
      if (address->s_addr)
        {
          portal_hotspot = true;
          return true;
        }
    }
#endif
  portal_hotspot = false;
  return netlib_get_ipv4addr(C6NET_IFNAME, address) == 0 &&
         address->s_addr != 0;
}
static void glass_portal_open(lv_event_t *e);
static void glass_music(lv_event_t *e);
static void glass_portal_show(lv_event_cb_t back);
static void portal_refresh(lv_event_t *e)
{
  LV_UNUSED(e); char code[PORTAL_CODE_LENGTH+1];
  if(portal_session_refresh(code,sizeof(code))) {
    if(portal_state) lv_label_set_text(portal_state,glass_tr("令牌刷新失败，请重试"));
    return;
  }
  glass_portal_show(portal_return);
}
static void portal_go_back(lv_event_t *e) { if (portal_return) portal_return(e); }
static void portal_page_deleted(lv_event_t *e)
{
  LV_UNUSED(e); if(portal_timer) lv_timer_delete(portal_timer);
  portal_timer=NULL; portal_qr=portal_state=portal_code=NULL;
  portal_page=NULL;
  free(portal_pixels); portal_pixels=NULL;
}
static void portal_tick(lv_timer_t *timer)
{
  LV_UNUSED(timer); if(!glass_ui_can_paint(portal_state)) return;
  unsigned seconds=portal_session_seconds(); char text[160];
  if(seconds) snprintf(text,sizeof(text),portal_hotspot?
    glass_tr("令牌 %02u:%02u 内有效\n请连接设备热点并保留连接"):
    glass_tr("令牌 %02u:%02u 内有效\n手机、电脑与设备连接同一 Wi-Fi"),seconds/60,seconds%60);
  else {
    snprintf(text,sizeof(text),portal_revoked?glass_tr("连接已关闭\n刷新令牌后可重新连接"):glass_tr("令牌已过期，请刷新令牌\n已连接的网页仍可继续使用"));
    if(portal_qr) lv_obj_add_flag(portal_qr,LV_OBJ_FLAG_HIDDEN);
    if(portal_code) lv_label_set_text(portal_code,"--- ---");
  }
  lv_label_set_text(portal_state,text);
}
static void portal_revoke(lv_event_t *e) { LV_UNUSED(e); portal_session_close(); portal_revoked=true; portal_tick(NULL); }
static void glass_portal_open(lv_event_t *e)
{
  LV_UNUSED(e); glass_portal_show(glass_settings);
}
static void glass_portal_show(lv_event_cb_t back)
{
  bool from_apps=g_desktop.app_from_apps;
  lv_obj_t *card=panel_card(glass_tr("连接设备")); if(!card) return;
  bool settings=back==glass_settings;
#ifdef CONFIG_SYSTEM_C6_DESKTOP
  settings|=back==glass_wifi_recovery_open;
#endif
  detail_page(card);
  lv_obj_t *back_button=settings?glass_settings_sidebar(card,SETTINGS_PORTAL):
    detail_sidebar(card,glass_tr("连接"),GLASS_ICON_QR,portal_go_back);
  if(settings)lv_obj_remove_event_cb(back_button,glass_settings);
  if(settings)lv_obj_add_event_cb(back_button,portal_go_back,LV_EVENT_CLICKED,NULL);
  g_desktop.page_back=portal_go_back;
  if(!settings)glass_navigation_register(back,from_apps,0);
  detail_text(card,glass_tr("扫码配置"),216,26,784,28,detail_ink());
  portal_page=card; portal_return=back; portal_revoked=false;
  lv_obj_add_event_cb(card,portal_page_deleted,LV_EVENT_DELETE,NULL);
  const char *back_text=back==glass_settings?glass_tr("返回设置"):
#ifdef CONFIG_SYSTEM_C6_DESKTOP
    back==glass_wifi_recovery_open?glass_tr("返回配网热点"):
#endif
    glass_tr("返回应用");
  lv_label_set_text(lv_obj_get_child(back_button,0),back_text);
  int left=216,right=578,right_width=422;
  struct in_addr address; char code[PORTAL_CODE_LENGTH+1],url[96],display[64],ip[INET_ADDRSTRLEN];
  if(!portal_local_address(&address)) {
    glass_text(card,glass_tr("请先连接 Wi-Fi 或开启配网热点，再打开扫码配置"),left,160,784,24,theme_primary()); return;
  }
  int ret=portal_service_start(); if(!ret) ret=portal_session_open(code,sizeof(code));
  if(ret) { glass_text(card,glass_tr("配置服务暂时无法启动，请重试"),left,160,784,28,theme_primary()); return; }
  inet_ntop(AF_INET,&address,ip,sizeof(ip));
  snprintf(display,sizeof(display),"http://%s:%d",ip,PORTAL_PORT);
  snprintf(url,sizeof(url),"%s/#%s",display,code);
  lv_obj_t *box=glass_box(card,left,88,328,328,0xffffff,20);
  uint8_t encoded[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
  uint8_t scratch[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
  portal_pixels=malloc(288*288*sizeof(*portal_pixels));
  bool encoded_ok=qrcodegen_encodeText(url,scratch,encoded,qrcodegen_Ecc_MEDIUM,
    1,6,qrcodegen_Mask_AUTO,true);
  if(portal_pixels&&encoded_ok) {
    int modules=qrcodegen_getSize(encoded),scale=288/(modules+8);
    int margin=(288-modules*scale)/2;
    for(unsigned i=0;i<288*288;i++) portal_pixels[i]=0xffff;
    for(int y=0;y<modules;y++) for(int x=0;x<modules;x++)
      if(qrcodegen_getModule(encoded,x,y)) for(int dy=0;dy<scale;dy++)
        for(int dx=0;dx<scale;dx++) portal_pixels[(margin+y*scale+dy)*288+margin+x*scale+dx]=0x0000;
    portal_qr=lv_canvas_create(box);
    lv_canvas_set_buffer(portal_qr,portal_pixels,288,288,LV_COLOR_FORMAT_RGB565);
    lv_obj_center(portal_qr);
  } else glass_text(box,glass_tr("二维码暂时不可用\n请用右侧令牌连接"),30,120,268,20,0x39394a);
  lv_obj_t *caption=glass_text(card,glass_tr("手机扫码，直接连接"),left,432,328,20,theme_secondary());
  lv_obj_set_style_text_align(caption,LV_TEXT_ALIGN_CENTER,0);
  glass_text(card,glass_tr("电脑输入令牌连接"),right,90,right_width,28,theme_primary());
  glass_text(card,glass_tr("1  在浏览器打开"),right,141,right_width,16,theme_secondary());
  glass_text(card,display,right,168,right_width,20,theme_primary());
  lv_obj_t *code_box=glass_box(card,right,213,right_width,124,theme_surface(),16);
  glass_text(code_box,glass_tr("2  输入连接令牌"),20,14,right_width-40,16,theme_secondary());
  char grouped[8]; snprintf(grouped,sizeof(grouped),"%.3s %.3s",code,code+3);
  portal_code=glass_number(code_box,grouped,20,43,right_width-40,false,theme_primary());
  portal_state=glass_text(card,"",right,352,right_width,20,theme_secondary());
  glass_button(card,glass_tr("刷新令牌"),right,418,198,theme_surface(),portal_refresh,NULL);
  glass_button(card,glass_tr("断开全部"),800,418,200,theme_surface(),portal_revoke,NULL);
  portal_timer=lv_timer_create(portal_tick,1000,NULL); portal_tick(NULL);
}
