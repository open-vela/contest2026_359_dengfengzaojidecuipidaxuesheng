/* SPDX-License-Identifier: Apache-2.0 */
#ifndef LOCAL_Q4_PIE_H
#define LOCAL_Q4_PIE_H
#include "glass_local_model.h"
/* Experimental integer activation path. SD block layout remains GLM1 Q4.
 * A16 is the target path; A8 exists for host quality comparison only. */
int glv_q4_matbatch(struct glv_model *,const char *,const float *,float *,
                    unsigned,unsigned,unsigned,unsigned);
int glv_q4_quantize32(const float *,int16_t *,float *,unsigned);
int32_t glv_q4_dot32_ref(const int16_t *,const int16_t *);
int glv_q4_selftest(struct glv_memory *);
#ifdef __NuttX__
int32_t glv_q4_dot32_pie(const int16_t *,const int16_t *);
/* Groups-major activations/results; each group is one aligned 32-item dot. */
void glv_q4_dot_batch_pie(const int16_t *,const int16_t *,int32_t *,unsigned,unsigned);
#endif
#endif
