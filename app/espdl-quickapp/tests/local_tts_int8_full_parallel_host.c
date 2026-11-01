/* SPDX-License-Identifier: Apache-2.0 */
#include "local_tts_int8.c"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>
static unsigned allocations,fail_alloc,creates,joins;
static bool fail_create,fail_join;
static atomic_uint polls;static unsigned stop_at,tests;static bool affine_test;static unsigned char marker=1;
void *__real_calloc(size_t,size_t);
void *__wrap_calloc(size_t n,size_t size)
{if(fail_alloc&&++allocations==fail_alloc){errno=ENOMEM;return NULL;}return __real_calloc(n,size);}
int __real_pthread_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
int __wrap_pthread_create(pthread_t *t,const pthread_attr_t *a,void *(*fn)(void *),void *p)
{creates++;return fail_create?EAGAIN:__real_pthread_create(t,a,fn,p);}
int __real_pthread_join(pthread_t,void **);
int __wrap_pthread_join(pthread_t t,void **p)
{joins++;return fail_join?EINVAL:__real_pthread_join(t,p);}
static bool cancelled(void *unused)
{(void)unused;unsigned n=atomic_fetch_add(&polls,1)+1;return stop_at&&n>=stop_at;}
static void reset(struct glv_memory *m,bool parallel)
{*m=(struct glv_memory){.limit=GLV_MEMORY_LIMIT,.cancelled=cancelled,.tts_parallel_pie=parallel,.tts_int8_reduce=true};m->tts_int8_policy=affine_test?&marker:NULL;atomic_store(&polls,0);}
static void one(unsigned c,unsigned o,unsigned k,unsigned time,unsigned dilation,bool leaky,bool add,int rounding)
{
  assert(!fesetround(rounding));
  size_t points=(size_t)c*time,size=(size_t)o*time+8;unsigned row=c*k+4;
  float *in=malloc(points*4),*a=malloc(size*4),*b=malloc(size*4),*bias=malloc(o*4);
  unsigned char *data=malloc((size_t)o*row);assert(in&&a&&b&&bias&&data);
  unsigned seed=31;
  for(size_t i=0;i<points;i++){seed=seed*1664525u+1013904223u;in[i]=(float)((int)(seed%65535)-32767)/32768.f;}
  for(unsigned r=0;r<o;r++){
    float scale=.0001f;memcpy(data+r*row,&scale,4);bias[r]=((int)(r%5)-2)*.01f;
    for(unsigned i=0;i<c*k;i++){seed=seed*1664525u+1013904223u;data[r*row+4+i]=(unsigned char)(int8_t)((int)(seed%255)-127);}
  }
  struct glv_memory m;reset(&m,false);
  struct glv_model model={.memory=&m,.data=data,.bytes=(size_t)o*row};
  struct glv_tensor w={.shape={o,c,k},.ndim=3,.type=2,.stride=row};unsigned padding=dilation*(k-1)/2;
  for(size_t i=0;i<size;i++)a[i]=b[i]=.125f;
  assert(!glv_tts_int8_conv(&model,&w,in,c,time,a+4,o,time,dilation,padding,bias,leaky,add)&&!m.used);
  size_t mandatory=m.peak;uint64_t dots=m.profile.tts_pie_dot_calls;unsigned batches=m.profile.tts_pie_batches;
  reset(&m,true);
  assert(!glv_tts_int8_conv(&model,&w,in,c,time,b+4,o,time,dilation,padding,bias,leaky,add)&&!m.used);
  assert(!memcmp(a,b,size*4)&&m.profile.tts_pie_dot_calls==dots&&m.profile.tts_pie_batches==batches);
  bool eligible=(uint64_t)o*time*((c*k+15)&~15u)>=262144&&time>8&&rounding==FE_TONEAREST;
  assert(m.profile.tts_pie_parallel_jobs==(eligible?1u:0u));
  assert(m.profile.tts_pie_context_checks==(eligible?2u:0u));
  unsigned complete_polls=atomic_load(&polls);
  if(eligible){
    for(unsigned mode=0;mode<6;mode++){
      reset(&m,true);for(size_t i=0;i<size;i++)b[i]=.125f;
      allocations=creates=joins=0;
      if(mode<2)fail_alloc=7+mode;
      if(mode==2){unsigned stride=(c*k+15)&~15u,blocks=1;
        m.limit=mandatory+glv_alloc_charge(PATCHES*stride+15)+glv_alloc_charge(o*PATCHES*blocks*4)+65536;}
      if(mode==3)fail_create=true;
      if(mode==4)m.tts_single_dot=true;
      if(mode==5)m.tts_serial_fp=true;
      assert(!glv_tts_int8_conv(&model,&w,in,c,time,b+4,o,time,dilation,padding,bias,leaky,add)&&!m.used);
      assert(!memcmp(a,b,size*4)&&!m.profile.tts_pie_parallel_jobs);
      if(mode<2)assert(m.profile.tts_pie_parallel_scratch_fallbacks==1);
      if(mode==2)assert(m.profile.tts_parallel_reasons[GLV_PAR_BUDGET]==1);
      if(mode==3)assert(m.profile.tts_parallel_reasons[GLV_PAR_CREATE]==1);
      fail_alloc=0;fail_create=false;
    }
    reset(&m,true);fail_join=true;
    assert(glv_tts_int8_conv(&model,&w,in,c,time,b+4,o,time,dilation,padding,bias,leaky,add)==-EINVAL&&!m.used);fail_join=false;
    for(unsigned i=0;i<4;i++){
      reset(&m,true);stop_at=(unsigned[]){1,3,complete_polls/2,complete_polls-1}[i];
      assert(glv_tts_int8_conv(&model,&w,in,c,time,b+4,o,time,dilation,padding,bias,leaky,add)==-ECANCELED&&!m.used);stop_at=0;
    }
    reset(&m,true);bias[o-1]=INFINITY;
    assert(glv_tts_int8_conv(&model,&w,in,c,time,b+4,o,time,dilation,padding,bias,leaky,add)==-ERANGE&&!m.used);
  }
  assert(!fesetround(FE_TONEAREST));free(in);free(a);free(b);free(bias);free(data);tests++;
}
int main(void)
{
  for(unsigned pass=0;pass<2;pass++){affine_test=pass!=0;
  const unsigned dims[][5]={{13,13,3,504,1},{13,13,3,505,1},{26,26,11,129,1},{52,52,11,129,16},{208,208,11,17,1},{3,5,7,1025,3}};
  for(unsigned d=0;d<6;d++)for(unsigned leaky=0;leaky<2;leaky++)for(unsigned add=0;add<2;add++)
    one(dims[d][0],dims[d][1],dims[d][2],dims[d][3],dims[d][4],leaky,add,FE_TONEAREST);
  for(unsigned mode=0;mode<3;mode++)one(13,13,3,505,1,true,true,(int[]){FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}[mode]);
  }
  usleep(5000);printf("FULL_ROW_INT8_PARALLEL_CANARY_THRESHOLD_ALLOC_CREATE_JOIN_CANCEL_ROUNDING_OK cases=%u\n",tests);return 0;
}
