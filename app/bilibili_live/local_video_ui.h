/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <lvgl/lvgl.h>
void local_video_show(lv_obj_t *, const lv_font_t *);
void local_video_stop(void);
void local_video_play_path(const char *);
/* GUI-thread diagnostic A/B switch; leaves decoder/PPA conversion unchanged. */
void local_video_set_upscale(bool enabled);
void local_video_set_volume(unsigned value);
void local_video_set_screen_layer(bool enabled);
void local_video_draw_status(void (*emit)(const char *,...));
/* Consume the full-screen overlay without leaving the owning player page. */
bool local_video_back(lv_obj_t *owner);

void local_video_view(lv_obj_t *parent,const lv_font_t *font,const char *path,lv_event_cb_t back);
