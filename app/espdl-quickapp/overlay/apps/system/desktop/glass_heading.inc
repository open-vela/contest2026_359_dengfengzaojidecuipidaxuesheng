/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_heading.h"
#include "glass_heading_data.inc"
const lv_font_t *glass_heading_font(int pixels,const lv_font_t *fallback)
{
  static const unsigned sizes[]={16,20,24,28,32,34,40};
  static lv_font_t *faces[7];
  unsigned slot=0;while(slot<6&&sizes[slot]<(unsigned)pixels)slot++;
  if(!faces[slot])faces[slot]=lv_tiny_ttf_create_data_ex(
    glass_heading_font_data,glass_heading_font_size,sizes[slot],LV_FONT_KERNING_NORMAL,64);
  if(!faces[slot])return fallback;
  faces[slot]->fallback=fallback;return faces[slot];
}
