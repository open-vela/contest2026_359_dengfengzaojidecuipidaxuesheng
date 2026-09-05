/****************************************************************************
 * apps/system/desktop/pomodoro_lvgl.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#pragma once

#include <lvgl/lvgl.h>

typedef void (*pomodoro_lvgl_close_cb_t)(void);

int pomodoro_lvgl_create(lv_obj_t *parent, const lv_font_t *font_zh,
                         pomodoro_lvgl_close_cb_t close_cb);
void pomodoro_lvgl_sync(void);
void pomodoro_lvgl_close(void);
bool pomodoro_lvgl_is_open(void);
