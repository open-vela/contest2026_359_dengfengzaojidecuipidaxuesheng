/* SPDX-License-Identifier: Apache-2.0 */
#ifndef LOCAL_ASR_PARALLEL_H
#define LOCAL_ASR_PARALLEL_H
#include "glass_local_model.h"
struct glv_asr_lane {struct glv_memory *memory;void *pool;unsigned index;int *pie_state;};
typedef int (*glv_asr_parallel_body)(void *,unsigned,unsigned,struct glv_asr_lane *);
int glv_asr_lane_cancel(struct glv_asr_lane *);
int glv_asr_parallel_run(struct glv_memory *,glv_asr_parallel_body,void *,unsigned,uint64_t);
void glv_asr_parallel_close(struct glv_memory *);
#endif
