/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "glass_app_theme.h"
/* Shared geometry for app navigation; data panels keep their own heights. */
#define GLASS_SIDEBAR_WIDTH 164
#define GLASS_SIDEBAR_PAD 12
#define GLASS_SIDEBAR_ITEM_WIDTH 140
#define GLASS_SIDEBAR_ITEM_HEIGHT 44
#define GLASS_SIDEBAR_GAP 8
#define GLASS_SIDEBAR_RADIUS 12
#if GLASS_SIDEBAR_WIDTH != GLASS_APP_SIDEBAR_WIDTH || \
    GLASS_SIDEBAR_ITEM_WIDTH != GLASS_APP_SIDEBAR_ITEM_WIDTH || \
    GLASS_SIDEBAR_ITEM_HEIGHT != GLASS_APP_SIDEBAR_ITEM_HEIGHT
#error "Application navigation geometry differs from the shared UI contract"
#endif

static struct {
  lv_obj_t *source,*cursor;
  lv_point_t start;
  bool moved;
} g_sidebar_drag;

static void glass_sidebar_drag_clear(void)
{
  lv_obj_t *cursor=g_sidebar_drag.cursor;
  memset(&g_sidebar_drag,0,sizeof(g_sidebar_drag));
  if(cursor)lv_obj_delete(cursor);
}
static void glass_sidebar_cursor_deleted(lv_event_t *e)
{
  if(g_sidebar_drag.cursor==lv_event_get_target(e))g_sidebar_drag.cursor=NULL;
}
static lv_obj_t *glass_sidebar_nearest(lv_obj_t *parent,int y,int *low,int *high)
{
  lv_obj_t *nearest=NULL;int distance=INT_MAX;
  *low=INT_MAX;*high=INT_MIN;
  for(unsigned i=0;i<lv_obj_get_child_count(parent);i++){
    lv_obj_t *row=lv_obj_get_child(parent,i);
    if(row==g_sidebar_drag.cursor||!lv_obj_is_visible(row)||
       !lv_obj_has_flag(row,LV_OBJ_FLAG_CLICKABLE)||lv_obj_has_state(row,LV_STATE_DISABLED)||
       lv_obj_get_style_x(row,0)!=GLASS_SIDEBAR_PAD||
       lv_obj_get_style_width(row,0)!=GLASS_SIDEBAR_ITEM_WIDTH||
       lv_obj_get_style_height(row,0)!=GLASS_SIDEBAR_ITEM_HEIGHT)continue;
    int top=lv_obj_get_style_y(row,0),delta=abs(y-top-GLASS_SIDEBAR_ITEM_HEIGHT/2);
    if(top<*low)*low=top;if(top>*high)*high=top;
    if(delta<distance){distance=delta;nearest=row;}
  }
  return nearest;
}
static void glass_sidebar_drag_event(lv_event_t *e)
{
  lv_obj_t *row=lv_event_get_target(e);lv_event_code_t code=lv_event_get_code(e);
  if(code==LV_EVENT_DELETE){if(row==g_sidebar_drag.source)glass_sidebar_drag_clear();return;}
  lv_indev_t *input=lv_indev_active();
  if(!input||lv_indev_get_type(input)!=LV_INDEV_TYPE_POINTER)return;
  if(code==LV_EVENT_PRESSED){
    glass_sidebar_drag_clear();g_sidebar_drag.source=row;
    lv_indev_get_point(input,&g_sidebar_drag.start);return;
  }
  if(row!=g_sidebar_drag.source)return;
  if(code==LV_EVENT_PRESS_LOST){glass_sidebar_drag_clear();return;}
  if(code!=LV_EVENT_PRESSING&&code!=LV_EVENT_RELEASED)return;
  lv_point_t point;lv_indev_get_point(input,&point);
  if(abs(point.y-g_sidebar_drag.start.y)>=5)g_sidebar_drag.moved=true;
  if(!g_sidebar_drag.moved){if(code==LV_EVENT_RELEASED)glass_sidebar_drag_clear();return;}
  lv_obj_t *parent=lv_obj_get_parent(row);lv_area_t area;lv_obj_get_coords(parent,&area);
  int low,high,y=point.y-area.y1;
  lv_obj_t *target=glass_sidebar_nearest(parent,y,&low,&high);
  if(!target){glass_sidebar_drag_clear();return;}
  if(code==LV_EVENT_RELEASED){
    bool inside=point.x>=area.x1&&point.x<area.x1+GLASS_SIDEBAR_WIDTH;
    /* Reset the actual pointer before dispatch: LVGL must not synthesize a
     * second click on the originally pressed row after this callback. */
    lv_indev_reset(input,NULL);lv_obj_remove_state(row,LV_STATE_PRESSED);
    glass_sidebar_drag_clear();
    if(inside)lv_obj_send_event(target,LV_EVENT_CLICKED,NULL);
    return; /* Target callback may have destroyed the entire page. */
  }
  if(!g_sidebar_drag.cursor){
    lv_obj_t *cursor=lv_obj_create(parent);g_sidebar_drag.cursor=cursor;
    lv_obj_remove_style_all(cursor);lv_obj_remove_flag(cursor,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(cursor,GLASS_SIDEBAR_ITEM_WIDTH,GLASS_SIDEBAR_ITEM_HEIGHT);
    lv_obj_set_style_radius(cursor,GLASS_SIDEBAR_RADIUS,0);
    lv_obj_set_style_bg_color(cursor,lv_color_hex(detail_accent()),0);
    lv_obj_set_style_bg_opa(cursor,LV_OPA_20,0);
    lv_obj_set_style_border_width(cursor,1,0);lv_obj_set_style_border_color(cursor,lv_color_hex(detail_accent()),0);
    lv_obj_add_event_cb(cursor,glass_sidebar_cursor_deleted,LV_EVENT_DELETE,NULL);
  }
  int top=y-GLASS_SIDEBAR_ITEM_HEIGHT/2;if(top<low)top=low;if(top>high)top=high;
  lv_obj_set_pos(g_sidebar_drag.cursor,GLASS_SIDEBAR_PAD,top);
}
static void glass_sidebar_track(lv_obj_t *item)
{
  for(unsigned i=0;i<lv_obj_get_event_count(item);i++)
    if(lv_event_dsc_get_cb(lv_obj_get_event_dsc(item,i))==glass_sidebar_drag_event)return;
  lv_obj_add_flag(item,LV_OBJ_FLAG_PRESS_LOCK);
  lv_obj_remove_flag(item,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_SCROLL_CHAIN_VER);
  lv_obj_add_event_cb(item,glass_sidebar_drag_event,LV_EVENT_ALL,NULL);
}

static void glass_sidebar_surface(lv_obj_t *sidebar)
{
  lv_obj_set_width(sidebar,GLASS_SIDEBAR_WIDTH);
  lv_obj_set_style_bg_color(sidebar,lv_color_hex(detail_paper()),0);
  lv_obj_set_style_bg_opa(sidebar,LV_OPA_COVER,0);
  lv_obj_set_style_radius(sidebar,0,0);
  lv_obj_set_style_border_width(sidebar,0,0);
  lv_obj_set_style_shadow_width(sidebar,0,0);
}

static void glass_sidebar_item(lv_obj_t *item,bool selected)
{
  glass_sidebar_track(item);
  lv_obj_set_x(item,GLASS_SIDEBAR_PAD);
  lv_obj_set_size(item,GLASS_SIDEBAR_ITEM_WIDTH,GLASS_SIDEBAR_ITEM_HEIGHT);
  lv_obj_set_style_radius(item,GLASS_SIDEBAR_RADIUS,0);
  lv_obj_set_style_border_width(item,0,0);
  lv_obj_set_style_shadow_width(item,0,0);
  lv_obj_set_style_bg_opa(item,LV_OPA_COVER,0);
  lv_obj_set_style_bg_color(item,lv_color_hex(selected?detail_soft():detail_paper()),0);
  lv_obj_set_style_bg_color(item,lv_color_hex(detail_soft()),LV_STATE_PRESSED);
  lv_obj_t *label=lv_obj_get_child(item,0);
  if(label&&lv_obj_has_class(label,&lv_label_class)){
    lv_obj_set_style_text_font(label,home_font(14),0);
    lv_obj_set_style_text_color(label,lv_color_hex(selected?detail_accent():detail_ink()),0);
    lv_obj_set_width(label,GLASS_SIDEBAR_ITEM_WIDTH-24);
    lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_LEFT,0);
    lv_label_set_long_mode(label,LV_LABEL_LONG_DOT);
    lv_obj_align(label,LV_ALIGN_LEFT_MID,12,0);
  }
}
