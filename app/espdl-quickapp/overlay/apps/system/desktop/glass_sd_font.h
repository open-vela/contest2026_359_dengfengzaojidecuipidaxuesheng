/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GLASS_SD_FONT_H
#define GLASS_SD_FONT_H
#include <lvgl/lvgl.h>
#include "glass_sd_font_asset.h"
/* Start checksum work off the UI thread. All other calls are UI-thread only. */
void glass_sd_font_start(void);
/* 1: new fonts ready; -1: withdraw fallbacks after IO failure; 0: unchanged. */
int glass_sd_font_poll(void);
lv_font_t *glass_sd_font_get(unsigned pixels);
void glass_sd_font_status(int *state,int *error);
/* The choice is stored on SD by a worker; never write internal Flash here.
 * load: false until startup checking finishes. save_status: 0 pending/no event,
 * 1 saved, negative errno on failure (consumed once). */
bool glass_font_choice_load(unsigned *index);
int glass_font_choice_save(unsigned index);
int glass_font_choice_save_status(void);
/* Optional global families: index 0 is the embedded default. UI thread only,
 * except for the private checksum worker. Fonts/caches have bounded lifetime
 * storage, so switching styles never invalidates a label's font pointer. */
unsigned glass_lyric_font_count(void);
const char *glass_lyric_font_name(unsigned index);
int glass_lyric_font_request(unsigned index);
/* 0 idle, 1 checking, 2 verified, 3 ready, 4 unavailable. */
int glass_lyric_font_state(unsigned index);
bool glass_lyric_font_poll(void);
struct glass_font_cache_status {
  unsigned index,glyph_capacity,preloaded_bytes,ready_sizes,glyph_cached;
  uint64_t reads,page_misses;
};
/* GUI-thread only: bounded active-family budget; live font pointers survive. */
void glass_lyric_font_set_global(unsigned index);
void glass_lyric_font_cache_status(unsigned index,struct glass_font_cache_status *status);
lv_font_t *glass_lyric_font_get(unsigned index,unsigned pixels);
#endif
