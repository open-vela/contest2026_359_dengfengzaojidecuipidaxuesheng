/* SPDX-License-Identifier: Apache-2.0 */
/* Shared native styling for the reviewed settings, performance and weather. */
static const lv_font_t *home_font(int size);
static lv_obj_t *home_text(lv_obj_t *,const char *,int,int,int,int,uint32_t);
static lv_obj_t *home_heading(lv_obj_t *,const char *,int,int,int,int,uint32_t);
static lv_obj_t *home_icon(lv_obj_t *,const char *,int,int,int,uint32_t);
static uint32_t detail_canvas(void){return glass_app_palette(g_desktop.light_theme).canvas;}
static uint32_t detail_paper(void){return glass_app_palette(g_desktop.light_theme).paper;}
static uint32_t detail_soft(void){return glass_app_palette(g_desktop.light_theme).soft;}
static uint32_t detail_line(void){return glass_app_palette(g_desktop.light_theme).line;}
static uint32_t detail_ink(void){return glass_app_palette(g_desktop.light_theme).ink;}
static uint32_t detail_muted(void){return glass_app_palette(g_desktop.light_theme).muted;}
static uint32_t detail_accent(void){return glass_app_palette(g_desktop.light_theme).accent;}
#include "glass_sidebar.h"
static void detail_page(lv_obj_t *card)
{
  lv_obj_clean(card);lv_obj_remove_style_all(card);
  lv_obj_set_pos(card,0,0);lv_obj_set_size(card,1024,GLASS_CONTENT_HEIGHT);
  lv_obj_remove_flag(card,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(card,lv_color_hex(detail_canvas()),0);
  lv_obj_set_style_bg_opa(card,LV_OPA_COVER,0);
}
static lv_obj_t *detail_tile(lv_obj_t *parent,int x,int y,int w,int h)
{
  lv_obj_t *tile=glass_box(parent,x,y,w,h,detail_paper(),GLASS_APP_CARD_RADIUS);
  glass_app_panel_style(tile,detail_paper(),detail_line());
  return tile;
}
static lv_obj_t *detail_text(lv_obj_t *parent,const char *text,int x,int y,int w,int size,uint32_t color)
{
  if(y<=60&&size>=20)return home_heading(parent,text,x,y,w,size,color);
  return home_text(parent,text,x,y,w,size,color);
}
static lv_obj_t *detail_button(lv_obj_t *parent,const char *text,int x,int y,int w,lv_event_cb_t cb,void *data)
{
  lv_obj_t *button=glass_button(parent,text,x,y,w,detail_soft(),cb,data);
  lv_obj_set_height(button,44);lv_obj_set_style_radius(button,12,0);
  lv_obj_set_style_border_width(button,0,0);
  lv_obj_set_style_text_font(lv_obj_get_child(button,0),home_font(14),0);
  lv_obj_set_style_text_color(lv_obj_get_child(button,0),lv_color_hex(detail_ink()),0);
  return button;
}
static lv_obj_t *detail_nav(lv_obj_t *parent,const char *text,const char *icon,int y,int w,lv_event_cb_t cb,void *data)
{
  lv_obj_t *row=detail_button(parent,"",20,y,w,cb,data);
  home_icon(row,icon,w==GLASS_SIDEBAR_ITEM_WIDTH?12:14,12,20,detail_accent());
  lv_obj_t *label=detail_text(row,text,w==GLASS_SIDEBAR_ITEM_WIDTH?42:46,12,w==GLASS_SIDEBAR_ITEM_WIDTH?w-50:w-80,14,detail_ink());
  lv_label_set_long_mode(label,LV_LABEL_LONG_DOT);
  lv_obj_set_height(label,home_font(14)->line_height);
  if(w!=GLASS_SIDEBAR_ITEM_WIDTH)home_icon(row,LV_SYMBOL_RIGHT,w-30,14,16,detail_muted());
  return row;
}
static lv_obj_t *detail_back_button(lv_obj_t *parent,const char *text,lv_event_cb_t back)
{
  lv_obj_update_layout(parent);
  lv_obj_t *button=detail_button(parent,text,GLASS_SIDEBAR_PAD,lv_obj_get_height(parent)-56,GLASS_SIDEBAR_ITEM_WIDTH,back,NULL);
  glass_app_frame_place_back(button,lv_obj_get_height(parent));
  glass_sidebar_item(button,false);return button;
}
static lv_obj_t *detail_sidebar(lv_obj_t *card,const char *name,const char *icon,lv_event_cb_t back)
{
  lv_obj_t *sidebar=glass_box(card,0,0,GLASS_SIDEBAR_WIDTH,GLASS_CONTENT_HEIGHT,detail_paper(),0);
  glass_sidebar_surface(sidebar);
  glass_box(card,GLASS_SIDEBAR_WIDTH-1,0,1,GLASS_CONTENT_HEIGHT,detail_line(),0);
  home_icon(card,icon,24,30,24,detail_accent());
  detail_text(card,name,58,30,94,20,detail_ink());
  return detail_back_button(card,glass_tr("返回桌面"),back);
}

/* Every settings page uses the same ordered navigation and one selected row. */
enum glass_settings_section {
  SETTINGS_WIFI=0, SETTINGS_FILES=1, SETTINGS_BLUETOOTH=2, SETTINGS_RESOURCES=3,
  SETTINGS_PANEL=4, SETTINGS_HOTSPOT=5, SETTINGS_MOUSE=6,
  SETTINGS_PORTAL=7, SETTINGS_ABOUT=8
};
static void glass_system_page_open(lv_event_t *e);
static void glass_settings_back(lv_event_t *e);
static void glass_portal_open(lv_event_t *e);
#ifdef CONFIG_SYSTEM_C6_DESKTOP
static void glass_wifi_network_open(lv_event_t *e);
#endif
static lv_obj_t *glass_settings_sidebar(lv_obj_t *card,enum glass_settings_section selected)
{
  static const struct {const char *name,*icon;enum glass_settings_section section;} items[]={
    {"控制面板",GLASS_ICON_GRID,SETTINGS_PANEL},
    {"Wi-Fi 网络",LV_SYMBOL_WIFI,SETTINGS_WIFI},
#ifdef CONFIG_SYSTEM_C6_DESKTOP
    {"热点配网",LV_SYMBOL_WIFI,SETTINGS_HOTSPOT},
#endif
    {"蓝牙设备",LV_SYMBOL_BLUETOOTH,SETTINGS_BLUETOOTH},
    {"系统性能",GLASS_ICON_SLIDERS,SETTINGS_RESOURCES},
    {"扫码配置",GLASS_ICON_QR,SETTINGS_PORTAL},
    {"关于设备",LV_SYMBOL_SETTINGS,SETTINGS_ABOUT}
  };
  g_desktop.settings_context=true;
  lv_event_cb_t parent=selected==SETTINGS_PANEL?glass_settings_back:glass_settings;
#ifdef CONFIG_SYSTEM_C6_DESKTOP
  if(selected==SETTINGS_HOTSPOT)parent=glass_wifi_network_open;
#endif
  lv_obj_t *back=detail_sidebar(card,glass_tr("设置"),GLASS_ICON_SLIDERS,parent);
  lv_label_set_text(lv_obj_get_child(back,0),selected==SETTINGS_PANEL?
    (glass_navigation_has_origin()?glass_tr("返回应用"):glass_tr("返回桌面")):selected==SETTINGS_HOTSPOT?glass_tr("返回网络"):glass_tr("返回设置"));
  for(unsigned i=0;i<sizeof(items)/sizeof(items[0]);i++){
    bool active=items[i].section==selected;
    const char *name=glass_i18n_language()==GLASS_LANGUAGE_EN&&items[i].section==SETTINGS_PANEL?"Controls":glass_tr(items[i].name);
    lv_obj_t *row=detail_nav(card,name,items[i].icon,80+i*48,
      GLASS_SIDEBAR_ITEM_WIDTH,active?NULL:glass_system_page_open,
      (void *)(uintptr_t)items[i].section);
    glass_sidebar_item(row,active);
    if(active){
      lv_obj_add_state(row,LV_STATE_CHECKED);
      for(unsigned n=0;n<lv_obj_get_child_count(row);n++){
        lv_obj_t *child=lv_obj_get_child(row,n);
        if(lv_obj_has_class(child,&lv_label_class))lv_obj_set_style_text_color(child,lv_color_hex(detail_accent()),0);
      }
    }
  }
  g_desktop.page_back=parent;
  return back;
}
