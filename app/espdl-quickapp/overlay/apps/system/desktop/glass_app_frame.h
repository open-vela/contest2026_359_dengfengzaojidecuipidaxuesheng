/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "glass_app_controls.h"
#include "glass_input_guard.h"

/* Native and QuickJS hosts share one rail, safe area and back target. */
struct glass_app_frame { lv_obj_t *sidebar,*body,*back; };
static inline void glass_app_frame_select(lv_event_t *event)
{
  lv_obj_t *button=lv_event_get_target(event),*rail=lv_obj_get_parent(button);
  struct glass_app_palette p=glass_app_palette(glass_app_object_light(rail));
  for(unsigned i=2;i<lv_obj_get_child_count(rail);i++)
    lv_obj_set_style_bg_color(lv_obj_get_child(rail,i),lv_color_hex(lv_obj_get_child(rail,i)==button?p.soft:p.paper),0);
}
static inline void glass_app_frame_navigation(lv_obj_t *body,unsigned count)
{
  lv_obj_t *rail=lv_obj_get_user_data(body);if(!rail)return;
  lv_obj_t *parent=lv_obj_get_parent(body);lv_obj_update_layout(parent);
  int width=lv_obj_get_width(parent),height=lv_obj_get_height(parent);
  struct glass_app_palette p=glass_app_palette(glass_app_object_light(parent));
  lv_obj_set_pos(body,count?GLASS_APP_SIDEBAR_WIDTH:0,0);
  lv_obj_set_size(body,count?width-GLASS_APP_SIDEBAR_WIDTH:width,count?height:height-56);
  lv_obj_set_pos(rail,0,count?0:height-56);
  lv_obj_set_size(rail,count?GLASS_APP_SIDEBAR_WIDTH:width,count?height:56);
  lv_obj_set_style_bg_color(rail,lv_color_hex(count?p.paper:p.canvas),0);
  lv_obj_t *title=lv_obj_get_child(rail,0),*back=lv_obj_get_child(rail,1);
  if(count)lv_obj_remove_flag(title,LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(title,LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_pos(back,GLASS_APP_SIDEBAR_PAD,count?height-56:0);
  lv_obj_set_style_bg_color(back,lv_color_hex(count?p.paper:p.canvas),0);
  lv_obj_move_to_index(rail,-1);lv_obj_update_layout(parent);
}
static inline void glass_app_frame_place_back(lv_obj_t *back,int height)
{
  lv_obj_set_pos(back,GLASS_APP_SIDEBAR_PAD,height-56);
  lv_obj_set_size(back,GLASS_APP_SIDEBAR_ITEM_WIDTH,GLASS_APP_SIDEBAR_ITEM_HEIGHT);
}
static inline lv_obj_t *glass_app_frame_box(lv_obj_t *parent,int x,int y,int w,int h,uint32_t color)
{
  lv_obj_t *obj=lv_obj_create(parent);if(!obj)return NULL;
  lv_obj_remove_style_all(obj);lv_obj_set_pos(obj,x,y);lv_obj_set_size(obj,w,h);
  lv_obj_set_style_bg_color(obj,lv_color_hex(color),0);lv_obj_set_style_bg_opa(obj,LV_OPA_COVER,0);
  lv_obj_remove_flag(obj,LV_OBJ_FLAG_SCROLLABLE);return obj;
}
static inline struct glass_app_frame glass_app_frame_create(lv_obj_t *parent,const char *title,
  const lv_font_t *font,lv_event_cb_t exit,const char *caption)
{
  struct glass_app_frame frame={0};lv_obj_update_layout(parent);
  int width=lv_obj_get_width(parent),height=lv_obj_get_height(parent);
  struct glass_app_palette p=glass_app_palette(glass_app_object_light(parent));
  frame.sidebar=glass_app_frame_box(parent,0,0,GLASS_APP_SIDEBAR_WIDTH,height,p.paper);
  frame.body=glass_app_frame_box(parent,GLASS_APP_SIDEBAR_WIDTH,0,width-GLASS_APP_SIDEBAR_WIDTH,height,p.canvas);
  if(!frame.sidebar||!frame.body)return frame;
  lv_obj_t *label=lv_label_create(frame.sidebar);if(!label)return frame;
  lv_label_set_text(label,title);lv_obj_set_pos(label,24,30);lv_obj_set_width(label,128);
  lv_label_set_long_mode(label,LV_LABEL_LONG_DOT);lv_obj_set_style_text_font(label,font,0);
  lv_obj_set_style_text_color(label,lv_color_hex(p.ink),0);
  lv_obj_remove_flag(label,LV_OBJ_FLAG_CLICKABLE);
  frame.back=lv_button_create(frame.sidebar);if(!frame.back)return frame;
  glass_app_frame_place_back(frame.back,height);glass_app_control_style(frame.back,p.paper,p.line);
  lv_obj_set_style_pad_all(frame.back,0,0);
  glass_input_guard_install(frame.back);lv_obj_add_event_cb(frame.back,exit,LV_EVENT_CLICKED,NULL);
  label=lv_label_create(frame.back);if(!label){lv_obj_delete(frame.back);frame.back=NULL;return frame;}
  lv_label_set_text(label,caption);lv_obj_set_style_text_font(label,font,0);
  lv_obj_set_style_text_color(label,lv_color_hex(p.ink),0);lv_obj_align(label,LV_ALIGN_LEFT_MID,12,0);
  lv_obj_set_user_data(frame.body,frame.sidebar);
  glass_app_frame_navigation(frame.body,0);
  return frame;
}
