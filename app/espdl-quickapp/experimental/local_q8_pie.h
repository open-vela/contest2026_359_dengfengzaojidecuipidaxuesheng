/* SPDX-License-Identifier: Apache-2.0 */
#ifndef LOCAL_Q8_PIE_H
#define LOCAL_Q8_PIE_H
#include "glass_local_model.h"
/* INT8 row weights, per-32 activation A16, signed INT32 group dots, FP32 scale.
 * Uses the reviewed preemptible S16 PIE batch kernel, never W8A8. */
int glv_q8_matbatch(struct glv_model *,const char *,const float *,float *,unsigned,unsigned,unsigned);
int glv_q8_selftest(struct glv_memory *);
#endif
