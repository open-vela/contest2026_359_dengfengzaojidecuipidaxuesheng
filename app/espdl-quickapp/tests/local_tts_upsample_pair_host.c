/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_local_tts.c"
#include <assert.h>
static unsigned fail_at,allocations,cancel_at,checks;static bool big_mode;static unsigned output_count=2;
void *__real_calloc(size_t,size_t);
void *__wrap_calloc(size_t n,size_t size)
{if(fail_at&&++allocations==fail_at){errno=ENOMEM;return NULL;}return __real_calloc(n,size);}
static bool cancelled(void *unused){(void)unused;return cancel_at&&++checks>=cancel_at;}
static void one(unsigned stage,unsigned time,bool fallback,bool cancel)
{
  const unsigned strides[]={5,5,4,3};unsigned step=strides[stage],ic=big_mode?48:3,oc=big_mode?output_count+2:output_count,kernel=2*step,pad=(step+1)/2;
  size_t count=ic*oc*kernel;float *data=calloc(count+oc,4),*expected=calloc((size_t)oc*time*step,4);assert(data&&expected);
  for(size_t i=0;i<count+oc;i++)data[i]=(float)((int)(i*19%71)-35)*.017f;
  struct glv_memory memory={.limit=GLV_MEMORY_LIMIT,.cancelled=cancelled,.tts_upsample_pair=true};struct glv_tts s={.memory=&memory};
  struct glv_tensor tensors[2]={{.shape={oc},.ndim=1,.offset=count*4},{.shape={ic,oc,kernel},.ndim=3}};
  snprintf(tensors[0].name,80,"vocoder.upsamples.%u.1.bias",stage);snprintf(tensors[1].name,80,"vocoder.upsamples.%u.1.weight",stage);
  s.model=(struct glv_model){.memory=&memory,.tensors=tensors,.count=2,.data=(unsigned char *)data,.bytes=(count+oc)*4};
  struct matrix x=make(&s,ic,time);assert(x.v);
  for(unsigned i=0;i<ic*time;i++)x.v[i]=(float)((int)(i*11%47)-23)*.023f;
  fail_at=fallback?4:0;allocations=0;cancel_at=cancel?2:0;checks=0;
  struct matrix y=upsample(&s,x,stage);fail_at=cancel_at=0;
  if(cancel)assert(s.error==-ECANCELED&&!y.v);
  else{
    assert(!s.error&&y.v&&y.t==time*step);if(fallback)assert(allocations==4);
    for(unsigned o=0;o<oc;o++)for(unsigned t=0;t<y.t;t++)expected[(size_t)o*y.t+t]=data[count+o];
    for(unsigned i=0;i<ic;i++)for(unsigned o=0;o<oc;o++)for(unsigned k=0;k<kernel;k++)for(unsigned t=0;t<time;t++){
      int pos=(int)(t*step+k)-(int)pad;
      if(pos>=0&&(unsigned)pos<y.t)expected[(size_t)o*y.t+(unsigned)pos]+=x.v[(size_t)i*time+t]*data[((size_t)i*oc+o)*kernel+k];
    }
    assert(!memcmp(expected,y.v,(size_t)oc*y.t*4));
#ifdef GLV_TTS_PARALLEL
    if(big_mode&&!fallback)assert(memory.profile.tts_parallel_jobs==((uint64_t)oc*step*ic*time*2u>=262144u?1u:0u));
#endif
  }
  drop(&x);drop(&y);free(data);free(expected);assert(!memory.used);
}
int main(void)
{
  unsigned times[]={1,2,3,4,5,17,60,255,256,257};
  for(unsigned s=0;s<4;s++)for(unsigned t=0;t<10;t++)for(unsigned fallback=0;fallback<2;fallback++)for(unsigned cancel=0;cancel<2;cancel++)one(s,times[t],fallback,cancel);
  big_mode=true;for(unsigned stage=0;stage<4;stage++)for(unsigned fallback=0;fallback<2;fallback++)for(unsigned cancel=0;cancel<2;cancel++)one(stage,257,fallback,cancel);
  for(output_count=1;output_count<=7;output_count++){
    big_mode=false;for(unsigned stage=0;stage<4;stage++)for(unsigned t=0;t<10;t++)one(stage,times[t],false,false);
    big_mode=true;for(unsigned stage=0;stage<4;stage++)one(stage,257,false,false);
  }
  puts("UPSAMPLE_PAIR_BOUNDARY_ODD_OUTPUT_PARALLEL_PACK_FALLBACK_CANCEL_EXACT_OK cases=484");return 0;
}
