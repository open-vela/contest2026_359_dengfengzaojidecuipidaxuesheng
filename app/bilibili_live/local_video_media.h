/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
struct local_video;
struct local_video_ops {
  int (*frame)(void *, const uint8_t *const[3], const int[2], int, int);
  int (*audio_open)(void *, unsigned, unsigned);
  int (*audio_write)(void *, const int16_t *, unsigned, unsigned);
  int (*audio_pause)(void *, bool);
  int (*audio_volume)(void *, unsigned);
  int (*audio_drain)(void *);
  unsigned (*audio_time)(void *);
  void (*audio_close)(void *);
  uint16_t *(*native_buffer)(void *,unsigned,unsigned);
  int (*native_frame)(void *,const uint16_t *,unsigned,unsigned);
};
struct local_video_snapshot {
  unsigned position_ms, duration_ms, frames, dropped, volume, video_ms, late_ms;
  bool running, paused, has_audio, mjpeg;
  int error;
  unsigned decoded;
  uint64_t decoder_us,render_us;
};
struct local_video *local_video_create(const char *, const struct local_video_ops *, void *);
int local_video_run(struct local_video *);
void local_video_cancel(struct local_video *);
bool local_video_cancelled(struct local_video *);
void local_video_pause(struct local_video *, bool);
void local_video_volume(struct local_video *, unsigned);
void local_video_snapshot(struct local_video *, struct local_video_snapshot *);
void local_video_destroy(struct local_video *);
int local_video_decode_benchmark(const char *path, unsigned mc_features);
