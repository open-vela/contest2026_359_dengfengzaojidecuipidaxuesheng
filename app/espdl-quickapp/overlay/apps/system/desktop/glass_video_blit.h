/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* GUI-thread-only bridge. LVGL still owns rendering, buffer swaps and flush. */
void glass_video_blit_bind(const void *source);
void glass_video_blit_release(void);
void glass_video_blit_set_enabled(bool enabled);
bool glass_video_blit_cover_source(const void *source);
uint64_t glass_video_profile_now(void);
void glass_video_profile_add(unsigned kind, uint64_t started);
void glass_video_profile_reset(void);
void glass_video_profile_report(void (*emit)(const char *, ...));
int glass_video_blit_image(void *unit, const void *image, const void *decoded,
                           const void *coords, const void *clip);
int glass_video_blit_copy(void *dest, const void *dest_area,
                          const void *src, const void *src_area);
int glass_video_blit_fill(void *unit, const void *fill, const void *coords);
