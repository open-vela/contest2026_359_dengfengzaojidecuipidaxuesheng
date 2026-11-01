/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "glass_local_model.h"
#define GLV_CAPTURE_RATE 16000u
#define GLV_CAPTURE_BLOCK 240u
struct glv_capture_source {
  int (*open)(void *);
  int (*read)(void *,int16_t *);
  int (*close)(void *);
  uint64_t (*milliseconds)(void *);
  void *opaque;
};
struct glv_capture_signals {
  bool (*finish)(void *);
  void (*level)(void *,unsigned,unsigned);
  void *opaque;
};
struct glv_capture_options {
  unsigned max_ms,no_speech_ms,end_silence_ms;
  bool automatic_endpoint;
};
/* Collect native mono s16 in RAM. Never writes a file. A reached duration or
 * capacity limit fails the whole utterance, never returns shortened speech.
 * Caller frees the returned glv_alloc PCM after recognition. A source close
 * must synchronously retire DMA callbacks before returning. */
int glv_capture_collect(struct glv_memory *,const struct glv_capture_source *,
                          const struct glv_capture_signals *,const struct glv_capture_options *,int16_t **,size_t *);
/* NuttX device adapter; single worker, existing ES8311 session and capture
 * teardown. Not enabled in the current ASR evaluation firmware. */
int glv_capture_platform(struct glv_memory *,const struct glv_capture_signals *,
                           const struct glv_capture_options *,int16_t **,size_t *);
