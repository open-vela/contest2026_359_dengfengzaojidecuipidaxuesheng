/* SPDX-License-Identifier: Apache-2.0 */
#include "local_tts_pie.c"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>
static unsigned fail_create,fail_join,creates,joins;
static atomic_uint cancel_calls;static unsigned cancel_after;
int __real_pthread_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
int __wrap_pthread_create(pthread_t *t,const pthread_attr_t *a,void *(*f)(void *),void *p)
{creates++;if(fail_create&&creates==fail_create)return EAGAIN;return __real_pthread_create(t,a,f,p);}
int __real_pthread_join(pthread_t,void **);
int __wrap_pthread_join(pthread_t t,void **p)
{joins++;if(fail_join&&joins==fail_join)return EINVAL;return __real_pthread_join(t,p);}
static bool cancelled(void *unused)
{(void)unused;return cancel_after&&atomic_fetch_add(&cancel_calls,1)+1>=cancel_after;}
static unsigned tests;
static void one(unsigned channels,unsigned time,bool leaky,int rounding,unsigned pattern)
{
 size_t points=(size_t)channels*time;float *x=malloc(points*4),*a=malloc((size_t)2*time*4),*b=malloc((size_t)2*time*4);assert(x&&a&&b);
 unsigned seed=17;
 for(size_t i=0;i<points;i++){
  seed=seed*1664525u+1013904223u;
  x[i]=pattern==0?(float)((int)(seed%16385)-8192)/8192.f:pattern==1?((i&1)?-0.f:0.f):(i%97==0?1.f:((int)(i%17)-8)*0x1p-145f);
 }
 unsigned stride=channels+4;unsigned char *data=calloc(2,stride);assert(data);
 for(unsigned o=0;o<2;o++){float scale=.125f;memcpy(data+o*stride,&scale,4);for(unsigned i=0;i<channels;i++)data[o*stride+4+i]=(unsigned char)(int8_t)(i&1?-127:127);}
 float bias[2]={.25f,-.5f};struct glv_tensor tensor={.name="input.weight",.shape={2,channels,1},.ndim=3,.type=2,.stride=stride};
 struct glv_memory memory={.limit=GLV_MEMORY_LIMIT,.tts_quant_fast=true,.cancelled=cancelled};struct glv_model model={.memory=&memory,.data=data,.bytes=2*stride};
 assert(!fesetround(rounding));int old=glv_tts_pie_conv(&model,&tensor,x,channels,time,a,2,time,1,0,bias,leaky,false);assert(!old&&!memory.used);
 memory=(struct glv_memory){.limit=GLV_MEMORY_LIMIT,.tts_quant_fast=true,.cancelled=cancelled,.tts_parallel_input=true};
 int now=glv_tts_pie_conv(&model,&tensor,x,channels,time,b,2,time,1,0,bias,leaky,false);assert(!now&&!memory.used&&!memcmp(a,b,(size_t)2*time*4));
 bool eligible=points>=65536&&rounding==FE_TONEAREST;
 assert(memory.profile.tts_input_max_jobs==(eligible?1u:0u)&&memory.profile.tts_input_quant_jobs==(eligible?1u:0u));
 assert(memory.profile.tts_input_rounding_fallbacks==((points>=65536&&rounding!=FE_TONEAREST)?1u:0u));
 if(eligible){
  size_t mandatory=memory.peak-131072u;memory=(struct glv_memory){.limit=mandatory+65536u,.tts_quant_fast=true,.cancelled=cancelled,.tts_parallel_input=true};
  assert(!glv_tts_pie_conv(&model,&tensor,x,channels,time,b,2,time,1,0,bias,leaky,false)&&!memory.used&&!memcmp(a,b,(size_t)2*time*4));
  assert(!memory.profile.tts_input_max_jobs&&!memory.profile.tts_input_quant_jobs&&memory.profile.tts_parallel_reasons[GLV_PAR_BUDGET]==2);
  for(unsigned failure=1;failure<=2;failure++){
   memory=(struct glv_memory){.limit=GLV_MEMORY_LIMIT,.tts_quant_fast=true,.cancelled=cancelled,.tts_parallel_input=true};creates=0;fail_create=failure;
   assert(!glv_tts_pie_conv(&model,&tensor,x,channels,time,b,2,time,1,0,bias,leaky,false)&&!memory.used&&!memcmp(a,b,(size_t)2*time*4));fail_create=0;
   assert(memory.profile.tts_parallel_reasons[GLV_PAR_CREATE]==1);
  }
  for(unsigned stop=1;stop<=3;stop+=2){
   memory=(struct glv_memory){.limit=GLV_MEMORY_LIMIT,.tts_quant_fast=true,.cancelled=cancelled,.tts_parallel_input=true};cancel_after=stop;atomic_store(&cancel_calls,0);
   assert(glv_tts_pie_conv(&model,&tensor,x,channels,time,b,2,time,1,0,bias,leaky,false)==-ECANCELED&&!memory.used);cancel_after=0;
  }
  memory=(struct glv_memory){.limit=GLV_MEMORY_LIMIT,.tts_quant_fast=true,.cancelled=cancelled,.tts_parallel_input=true};joins=0;fail_join=1;
  assert(glv_tts_pie_conv(&model,&tensor,x,channels,time,b,2,time,1,0,bias,leaky,false)==-EINVAL&&!memory.used);fail_join=0;
  for(unsigned bad=0;bad<2;bad++){
   memory=(struct glv_memory){.limit=GLV_MEMORY_LIMIT,.tts_quant_fast=true,.cancelled=cancelled,.tts_parallel_input=true};float saved=x[points-1];x[points-1]=bad?INFINITY:NAN;
   assert(glv_tts_pie_conv(&model,&tensor,x,channels,time,b,2,time,1,0,bias,leaky,false)==-EDOM&&!memory.used);x[points-1]=saved;
  }
 }
 assert(!fesetround(FE_TONEAREST));free(x);free(a);free(b);free(data);tests++;
}
int main(void)
{
 const unsigned dims[][2]={{2,32767},{2,32768},{2,33001},{13,5043}};
 for(unsigned d=0;d<4;d++)for(unsigned leaky=0;leaky<2;leaky++)for(unsigned pattern=0;pattern<3;pattern++)one(dims[d][0],dims[d][1],leaky,FE_TONEAREST,pattern);
 for(unsigned leaky=0;leaky<2;leaky++)for(unsigned mode=0;mode<3;mode++)one(2,33001,leaky,(int[]){FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}[mode],0);
 usleep(5000);printf("TTS_INPUT_PARALLEL_EXACT_FAULT_CANCEL_ROUNDING_OK cases=%u\n",tests);return 0;
}
