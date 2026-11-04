/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "glass_local_model.h"
int glv_w8a8_matbatch(struct glv_model *,const char *,const float *,float *,unsigned,unsigned,unsigned);

int glv_w8a8_prepare(struct glv_model *);
/* Owner-only: reuse=true requires caller to keep x byte-identical since the
 * immediately preceding projection. Ordinary public matbatch never reuses. */
int glv_w8a8_matbatch_cached(struct glv_model *,const char *,const float *,float *,unsigned,unsigned,unsigned,bool);
