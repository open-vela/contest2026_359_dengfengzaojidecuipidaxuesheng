/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "glass_local_model.h"
#ifndef GLV_ASR_RELEASE_ROOT
#define GLV_ASR_RELEASE_ROOT "/sdcard/local-voice/models/zipformer14m-q48-ram-20261104"
#endif
/* Worker-only staged verification entry. No microphone/network/Flash access.
 * Only the two manifest-bound, public model test clips are selectable. */
int glv_asr_test_file(unsigned,struct glv_memory *,char *,size_t,
                      int (*)(void *,unsigned,unsigned),void *,void (*)(void *,unsigned));
int glv_asr_assets_check(struct glv_memory *);
int glv_asr_transcribe_pcm(const int16_t *,size_t,struct glv_memory *,char *,size_t,
                            int (*)(void *,unsigned,unsigned),void *,void (*)(void *,unsigned));
