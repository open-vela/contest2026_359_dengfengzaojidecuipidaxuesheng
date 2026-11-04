/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
struct glass_lock_photo { uint16_t *pixels; unsigned width,height; int error; };
int glass_lock_photo_request(void);
void glass_lock_photo_cancel(void);
/* Transfers ownership of pixels to the caller; never invokes LVGL. */
bool glass_lock_photo_take(struct glass_lock_photo *out);

#define GLASS_COVER_MUSIC 1u
#define GLASS_COVER_LOCK 2u
#define GLASS_COVER_HOME 3u
#define GLASS_COVER_THUMBNAIL_FIRST 4u
#define GLASS_COVER_THUMBNAIL_COUNT 4u
#define GLASS_COVER_MAX (GLASS_COVER_THUMBNAIL_FIRST+GLASS_COVER_THUMBNAIL_COUNT-1u)
int glass_lock_cover_request(unsigned owner,const char *url);
void glass_lock_cover_cancel(unsigned owner);
bool glass_lock_cover_take(unsigned owner,struct glass_lock_photo *out);

struct music_song;
/* Background conversion shares the single bounded JPEG owner. It never
 * publishes a result into an on-screen view's generation/slot. */
int glass_music_artwork_prepare(const struct music_song *song,
                               bool (*cancelled)(void *),void *context);
struct glass_artwork_stats {unsigned cache_hits,jpeg_decodes,prepared;};
void glass_lock_artwork_stats(struct glass_artwork_stats *out);
