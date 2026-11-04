/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "local_asr_ops.h"
/* Inputs have already passed broadcast/capacity/alias validation. Chunk
 * length<=1024 and increments0/1 keep cancellation and integer indexing bounded.
 * Returns1 before touching output if this optional backend is unavailable. */
int glv_asr_dsp_binary(struct glv_memory *,enum glv_asr_binary,const float *,const float *,float *,unsigned,unsigned,unsigned);
