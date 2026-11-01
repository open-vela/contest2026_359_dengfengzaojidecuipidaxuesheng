/* SPDX-License-Identifier: Apache-2.0 */
#include "local_tts_int8.c"
#include <assert.h>
#include <stdlib.h>
static unsigned fail_at,allocations;
void *__real_calloc(size_t,size_t);
void *__wrap_calloc(size_t n,size_t size)
{if(fail_at&&++allocations==fail_at){errno=ENOMEM;return NULL;}return __real_calloc(n,size);}
static bool cancelled(void *unused){(void)unused;return true;}

static void integer_reference_case(unsigned channels,unsigned time,unsigned kernel,
                                   unsigned outputs,unsigned dilation,bool leaky,bool add)
{
  unsigned count=channels*kernel,stride=(count+15u)&~15u,padding=dilation*(kernel-1)/2;
  size_t values=(size_t)outputs*time;
  unsigned char *raw=malloc((size_t)outputs*(count+4));
  float *x=malloc((size_t)channels*time*4),*y=malloc((values+2)*4),*expected=malloc(values*4),*bias=malloc(outputs*4);
  int8_t *qx=malloc((size_t)channels*time);assert(raw&&x&&y&&expected&&bias&&qx);
  unsigned seed=channels*71u+time+kernel;float peak=0;
  for(unsigned i=0;i<channels*time;i++){
    seed=seed*1664525u+1013904223u;x[i]=((int)(seed%1001)-500)*.007f;
    float a=x[i];if(leaky&&a<0)a*=.1f;if(fabsf(a)>peak)peak=fabsf(a);
  }
  for(unsigned i=0;i<channels*time;i++){float v=x[i];if(leaky&&v<0)v*=.1f;qx[i]=quant(v,peak,127);}
  for(unsigned o=0;o<outputs;o++){
    float scale=.003f+(float)o*.0001f;memcpy(raw+(size_t)o*(count+4),&scale,4);bias[o]=o*.003f;
    for(unsigned i=0;i<count;i++){seed=seed*1664525u+1013904223u;raw[(size_t)o*(count+4)+4+i]=(unsigned char)(int8_t)((int)(seed%255)-127);}
    raw[(size_t)o*(count+4)+4]=127;
    float actual_scale=(scale*127.f)/127.f,xs=peak?peak/127.f:1.f;
    for(unsigned t=0;t<time;t++){
      float value=0;
      for(unsigned block=0;block<stride;block+=256){
        int32_t acc=0;unsigned end=block+256;if(end>count)end=count;
        for(unsigned i=block;i<end;i++){
          unsigned c=i/kernel,k=i%kernel;int pos=(int)t+(int)(k*dilation)-(int)padding;
          if((unsigned)pos<time)acc+=(int8_t)raw[(size_t)o*(count+4)+4+i]*(int32_t)qx[(size_t)c*time+(unsigned)pos];
        }
        value+=((float)acc*actual_scale)*xs;
      }
      value+=bias[o];if(add)value+=.125f;expected[(size_t)o*time+t]=value;
    }
  }
  struct glv_memory m={.limit=GLV_MEMORY_LIMIT,.tts_parallel_input=true,.tts_parallel_pie=true};
  struct glv_model model={.memory=&m,.data=raw,.bytes=(size_t)outputs*(count+4)};
  struct glv_tensor tensor={.ndim=3,.type=2,.shape={outputs,channels,kernel},.stride=count+4};
  for(unsigned mode=0;mode<4;mode++){
    m.tts_single_dot=mode&1;m.tts_weight_lut=(mode&2)!=0;
    for(size_t i=0;i<values;i++)y[1+i]=.125f;y[0]=y[values+1]=12345.5f;
    assert(!glv_tts_int8_conv(&model,&tensor,x,channels,time,y+1,outputs,time,dilation,padding,bias,leaky,add));
    assert(!memcmp(y+1,expected,values*4));assert(y[0]==12345.5f&&y[values+1]==12345.5f);assert(!m.used);
  }
  free(qx);free(bias);free(expected);free(y);free(x);free(raw);
}
int main(void)
{
  unsigned cases=0;
  const unsigned channels[]={1,3,16,33,208},times[]={1,7,8,9,17};
  for(unsigned c=0;c<5;c++)for(unsigned t=0;t<5;t++){integer_reference_case(channels[c],times[t],c&1?3:11,13,t&1?3:1,c&1,t&1);cases++;}
  integer_reference_case(208,17,11,208,3,true,true);cases++;
  printf("INT8_EXACT_INTEGER_CONV_CASES %u x4_modes\n",cases);
  unsigned quant_seed=19;
  for(unsigned trial=0;trial<1000000;trial++){
    quant_seed=quant_seed*1664525u+1013904223u;uint32_t bits=quant_seed&0x7fffffffu;
    float classified;memcpy(&classified,&bits,4);assert(finite_bits(classified)==glv_finite(classified));
    if(!bits||bits>=0x7f800000u)continue;float maximum;memcpy(&maximum,&bits,4);
    int limit=trial&1?127:127;float factor=(float)limit/maximum;
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
  _Alignas(16) int8_t w[256],x[256];unsigned seed=1;
  for(unsigned trial=0;trial<4096;trial++){
    int64_t expected=0;
    for(unsigned i=0;i<256;i++){
      seed=seed*1664525u+1013904223u;w[i]=(int)((seed>>16)%255)-127;
      seed=seed*1664525u+1013904223u;x[i]=(int)((seed>>1)%255)-127;
      if(trial<4){w[i]=trial&1?127:-127;x[i]=trial&2?127:-127;}
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
  assert(!glv_tts_int8_conv(&model,&tensor,input,3,5,output,2,5,1,1,bias,true,false));
  for(unsigned o=0;o<2;o++)for(unsigned t=0;t<5;t++){
    float expected=bias[o];
    for(unsigned c=0;c<3;c++)for(unsigned k=0;k<3;k++){
      int pos=(int)t+(int)k-1;if(pos<0||pos>=5)continue;
      float v=input[c*5+(unsigned)pos];if(v<0)v*=.1f;
      expected+=v*((int8_t)data[o*13+4+c*3+k]*.01f);
    }
    assert(fabsf(expected-output[o*5+t])<.04f);
  }
  assert(!memory.used);
  float batch_output[10];memcpy(batch_output,output,sizeof(output));memory.tts_single_dot=true;
  assert(!glv_tts_int8_conv(&model,&tensor,input,3,5,output,2,5,1,1,bias,true,false));
  assert(!memcmp(batch_output,output,sizeof(output)));assert(!memory.used);
  for(unsigned n=1;n<=6;n++){
    fail_at=n;allocations=0;
    assert(glv_tts_int8_conv(&model,&tensor,input,3,5,output,2,5,1,1,bias,false,false)==-ENOMEM);
    assert(!memory.used);fail_at=0;
  }
  memory.cancelled=cancelled;
  assert(glv_tts_int8_conv(&model,&tensor,input,3,5,output,2,5,1,1,bias,false,false)==-ECANCELED);
  assert(!memory.used);memory.cancelled=NULL;input[0]=NAN;
  assert(glv_tts_int8_conv(&model,&tensor,input,3,5,output,2,5,1,1,bias,false,false)==-EDOM);
  assert(!memory.used);
  puts("TTS_INT8_HOST_DOT_BOUNDS_CONV_PADDING_ALLOC_CANCEL_NAN_OK");return 0;
}
