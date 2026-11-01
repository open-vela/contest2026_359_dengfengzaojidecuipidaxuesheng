/* SPDX-License-Identifier: Apache-2.0 */
#ifndef LOCAL_TTS_INT8_POLICY_H
#define LOCAL_TTS_INT8_POLICY_H
#include "glass_local_model.h"
#ifndef GLV_TTS_INT8_POLICY_ROOT
#define GLV_TTS_INT8_POLICY_ROOT "/sdcard/local-voice/models/tts-int8-mix-v1-20261101"
#endif
int glv_tts_int8_policy_load(struct glv_model *);
void glv_tts_int8_policy_close(struct glv_memory *);
int glv_tts_int8_precision(const struct glv_model *,const struct glv_tensor *);
static inline bool glv_tts_int8_affine(const struct glv_memory *m)
{return m->tts_int8_policy!=NULL;}
#endif
