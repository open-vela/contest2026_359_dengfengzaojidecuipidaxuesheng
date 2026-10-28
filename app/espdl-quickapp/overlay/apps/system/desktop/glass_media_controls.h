/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "glass_input_guard.h"

/* Full-screen controls keep their own contrast over any photo/video frame. */
static inline lv_obj_t *glass_media_exit(lv_obj_t *overlay,const lv_font_t *font,
                                       lv_event_cb_t callback,void *data)
{
  lv_obj_t *button=lv_button_create(overlay);
  lv_obj_remove_style_all(button);
  lv_obj_set_size(button,132,44);
  /* The shell status bar lives on layer_top even when video uses the screen. */
  lv_obj_align(button,LV_ALIGN_TOP_RIGHT,-16,48);
  lv_obj_set_style_radius(button,22,0);
  lv_obj_set_style_bg_color(button,lv_color_hex(0x252832),0);
  lv_obj_set_style_bg_opa(button,LV_OPA_COVER,0);
  lv_obj_set_style_border_color(button,lv_color_white(),0);
  lv_obj_set_style_border_width(button,1,0);
  lv_obj_remove_flag(button,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_EVENT_BUBBLE|LV_OBJ_FLAG_GESTURE_BUBBLE);
  glass_input_guard_install(button);
  lv_obj_add_event_cb(button,callback,LV_EVENT_CLICKED,data);
  lv_obj_t *label=lv_label_create(button);
  lv_label_set_text(label,"退出全屏");
  if(font)lv_obj_set_style_text_font(label,font,0);
  lv_obj_set_style_text_color(label,lv_color_white(),0);
  lv_obj_remove_flag(label,LV_OBJ_FLAG_CLICKABLE);
  lv_obj_center(label);lv_obj_move_foreground(button);
  return button;
}
