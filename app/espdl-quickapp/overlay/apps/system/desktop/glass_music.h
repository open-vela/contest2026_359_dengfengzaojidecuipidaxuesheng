/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MUSIC_FAVORITES_MAX 20
/* Configuration failures must not be reported as an empty search result. */
#define MUSIC_ERROR_KEY_REQUIRED (-1001)
struct music_song {
  char name[128], artist[96], album[128], query[128];
  char url[2048];
};
enum music_state { MUSIC_IDLE, MUSIC_LOADING, MUSIC_PLAYING, MUSIC_PAUSED,
                   MUSIC_STOPPED, MUSIC_ENDED, MUSIC_ERROR };
struct music_snapshot {
  struct music_song current, result;
  struct music_song favorites[MUSIC_FAVORITES_MAX];
  unsigned revision, search_revision, position_ms, duration_ms, volume, favorite_count;
  enum music_state state;
  bool searching, has_result, initialized;
  int error, search_error, storage_error;
};
#define MUSIC_LYRIC_TEXT_MAX 192
enum music_lyrics_state { MUSIC_LYRICS_NONE, MUSIC_LYRICS_LOADING,
                          MUSIC_LYRICS_READY, MUSIC_LYRICS_MISSING };
/* Small, UI-independent view: no URLs, library copies or retained pointers. */
struct music_now_playing {
  char name[128], artist[96], album[128];
  char cover_url[1024];
  unsigned track_id;
  char lyrics[3][MUSIC_LYRIC_TEXT_MAX];
  char translations[3][MUSIC_LYRIC_TEXT_MAX];
  unsigned revision, position_ms, duration_ms, volume;
  enum music_state state;
  enum music_lyrics_state lyrics_state;
  bool pause_requested, favorite, can_skip, lyrics_started;
};
void glass_music_now_playing(struct music_now_playing *out);
#define MUSIC_LYRICS_WINDOW 9
/* Bounded, owned copy for a virtual lyric viewport; never exposes parser RAM. */
struct music_lyrics_window {
  unsigned track_id, first, count, total;
  int current;
  enum music_lyrics_state state;
  unsigned start_ms[MUSIC_LYRICS_WINDOW], end_ms[MUSIC_LYRICS_WINDOW];
  char lines[MUSIC_LYRICS_WINDOW][MUSIC_LYRIC_TEXT_MAX];
  char translations[MUSIC_LYRICS_WINDOW][MUSIC_LYRIC_TEXT_MAX];
};
void glass_music_lyrics_read(unsigned first, struct music_lyrics_window *out);
int glass_music_skip(int direction);
void glass_music_start(void);
void glass_music_get(struct music_snapshot *out);
int glass_music_search(const char *query);
int glass_music_play(const struct music_song *song);
/* Play the latest search result once, within 60s, without a second lookup.
 * Query and revision are checked atomically; URLs never cross the tool API. */
int glass_music_play_result(const char *query, unsigned result_id, unsigned *track_id);
void glass_music_toggle(void);
/* Worker performs an accurate decoded-sample seek. Reject stale UI gestures. */
int glass_music_seek(unsigned track_id, unsigned position_ms);
/* Idempotent transport control: never starts or toggles a different track. */
int glass_music_set_paused(bool paused);
void glass_music_stop(void);
int glass_music_stop_wait(unsigned timeout_ms);
/* Set the shared system speaker volume (clamped to 0..100). */
void glass_music_volume(unsigned volume);
int glass_music_favorite(const struct music_song *song);
bool glass_music_is_favorite(const struct music_snapshot *state, const struct music_song *song);
int music_song_parse(const char *body, size_t size, const char *query, struct music_song *song);
int music_search_url(const char *query, const char *key, char *url, size_t cap);

typedef bool (*music_cancel_cb)(void *);
typedef int (*music_data_cb)(void *, const unsigned char *, size_t);
int music_http_get(const char *url, music_data_cb sink, void *ctx,
                   music_cancel_cb cancelled, unsigned *status);
