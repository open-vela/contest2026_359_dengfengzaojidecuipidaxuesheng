/* SPDX-License-Identifier: Apache-2.0 */
#ifndef LOCAL_TTS_PARALLEL_H
#define LOCAL_TTS_PARALLEL_H
#include "glass_local_model.h"
struct glv_tts_lane {struct glv_memory *memory;void *sync;};
typedef int (*glv_tts_parallel_body)(void *,unsigned,unsigned,struct glv_tts_lane *);
enum glv_tts_parallel_reason {
  GLV_PAR_CACHE,GLV_PAR_BUDGET,GLV_PAR_RESERVE,GLV_PAR_FRAGMENT,
  GLV_PAR_MUTEX,GLV_PAR_SETUP,GLV_PAR_CREATE,GLV_PAR_REASON_COUNT
};
void glv_tts_parallel_fallback(struct glv_memory *,enum glv_tts_parallel_reason,int);
int glv_tts_parallel_cancel(struct glv_tts_lane *);
/* Read-only inputs and disjoint output ranges. No allocation/I/O in body.
 * Both ranges retire before this function returns, including join errors. */
int glv_tts_parallel_run(struct glv_memory *,glv_tts_parallel_body,void *,unsigned,uint64_t);
#endif
