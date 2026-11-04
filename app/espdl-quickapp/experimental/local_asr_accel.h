/* SPDX-License-Identifier: Apache-2.0 */
#ifndef LOCAL_ASR_ACCEL_H
#define LOCAL_ASR_ACCEL_H
#include "local_asr_ops.h"
#include "local_asr_parallel.h"
/* Return 1 only before any I/O when optional admission is unavailable. */
int glv_asr_accel_matmul(struct glv_memory *,const float *,float *,unsigned,unsigned,unsigned,const struct glv_asr_rows *);
int glv_asr_accel_conv(struct glv_memory *,const float *,const float *,float *,const struct glv_asr_conv *,const struct glv_asr_rows *,unsigned,unsigned,unsigned);
int glv_asr_accel_probe(struct glv_memory *);
/* Exact published UINT8 x INT8 -> INT32; zero-points are scalar. Reader
 * runs only on the owner; compute lanes share immutable aligned S16 tiles. */
struct glv_asr_integer_rows {int (*read)(void *,unsigned,int8_t *,unsigned);void *opaque;};
int glv_asr_accel_integer(struct glv_memory *,const uint8_t *,int32_t *,unsigned,unsigned,unsigned,
                         unsigned,int,const struct glv_asr_integer_rows *);
#endif
