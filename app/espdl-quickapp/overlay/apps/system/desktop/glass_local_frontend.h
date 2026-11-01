/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GLASS_LOCAL_FRONTEND_H
#define GLASS_LOCAL_FRONTEND_H
#include "glass_local_tokenizer.h"
#define GLV_PHONE_SEGMENTS 128
#define GLV_PHONE_LEGACY_SEGMENTS 32
struct glv_phone_part {unsigned count;int ids[256];unsigned char kind[256];};
struct glv_phone_plan {unsigned count;struct glv_phone_part parts[];};
/* root is the verified release directory. All temporary dictionaries are
 * released before returning; only the small phone plan survives into TTS. */
int glv_frontend_plan(const char *,const char *,struct glv_memory *,struct glv_phone_plan **);
#endif
