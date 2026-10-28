/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <mqueue.h>
#include <nuttx/audio/audio.h>
#define MUSIC_PCM_BUFFERS 4
/* The I2S bus runs two 16-bit slots per frame, so one frame is four bytes. */
/* Speech retains 240-frame blocks for its microphone observer. Music and
 * video at 32/44.1/48 kHz use 960-frame blocks: four buffers retain about
 * 80 ms instead of 20 ms at 48 kHz, covering decoder/GUI scheduling gaps.
 * Both sizes are complete 64-byte cache lines; one DMA buffer stays <4 KiB.
 */
#define MUSIC_PCM_FRAMES 240
#define MUSIC_PCM_MEDIA_FRAMES 960
#define MUSIC_PCM_BLOCK_FRAMES(rate) ((rate) > 24000 ? MUSIC_PCM_MEDIA_FRAMES : MUSIC_PCM_FRAMES)
struct music_pcm {
  int fd;
  mqd_t mq;
  struct ap_buffer_s *buffers[MUSIC_PCM_BUFFERS];
  bool queued[MUSIC_PCM_BUFFERS], started, reserved, session_locked;
  unsigned rate, pending, played_samples;
  unsigned submitted_samples[MUSIC_PCM_BUFFERS];
  unsigned staged_frames, staged_index;
};
int music_pcm_open(struct music_pcm *pcm, unsigned rate, unsigned volume);
int music_pcm_try_open(struct music_pcm *pcm, unsigned rate, unsigned volume);
/* Worker-only bounded handoff from low-priority background wake. Other
 * foreground owners retain their session and return EBUSY. */
int music_pcm_foreground_open(struct music_pcm *pcm, unsigned rate,
                             unsigned volume, unsigned timeout_ms,
                             bool (*cancelled)(void *), void *opaque);
bool music_pcm_foreground_pending(void);
int music_pcm_write(struct music_pcm *pcm, const int16_t *samples, unsigned frames, unsigned channels);
int music_pcm_volume(struct music_pcm *pcm, unsigned volume);
int music_pcm_pause(struct music_pcm *pcm, bool paused);
int music_pcm_drain(struct music_pcm *pcm);
void music_pcm_close(struct music_pcm *pcm);
int music_pcm_session_lock(void);
int music_pcm_session_trylock(void);
void music_pcm_session_unlock(void);
bool music_pcm_session_active(void);
