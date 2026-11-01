/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GLASS_LOCAL_TTS_H
#define GLASS_LOCAL_TTS_H
#include "glass_local_model.h"
struct glv_tts;
typedef int (*glv_audio_sink)(void *,const float *,unsigned,unsigned);
int glv_tts_open(const char *,struct glv_memory *,struct glv_tts **);
void glv_tts_close(struct glv_tts *);
/* phone_kind: 1 = tone-bearing vowel, 2 = punctuation pause, 0 = other.
 * leading inserts the saved V2 125ms pad once, never at internal tiles. */
int glv_tts_phones(struct glv_tts *,const int *,const unsigned char *,unsigned,bool,glv_audio_sink,void *);
/* Numeric-gate interfaces use the same production acoustic/vocoder kernels. */
int glv_tts_mel(struct glv_tts *,const int *,const unsigned char *,unsigned,float **,unsigned *);
int glv_tts_vocode(struct glv_tts *,const float *,unsigned,glv_audio_sink,void *);
#endif
