/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GLV_EXPERIMENTAL_ASR_FBANK_H
#define GLV_EXPERIMENTAL_ASR_FBANK_H
#include "glass_local_model.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Fixed 16 kHz, 25 ms Povey / 10 ms shift / 512 FFT / 80 log-mel bins,
 * 20..7600 Hz, DC removal, preemphasis .97, dither off, snip_edges false.
 * PCM remains caller-owned/counted. Callback must consume/copy the 80 floats
 * before return. Negative callback results abort without replay. */
struct glv_asr_fbank;
int glv_asr_fbank_create(struct glv_asr_fbank **,struct glv_memory *);
void glv_asr_fbank_destroy(struct glv_asr_fbank *);
int glv_asr_fbank_pcm(struct glv_asr_fbank *,const int16_t *,size_t,size_t,
                       int (*)(void *,const float *,unsigned),void *);
int glv_asr_fbank_stream_begin(struct glv_asr_fbank *);
int glv_asr_fbank_stream_accept(struct glv_asr_fbank *,const int16_t *,size_t,int (*)(void *,const float *,unsigned),void *);
int glv_asr_fbank_stream_finish(struct glv_asr_fbank *,size_t,int (*)(void *,const float *,unsigned),void *);
#ifdef __cplusplus
}
#endif
#endif
