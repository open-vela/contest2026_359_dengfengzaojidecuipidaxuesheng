/* SPDX-License-Identifier: Apache-2.0 */
#ifndef LOCAL_TTS_INT8_H
#define LOCAL_TTS_INT8_H
#include "glass_local_model.h"
/* Experimental W8A8 convolution. Integer activations change numeric results;
 * never enable before waveform quality and native timing/cleanup acceptance.
 * Input/output are channel-major; weights keep their existing SD format. */
int glv_tts_int8_conv(struct glv_model *,const struct glv_tensor *,const float *,
                     unsigned,unsigned,float *,unsigned,unsigned,unsigned,unsigned,
                     const float *,bool,bool);
#endif
