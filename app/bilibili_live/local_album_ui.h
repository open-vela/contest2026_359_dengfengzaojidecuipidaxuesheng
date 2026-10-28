/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <lvgl/lvgl.h>
void local_album_show(lv_obj_t *parent,const lv_font_t *font);
/* Camera entry starts at captured local photos; SD remains reachable. */
void local_album_show_camera(lv_obj_t *parent,const lv_font_t *font);
void local_album_show_app(lv_obj_t *parent,const lv_font_t *font,lv_event_cb_t exit,const char *caption,bool camera);
void local_album_open_path(const char *path);
/* Consume one view owned by this page; false lets the shell leave the album. */
bool local_album_back(lv_obj_t *page);

void local_album_view(lv_obj_t *parent,const lv_font_t *font,const char *path,lv_event_cb_t back);
