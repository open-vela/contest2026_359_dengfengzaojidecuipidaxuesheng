/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#define GLASS_WAKE_RATE 16000
#define GLASS_WAKE_STEPS 32
#define GLASS_WAKE_COEFS 13
#define GLASS_WAKE_INPUTS (GLASS_WAKE_STEPS*GLASS_WAKE_COEFS)
#define GLASS_WAKE_POSITIVE 54
#define GLASS_WAKE_NEGATIVE 144
#define GLASS_WAKE_TEMPLATES (GLASS_WAKE_POSITIVE+GLASS_WAKE_NEGATIVE)
#define GLASS_WAKE_MAX_SAMPLES 64000
#define GLASS_WAKE_WEIGHTS (GLASS_WAKE_TEMPLATES*GLASS_WAKE_INPUTS)
/* For at most 64000 int16 samples, every square and their sum are exact in
 * double. Accumulate the same integers without per-sample soft-double calls;
 * retain the original double division and float square-root rounding. */
static inline float glass_wake_pcm_rms(const int16_t *pcm,size_t samples)
{
  uint64_t sum=0;
  for(size_t i=0;i<samples;i++){
    int32_t value=pcm[i];sum+=(uint32_t)(value*value);
  }
  return samples?sqrtf((double)sum/samples):0;
}
/* Shared by the host trainer and board; returns zero or a negative errno. */
int glass_wake_extract(const int16_t *pcm,size_t samples,float out[GLASS_WAKE_INPUTS]);
float glass_wake_ratio(const float *templates,const float features[GLASS_WAKE_INPUTS]);
float glass_wake_dtw(const float *a,const float *b);

/* Same DTW search as glass_wake_ratio; optional absolute positive mean. */
float glass_wake_ratio_distance(const float *templates,const float features[GLASS_WAKE_INPUTS],float *positive_distance);
