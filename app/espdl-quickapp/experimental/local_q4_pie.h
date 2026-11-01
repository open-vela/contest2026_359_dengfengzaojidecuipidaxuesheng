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
#endif
#endif
