/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <lvgl/lvgl.h>
static lv_point_t g_ui_probe_pointer_point;
static lv_indev_state_t g_ui_probe_pointer_state;
static void desktop_ui_probe_pointer_read(lv_indev_t *input,lv_indev_data_t *data)
{LV_UNUSED(input);data->point=g_ui_probe_pointer_point;data->state=g_ui_probe_pointer_state;}
/* GUI thread only: feed pressed movement samples so LVGL can actually scroll
 * and suppress a row click. A press followed only by release is not a drag. */
static bool desktop_ui_probe_pointer_stroke(int x,int y,int end_x,int end_y)
{
  lv_display_t *display=lv_display_get_default();
  if(!display||x<0||y<0||x>=lv_display_get_horizontal_resolution(display)||
     y>=lv_display_get_vertical_resolution(display)||end_x<0||end_y<0||
     end_x>=lv_display_get_horizontal_resolution(display)||end_y>=lv_display_get_vertical_resolution(display))return false;
  lv_indev_t *input=lv_indev_create();if(!input)return false;
  lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);lv_indev_set_display(input,display);
  lv_indev_set_mode(input,LV_INDEV_MODE_EVENT);lv_indev_set_read_cb(input,desktop_ui_probe_pointer_read);
  g_ui_probe_pointer_point=(lv_point_t){x,y};g_ui_probe_pointer_state=LV_INDEV_STATE_PRESSED;lv_indev_read(input);
  if(x!=end_x||y!=end_y)for(unsigned i=1;i<=8;i++){
    g_ui_probe_pointer_point.x=x+(end_x-x)*(int)i/8;
    g_ui_probe_pointer_point.y=y+(end_y-y)*(int)i/8;lv_indev_read(input);
  }
  g_ui_probe_pointer_point=(lv_point_t){end_x,end_y};g_ui_probe_pointer_state=LV_INDEV_STATE_RELEASED;lv_indev_read(input);
  /* This LVGL version does not cancel input-owned throw animations from
   * lv_indev_delete. Retire them while their device is still alive. */
  lv_anim_delete(input,NULL);lv_indev_delete(input);return true;
}
static bool desktop_ui_probe_pointer_tap(int x,int y)
{return desktop_ui_probe_pointer_stroke(x,y,x,y);}
