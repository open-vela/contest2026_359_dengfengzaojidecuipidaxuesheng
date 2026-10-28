/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <lvgl/lvgl.h>
#include <stdlib.h>
#define GLASS_TAP_MOVE_LIMIT 12
#define GLASS_PAGE_SWIPE_LIMIT 48
struct glass_swipe_tracker {lv_indev_t *input;lv_point_t start;bool tracking;};
/* Parent grids receive bubbled pointer events from guarded tiles. Recognition
 * is based on the released stroke, including slow movement and blank gaps. */
static inline lv_dir_t glass_swipe_direction(lv_event_t *event,struct glass_swipe_tracker *track)
{
  lv_event_code_t code=lv_event_get_code(event);
  if(code!=LV_EVENT_PRESSED&&code!=LV_EVENT_PRESS_LOST&&code!=LV_EVENT_RELEASED)return LV_DIR_NONE;
  lv_indev_t *input=lv_event_get_indev(event);
  if(!input||lv_indev_get_type(input)!=LV_INDEV_TYPE_POINTER)return LV_DIR_NONE;
  if(code==LV_EVENT_PRESSED){track->input=input;lv_indev_get_point(input,&track->start);track->tracking=true;return LV_DIR_NONE;}
  if(code==LV_EVENT_PRESS_LOST){track->tracking=false;return LV_DIR_NONE;}
  if(!track->tracking||track->input!=input)return LV_DIR_NONE;
  track->tracking=false;lv_point_t point;lv_indev_get_point(input,&point);
  int dx=point.x-track->start.x,dy=point.y-track->start.y;
  if(abs(dx)>=GLASS_PAGE_SWIPE_LIMIT&&abs(dx)>2*abs(dy))return dx<0?LV_DIR_LEFT:LV_DIR_RIGHT;
  if(abs(dy)>=GLASS_PAGE_SWIPE_LIMIT&&abs(dy)>2*abs(dx))return dy<0?LV_DIR_TOP:LV_DIR_BOTTOM;
  return LV_DIR_NONE;
}
struct glass_input_guard {
  lv_indev_t *input;
  lv_point_t start;
  bool tracking,moved;
};
/* Native and QuickJS click targets share this policy. Scroll, slider and
 * touch-release events remain available to the controls that own them. */
static void glass_input_guard_event(lv_event_t *event)
{
  if(lv_event_get_target(event)!=lv_event_get_current_target(event))return;
  struct glass_input_guard *guard=lv_event_get_user_data(event);
  lv_event_code_t code=lv_event_get_code(event);
  if(code==LV_EVENT_DELETE){free(guard);return;}
  if(code!=LV_EVENT_PRESSED&&code!=LV_EVENT_PRESSING&&code!=LV_EVENT_RELEASED&&
     code!=LV_EVENT_PRESS_LOST&&code!=LV_EVENT_CLICKED&&code!=LV_EVENT_SHORT_CLICKED&&
     code!=LV_EVENT_LONG_PRESSED&&code!=LV_EVENT_LONG_PRESSED_REPEAT)return;
  /* Intentional programmatic navigation has no input parameter. */
  lv_indev_t *input=lv_event_get_indev(event);
  if(!input||lv_indev_get_type(input)!=LV_INDEV_TYPE_POINTER)return;
  if(code==LV_EVENT_PRESSED){
    guard->input=input;guard->tracking=true;guard->moved=false;
    lv_indev_get_point(input,&guard->start);return;
  }
  if(!guard->tracking||guard->input!=input)return;
  lv_point_t point;lv_indev_get_point(input,&point);
  if(abs(point.x-guard->start.x)>=GLASS_TAP_MOVE_LIMIT||
     abs(point.y-guard->start.y)>=GLASS_TAP_MOVE_LIMIT)guard->moved=true;
  if(code==LV_EVENT_PRESS_LOST){guard->moved=true;return;}
  if(code==LV_EVENT_CLICKED||code==LV_EVENT_SHORT_CLICKED||
     code==LV_EVENT_LONG_PRESSED||code==LV_EVENT_LONG_PRESSED_REPEAT){
    if(guard->moved){lv_event_stop_processing(event);return;}
    if(code==LV_EVENT_CLICKED)guard->tracking=false;
  }
}
static inline bool glass_input_guard_install(lv_obj_t *object)
{
  if(!object)return false;
  for(unsigned i=0;i<lv_obj_get_event_count(object);i++)
    if(lv_event_dsc_get_cb(lv_obj_get_event_dsc(object,i))==glass_input_guard_event)return true;
  struct glass_input_guard *guard=calloc(1,sizeof(*guard));
  if(guard&&lv_obj_add_event_cb(object,glass_input_guard_event,LV_EVENT_ALL,guard)){
    lv_obj_add_flag(object,LV_OBJ_FLAG_PRESS_LOCK);return true;
  }
  free(guard);lv_obj_add_state(object,LV_STATE_DISABLED);
  lv_obj_remove_flag(object,LV_OBJ_FLAG_CLICKABLE);return false;
}
