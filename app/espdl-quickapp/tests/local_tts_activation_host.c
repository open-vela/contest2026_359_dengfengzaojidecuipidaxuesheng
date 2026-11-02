/* SPDX-License-Identifier: Apache-2.0 */
/* Test real private kernels, including optional activation allocation failure. */
#include "glass_local_tts.c"
#include <assert.h>
static unsigned allocations,fail_at;
void *__real_calloc(size_t,size_t);
void *__wrap_calloc(size_t n,size_t size)
{if(fail_at&&++allocations==fail_at){errno=ENOMEM;return NULL;}return __real_calloc(n,size);}
static bool stop(void *opaque){return *(bool *)opaque;}
static void one(unsigned channels,unsigned time,unsigned kernel,unsigned dilation,bool depth,bool fallback)
{
  struct glv_memory memory={.limit=GLV_MEMORY_LIMIT};struct glv_tts s={.memory=&memory};
  unsigned oc=depth?channels:3,ic=depth?1:channels,pad=dilation*(kernel-1)/2;
  size_t coefficients=(size_t)oc*ic*kernel;
  float *data=calloc(coefficients+oc,4);assert(data);
  for(size_t i=0;i<coefficients+oc;i++)data[i]=(float)((int)(i*17%53)-26)*.013f;
  struct glv_tensor tensors[2]={
    {.name="probe.bias",.shape={oc},.ndim=1,.offset=coefficients*4},
    {.name="probe.weight",.shape={oc,ic,kernel},.ndim=3}};
  s.model=(struct glv_model){.memory=&memory,.tensors=tensors,.count=2,
    .data=(unsigned char *)data,.bytes=(coefficients+oc)*4};
  struct matrix x=make(&s,channels,time);assert(x.v);
  for(size_t i=0;i<(size_t)channels*time;i++)x.v[i]=(float)((int)(i*31%97)-48)*.021f;
  allocations=0;fail_at=fallback?4:0;
  struct matrix y=conv_impl(&s,x,"probe",dilation,pad,true);fail_at=0;
  assert(!s.error&&y.v&&y.t==time);if(fallback)assert(allocations>=5);
  for(unsigned o=0;o<oc;o++)for(unsigned t=0;t<time;t++){
    float expected=data[coefficients+o];
    for(unsigned i=0;i<ic;i++)for(unsigned k=0;k<kernel;k++){
      int pos=(int)t+(int)(k*dilation)-(int)pad;
      if(pos>=0&&(unsigned)pos<time){
        float v=x.v[(size_t)(depth?o:i)*time+(unsigned)pos];if(v<0)v*=.1f;
        expected+=v*data[((size_t)o*ic+i)*kernel+k];
      }
    }
    assert(!memcmp(&expected,y.v+(size_t)o*time+t,4));
  }
  drop(&y);
  bool cancelled=true;memory.cancelled=stop;memory.opaque=&cancelled;
  y=conv_impl(&s,x,"probe",dilation,pad,true);assert(s.error==-ECANCELED&&!y.v);
  drop(&x);free(data);assert(!memory.used);
}
int main(void)
{
  unsigned cases=0;
  const unsigned times[]={1,2,3,4,17,255,256,257,513},kernels[]={1,3,7,11};
  for(unsigned t=0;t<9;t++)for(unsigned k=0;k<4;k++)for(unsigned d=1;d<=5;d+=2)
    for(unsigned depth=0;depth<2;depth++)for(unsigned fallback=0;fallback<2;fallback++){
      one(3,times[t],kernels[k],d,depth,fallback);cases++;
    }
  puts("TTS_ACTIVATION_BOUNDARIES_FALLBACK_CANCEL_EXACT_OK");
  printf("cases=%u\n",cases);return 0;
}
