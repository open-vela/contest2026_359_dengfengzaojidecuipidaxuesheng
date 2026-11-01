/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_local_tts.c"
#include <assert.h>
static unsigned fail_at,allocations;
void *__real_calloc(size_t,size_t);
void *__wrap_calloc(size_t n,size_t size)
{if(fail_at&&++allocations==fail_at){errno=ENOMEM;return NULL;}return __real_calloc(n,size);}
static void convolution(bool depth,bool fallback,bool dense)
{
  unsigned ic=depth?1:32,oc=depth?48:5,channels=depth?oc:ic,time=dense?2048:600,kernel=dense?1:depth?11:3;
  unsigned padding=kernel/2;size_t count=(size_t)oc*ic*kernel;
  float *data=calloc(count+oc,4),*reference=calloc((size_t)oc*time,4);assert(data&&reference);
  for(size_t i=0;i<count+oc;i++)data[i]=(float)((int)(i*19%71)-35)*.017f;
  struct glv_memory memory={.limit=GLV_MEMORY_LIMIT};struct glv_tts s={.memory=&memory};
  struct glv_tensor tensors[2]={{.name="test.bias",.shape={oc},.ndim=1,.offset=count*4},{.name="test.weight",.shape={oc,ic,kernel},.ndim=dense?2:3}};
  s.model=(struct glv_model){.memory=&memory,.tensors=tensors,.count=2,.data=(unsigned char *)data,.bytes=(count+oc)*4};
  struct matrix x=make(&s,channels,time);assert(x.v);
  for(unsigned i=0;i<channels*time;i++)x.v[i]=(float)((int)(i*11%47)-23)*.023f;
  fail_at=fallback?4:0;allocations=0;
  struct matrix y=dense?linear(&s,x,"test.weight","test.bias",oc):conv_impl(&s,x,"test",1,padding,false);fail_at=0;
  assert(!s.error&&y.v&&y.t==time);
  for(unsigned o=0;o<oc;o++)for(unsigned t=0;t<time;t++){
    float sum=data[count+o];
    for(unsigned i=0;i<ic;i++)for(unsigned k=0;k<kernel;k++){
      int at=(int)t+(int)k-(int)padding;
      if((unsigned)at<time)sum+=data[((size_t)o*ic+i)*kernel+k]*x.v[(size_t)(depth?o:i)*time+(unsigned)at];
    }
    reference[(size_t)o*time+t]=sum;
  }
  assert(!memcmp(y.v,reference,(size_t)oc*time*4));
  if(fallback)assert(!memory.profile.tts_parallel_jobs&&memory.profile.tts_parallel_fallbacks==1);
  else assert(memory.profile.tts_parallel_jobs==1);
  drop(&x);drop(&y);free(data);free(reference);assert(!memory.used);
}
static void mlp(unsigned time)
{
  unsigned c=CHANNELS;size_t count=(size_t)c*time;
  float *a=calloc(c*c*4,4),*b=calloc(c*c*4,4),*ab=calloc(c*4,4),*bb=calloc(c,4),*scale=calloc(c,4),*reference=calloc(count,4);
  int *mask=calloc(time,sizeof(int));assert(a&&b&&ab&&bb&&scale&&reference&&mask);
  for(unsigned i=0;i<c*c*4;i++){a[i]=(float)((int)(i*19%71)-35)*.0017f;b[i]=(float)((int)(i*11%43)-21)*.0023f;}
  for(unsigned i=0;i<c*4;i++)ab[i]=(float)((int)(i%7)-3)*.013f;
  for(unsigned i=0;i<c;i++){bb[i]=(float)((int)(i%5)-2)*.017f;scale[i]=.11f;}
  struct glv_memory memory={.limit=GLV_MEMORY_LIMIT};struct glv_tts s={.memory=&memory};
  struct matrix x=make(&s,c,time),h=make(&s,c,time);assert(x.v&&h.v);
  for(size_t i=0;i<count;i++){x.v[i]=(float)((int)(i%31)-15)*.013f;h.v[i]=(float)((int)(i%47)-23)*.017f;}
  for(unsigned t=0;t<time;t++)mask[t]=t%3!=0;
  memcpy(reference,x.v,count*4);
  struct matrix ref=x;ref.v=reference;struct block_task task={ref,h,a,b,ab,bb,scale,mask};
  memory.tts_serial_fp=true;assert(!glv_tts_parallel_run(&memory,block_task_body,&task,time,(uint64_t)time*c*c*8u));
  task.x=x;memory.tts_serial_fp=false;assert(!glv_tts_parallel_run(&memory,block_task_body,&task,time,(uint64_t)time*c*c*8u));
  assert(!memcmp(reference,x.v,count*4));assert(memory.profile.tts_parallel_jobs==(time>1));
  drop(&x);drop(&h);assert(!memory.used);free(a);free(b);free(ab);free(bb);free(scale);free(reference);free(mask);
}
int main(void)
{
  for(unsigned fallback=0;fallback<2;fallback++){
    convolution(false,fallback,false);convolution(true,fallback,false);convolution(false,fallback,true);
  }
  mlp(1);mlp(3);mlp(31);mlp(60);
  puts("TTS_PARALLEL_CONV_DEPTH_LINEAR_EXACT_AND_ALLOC_FALLBACK_OK");return 0;
}
