/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GLV_EXPERIMENTAL_ASR_H
#define GLV_EXPERIMENTAL_ASR_H
#include "local_asr_graph.h"
#include "local_asr_fbank.h"
#ifdef __cplusplus
extern "C" {
#endif
struct glv_asr;
struct glv_asr_stats {unsigned frames,chunks,tokens;};
/* Synchronous worker-only API, single owner. All model/plan/token data are
 * regular SD assets. Audio must be captured at 16 kHz before inference; this
 * function does not reserve a microphone, write files or access the network.
 * Caller counts the PCM/output buffer in the same memory budget. */
int glv_asr_open(struct glv_asr **,const char *,struct glv_memory *);
void glv_asr_close(struct glv_asr *);
void glv_asr_io_statistics(const struct glv_asr *,struct glv_asr_io_stats *);
size_t glv_asr_cached_bytes(const struct glv_asr *);
int glv_asr_transcribe(struct glv_asr *,const int16_t *,size_t,char *,size_t,
                        int (*)(void *,unsigned,unsigned),void *,struct glv_asr_stats *);
#ifdef __cplusplus
}
#endif
#endif
