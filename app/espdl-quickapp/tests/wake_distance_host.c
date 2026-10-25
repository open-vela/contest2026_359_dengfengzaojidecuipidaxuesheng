/* SPDX-License-Identifier: Apache-2.0 */
/* Offline public-stream boundary test; matcher/model I/O are inert. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#define glass_wake_extract test_extract
#define glass_wake_ratio_distance test_match
#include "glass_custom_wake.c"
static float test_ratio,test_distance;
int test_extract(const int16_t *pcm,size_t count,float *out)
{assert(pcm&&count>=8000);memset(out,0,GLASS_WAKE_INPUTS*sizeof(*out));return 0;}
float test_match(const float *weights,const float *feature,float *distance)
{(void)weights;assert(feature&&distance);*distance=test_distance;return test_ratio;}
void qpk_dl_sha256(const void *p,size_t n,uint8_t out[32])
{(void)p;(void)n;memset(out,0,32);}
static void check(float ratio,float distance,unsigned expected)
{
  struct glass_custom_wake *s=calloc(1,sizeof(*s));assert(s);
  s->threshold=.91647577f;s->noise=50;s->warmup=4;s->initial_floor=1e20f;
  test_ratio=ratio;test_distance=distance;
  int16_t pcm[320]={0};unsigned events=0;
  for(unsigned block=0;block<120;block++){
    for(unsigned i=0;i<320;i++)pcm[i]=block>=20&&block<70?(i&1?1000:-1000):0;
    int event=glass_custom_wake_feed(s,pcm,320);assert(event>=0);events+=event;
  }
  assert(events==expected);free(s);
}
int main(void)
{
  check(.8f,GLASS_WAKE_MAX_POSITIVE_DISTANCE,1);
  check(.8f,GLASS_WAKE_MAX_POSITIVE_DISTANCE+.0001f,0);
  check(.8f,INFINITY,0);check(.8f,NAN,0);
  check(.95f,.1f,0);check(.90f,.1f,1);
  puts("WAKE_DISTANCE_BOUNDARY_OK: limit, above-limit, nonfinite, unchanged ratio rejection and delayed final acceptance; ASan/UBSan");
}
