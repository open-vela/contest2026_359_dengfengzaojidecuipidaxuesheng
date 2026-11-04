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
/* Call before capture; failure requires closing the whole ASR session. */
int glv_asr_make_resident(struct glv_asr *);
/* Streaming policy: resident joiner, <=1MiB encoder block, row-read decoder. */
int glv_asr_prepare_partial(struct glv_asr *);
void glv_asr_io_statistics(const struct glv_asr *,struct glv_asr_io_stats *);
size_t glv_asr_cached_bytes(const struct glv_asr *);
int glv_asr_transcribe(struct glv_asr *,const int16_t *,size_t,char *,size_t,
                        int (*)(void *,unsigned,unsigned),void *,struct glv_asr_stats *);
/* Live, single-owner incremental API. Only finish produces a final command.
 * Partial callback text is provisional and must never dispatch a tool. */
int glv_asr_stream_begin(struct glv_asr *,int (*)(void *,const char *,unsigned,unsigned),void *);
int glv_asr_stream_accept(struct glv_asr *,const int16_t *,size_t);
int glv_asr_stream_finish(struct glv_asr *,char *,size_t,struct glv_asr_stats *);
void glv_asr_stream_abort(struct glv_asr *);
#ifdef __cplusplus
}
#endif
#endif
