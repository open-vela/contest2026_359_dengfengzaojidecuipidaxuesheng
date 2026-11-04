/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BILI_LIVE_H
#define BILI_LIVE_H
#include <lvgl/lvgl.h>
void bili_live_show(lv_obj_t *parent, const lv_font_t *font);
/* Call before hiding/cleaning/replacing the desktop panel. LVGL thread only. */
void bili_live_stop(void);
/* Desktop/LVGL thread only; same action as the Play button. */
void bili_live_play_room(const char *room);
void bili_live_play_platform(int platform,const char *room);

/* Read-only LVGL-thread query; never changes player state. */
bool bili_live_fullscreen_active(void);
#endif
