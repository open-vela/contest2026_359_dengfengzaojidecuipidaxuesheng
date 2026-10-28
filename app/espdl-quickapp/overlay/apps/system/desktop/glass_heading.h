/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <lvgl/lvgl.h>
/* A true weight-700 CJK/Latin face; normal text and icon fallback stay live. */
const lv_font_t *glass_heading_font(int pixels,const lv_font_t *fallback);
