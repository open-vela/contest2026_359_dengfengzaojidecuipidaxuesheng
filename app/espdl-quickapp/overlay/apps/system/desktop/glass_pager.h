/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "glass_motion_hooks.h"
#include "glass_input_guard.h"
#include <lvgl/lvgl.h>
#include <stdlib.h>

/* Quiet 28 px capsule, 6 px dots with a 20 px active pill and 40 px touch height.
 * A sliding window bounds native objects independently of the page count. */
#define GLASS_PAGER_SLOTS 5
struct glass_pager;
struct glass_pager_slot {
  struct glass_pager *owner;
  lv_obj_t *hit,*mark;
  unsigned page;
};
struct glass_pager {
  lv_obj_t *root,*face;
  struct glass_pager_slot slots[GLASS_PAGER_SLOTS];
  void (*select)(void *,unsigned);
  void *context;
  unsigned current,limit;
  uint32_t ink,muted;
  bool enabled;
};
static inline void glass_pager_clicked(lv_event_t *event)
{
  struct glass_pager_slot *slot=lv_event_get_user_data(event);
  struct glass_pager *pager=slot->owner;
  if(!pager->enabled||slot->page==pager->current)return;
  /* The selection callback may delete the entire owning page. */
  void (*select)(void *,unsigned)=pager->select;
  void *context=pager->context;unsigned page=slot->page;
  if(select)select(context,page);
}
static inline void glass_pager_deleted(lv_event_t *event)
{free(lv_event_get_user_data(event));}
static inline lv_obj_t *glass_pager_create(lv_obj_t *parent,uint32_t background,
                                   uint32_t ink,uint32_t muted,
                                   void (*select)(void *,unsigned),void *context)
{
  struct glass_pager *pager=calloc(1,sizeof(*pager));if(!pager)return NULL;
  lv_obj_t *root=lv_obj_create(parent);lv_obj_remove_style_all(root);
  lv_obj_remove_flag(root,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(root,40,40);lv_obj_set_user_data(root,pager);
  lv_obj_add_event_cb(root,glass_pager_deleted,LV_EVENT_DELETE,pager);
  pager->root=root;pager->ink=ink;pager->muted=muted;
  pager->select=select;pager->context=context;pager->limit=GLASS_PAGER_SLOTS;
  pager->face=lv_obj_create(root);lv_obj_remove_style_all(pager->face);
  lv_obj_remove_flag(pager->face,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_pos(pager->face,0,6);lv_obj_set_size(pager->face,40,28);
  lv_obj_set_style_bg_color(pager->face,lv_color_hex(background),0);
  lv_obj_set_style_bg_opa(pager->face,LV_OPA_COVER,0);
  lv_obj_set_style_radius(pager->face,14,0);
  lv_obj_add_flag(root,LV_OBJ_FLAG_HIDDEN);return root;
}
static inline void glass_pager_set_enabled(lv_obj_t *root,bool enabled)
{
  if(!root)return;struct glass_pager *pager=lv_obj_get_user_data(root);
  pager->enabled=enabled;lv_obj_set_style_opa(root,enabled?LV_OPA_COVER:LV_OPA_50,0);
  for(unsigned i=0;i<GLASS_PAGER_SLOTS;i++)if(pager->slots[i].hit){
    if(enabled)lv_obj_remove_state(pager->slots[i].hit,LV_STATE_DISABLED);
    else lv_obj_add_state(pager->slots[i].hit,LV_STATE_DISABLED);
  }
}
static inline void glass_pager_update_window(lv_obj_t *root,unsigned current,
                                      unsigned first,unsigned count,bool enabled)
{
  if(!root)return;struct glass_pager *pager=lv_obj_get_user_data(root);
  if(count>pager->limit)count=pager->limit;
  pager->current=current;
  if(count<=1)lv_obj_add_flag(root,LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(root,LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_width(root,16+count*24);lv_obj_set_width(pager->face,16+count*24);
  for(unsigned i=0;i<GLASS_PAGER_SLOTS;i++){
    struct glass_pager_slot *slot=&pager->slots[i];
    if(i>=count){if(slot->hit)lv_obj_add_flag(slot->hit,LV_OBJ_FLAG_HIDDEN);continue;}
    bool created=!slot->hit;
    if(!slot->hit){
      slot->owner=pager;slot->hit=lv_button_create(root);lv_obj_remove_style_all(slot->hit);
      lv_obj_set_pos(slot->hit,8+i*24,0);lv_obj_set_size(slot->hit,24,40);
      lv_obj_remove_flag(slot->hit,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_EVENT_BUBBLE);
      glass_input_guard_install(slot->hit);
      lv_obj_add_event_cb(slot->hit,glass_pager_clicked,LV_EVENT_CLICKED,slot);
      slot->mark=lv_obj_create(slot->hit);lv_obj_remove_style_all(slot->mark);
      lv_obj_remove_flag(slot->mark,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_set_style_bg_opa(slot->mark,LV_OPA_COVER,0);lv_obj_set_style_radius(slot->mark,3,0);
    }
    slot->page=first+i;bool active=slot->page==current;
    int width=active?20:6,x=(24-width)/2;
    if(!desktop_motion_geometry||!desktop_motion_geometry(slot->mark,x,17,width,6,3,!created)){
      lv_obj_set_pos(slot->mark,x,17);lv_obj_set_size(slot->mark,width,6);
    }
    lv_color_t target=active?lv_color_hex(pager->ink):
      lv_color_mix(lv_color_hex(pager->muted),lv_obj_get_style_bg_color(pager->face,0),LV_OPA_40);
    if(!desktop_motion_color||!desktop_motion_color(slot->mark,lv_color_to_u32(target),!created))
      lv_obj_set_style_bg_color(slot->mark,target,0);
    lv_obj_set_style_bg_opa(slot->mark,LV_OPA_COVER,0);
    lv_obj_remove_flag(slot->hit,LV_OBJ_FLAG_HIDDEN);
  }
  glass_pager_set_enabled(root,enabled);
}
static inline void glass_pager_update(lv_obj_t *root,unsigned current,unsigned total,bool enabled)
{
  if(!root)return;struct glass_pager *pager=lv_obj_get_user_data(root);
  unsigned count=total<pager->limit?total:pager->limit;
  unsigned first=current>count/2?current-count/2:0;
  if(first>total-count)first=total-count;
  glass_pager_update_window(root,current,first,count,enabled);
}
