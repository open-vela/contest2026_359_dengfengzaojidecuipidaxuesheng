/* SPDX-License-Identifier: Apache-2.0 */
#include "local_tts_pie.c"
#include <assert.h>
#include <stdlib.h>
static unsigned fail_at,allocations;
void *__real_calloc(size_t,size_t);
void *__wrap_calloc(size_t n,size_t size)
{if(fail_at&&++allocations==fail_at){errno=ENOMEM;return NULL;}return __real_calloc(n,size);}
static bool cancelled(void *unused){(void)unused;return true;}
int main(void)
{
  unsigned quant_seed=19;
  for(unsigned trial=0;trial<1000000;trial++){
    quant_seed=quant_seed*1664525u+1013904223u;uint32_t bits=quant_seed&0x7fffffffu;
    float classified;memcpy(&classified,&bits,4);assert(finite_bits(classified)==glv_finite(classified));
    if(!bits||bits>=0x7f800000u)continue;float maximum;memcpy(&maximum,&bits,4);
    int limit=trial&1?32767:127;float factor=(float)limit/maximum;
    if(!glv_finite(factor)||factor<=0)factor=0;
    quant_seed=quant_seed*1664525u+1013904223u;
    unsigned q=quant_seed%(unsigned)limit;
    float value=((float)q+.5f)/(float)limit*maximum;
    for(int offset=-2;offset<=2;offset++){
      float v=value;for(int j=0;j<abs(offset);j++)v=nextafterf(v,offset<0?0:INFINITY);
      if(glv_finite(v)&&v<=maximum){
        assert(quant_recip(v,maximum,factor,limit)==quant(v,maximum,limit));
        assert(quant_recip(-v,maximum,factor,limit)==quant(-v,maximum,limit));
      }
    }
  }
  const uint32_t classes[]={0,0x80000000u,1,0x80000001u,0x7f7fffffu,0xff7fffffu,
    0x7f800000u,0xff800000u,0x7fc00000u,0xffc00000u,0x7f800001u,0xff800001u};
  for(unsigned i=0;i<sizeof(classes)/sizeof(classes[0]);i++){
    float v;memcpy(&v,&classes[i],4);assert(finite_bits(v)==glv_finite(v));
  }
  _Alignas(16) int16_t w[256],x[256];unsigned seed=1;
  for(unsigned trial=0;trial<4096;trial++){
    int64_t expected=0;
    for(unsigned i=0;i<256;i++){
      seed=seed*1664525u+1013904223u;w[i]=(int)((seed>>16)%255)-127;
      seed=seed*1664525u+1013904223u;x[i]=(int)((seed>>1)%65535)-32767;
      if(trial<4){w[i]=trial&1?127:-127;x[i]=trial&2?32767:-32767;}
      expected+=(int32_t)w[i]*x[i];
    }
    assert(dot(w,x,256)==expected);
  }
  struct glv_memory memory={.limit=GLV_MEMORY_LIMIT};unsigned char data[26];
  for(unsigned o=0;o<2;o++){
    float scale=.01f;memcpy(data+o*13,&scale,4);
    for(unsigned i=0;i<9;i++)data[o*13+4+i]=(unsigned char)(int8_t)(i==0?127:(int)i*17-100);
  }
  struct glv_model model={.memory=&memory,.data=data,.bytes=sizeof(data)};
  struct glv_tensor tensor={.shape={2,3,3},.ndim=3,.type=2,.stride=13};
  float input[15],output[10],bias[2]={.1f,-.2f};
  for(unsigned i=0;i<15;i++)input[i]=(float)((int)i-7)*.11f;
  assert(!glv_tts_pie_conv(&model,&tensor,input,3,5,output,2,5,1,1,bias,true,false));
  for(unsigned o=0;o<2;o++)for(unsigned t=0;t<5;t++){
    float expected=bias[o];
    for(unsigned c=0;c<3;c++)for(unsigned k=0;k<3;k++){
      int pos=(int)t+(int)k-1;if(pos<0||pos>=5)continue;
      float v=input[c*5+(unsigned)pos];if(v<0)v*=.1f;
      expected+=v*((int8_t)data[o*13+4+c*3+k]*.01f);
    }
    assert(fabsf(expected-output[o*5+t])<.001f);
  }
  assert(!memory.used);
  float batch_output[10];memcpy(batch_output,output,sizeof(output));memory.tts_single_dot=true;
  assert(!glv_tts_pie_conv(&model,&tensor,input,3,5,output,2,5,1,1,bias,true,false));
  assert(!memcmp(batch_output,output,sizeof(output)));assert(!memory.used);
  for(unsigned n=1;n<=6;n++){
    fail_at=n;allocations=0;
    assert(glv_tts_pie_conv(&model,&tensor,input,3,5,output,2,5,1,1,bias,false,false)==-ENOMEM);
    assert(!memory.used);fail_at=0;
  }
  memory.cancelled=cancelled;
  assert(glv_tts_pie_conv(&model,&tensor,input,3,5,output,2,5,1,1,bias,false,false)==-ECANCELED);
  assert(!memory.used);memory.cancelled=NULL;input[0]=NAN;
  assert(glv_tts_pie_conv(&model,&tensor,input,3,5,output,2,5,1,1,bias,false,false)==-EDOM);
  assert(!memory.used);
  puts("TTS_PIE_HOST_DOT_BOUNDS_CONV_PADDING_ALLOC_CANCEL_NAN_OK");return 0;
}
