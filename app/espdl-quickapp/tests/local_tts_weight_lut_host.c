/* SPDX-License-Identifier: Apache-2.0 */
#include "local_tts_pie.c"
#include <assert.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <float.h>
static unsigned fail_alloc,allocations,stop_at,tests;
static atomic_uint polls;
void *__real_calloc(size_t,size_t);
void *__wrap_calloc(size_t n,size_t size)
{if(fail_alloc&&++allocations==fail_alloc){errno=ENOMEM;return NULL;}return __real_calloc(n,size);}
static bool cancelled(void *unused)
{(void)unused;unsigned n=atomic_fetch_add(&polls,1)+1;return stop_at&&n>=stop_at;}
static void one(unsigned c,unsigned o,unsigned k,unsigned time,unsigned dilation,int rounding,bool parallel,unsigned pattern)
{
  assert(!fesetround(rounding));unsigned row=c*k+4,pad=dilation*(k-1)/2;
  size_t points=(size_t)c*time,n=(size_t)o*time+8;
  float *x=malloc(points*4),*a=malloc(n*4),*b=malloc(n*4),*bias=calloc(o,4);
  unsigned char *data=malloc((size_t)o*row);assert(x&&a&&b&&bias&&data);
  unsigned seed=83;
  for(size_t i=0;i<points;i++){seed=seed*1664525u+1013904223u;x[i]=((int)(seed%65535)-32767)/32768.f;}
  for(unsigned r=0;r<o;r++){
    float scale=pattern==2?0x1p-140f:.00013f*(1+r%7);memcpy(data+r*row,&scale,4);
    for(unsigned i=0;i<c*k;i++){
      seed=seed*1664525u+1013904223u;
      data[r*row+4+i]=pattern==1?0:(unsigned char)(int8_t)((int)(seed%256)-128);
    }
  }
  struct glv_memory m={.limit=GLV_MEMORY_LIMIT,.cancelled=cancelled,.tts_parallel_pie=parallel};
  struct glv_model model={.memory=&m,.data=data,.bytes=(size_t)o*row};
  struct glv_tensor w={.shape={o,c,k},.ndim=3,.type=2,.stride=row};
  for(size_t i=0;i<n;i++)a[i]=b[i]=.125f;
  assert(!glv_tts_pie_conv(&model,&w,x,c,time,a+4,o,time,dilation,pad,bias,true,true)&&!m.used);
  size_t peak=m.peak;uint64_t dots=m.profile.tts_pie_dot_calls;unsigned batches=m.profile.tts_pie_batches;
  m=(struct glv_memory){.limit=GLV_MEMORY_LIMIT,.cancelled=cancelled,.tts_parallel_pie=parallel,.tts_weight_lut=true};
  atomic_store(&polls,0);
  assert(!glv_tts_pie_conv(&model,&w,x,c,time,b+4,o,time,dilation,pad,bias,true,true)&&!m.used);
  assert(!memcmp(a,b,n*4)&&peak==m.peak&&dots==m.profile.tts_pie_dot_calls&&batches==m.profile.tts_pie_batches);
  unsigned complete=atomic_load(&polls),stride=(c*k+7)&~7u;
  bool eligible=c*k>=256&&(size_t)o*PATCHES*((stride+255)/256)*4>=1024;
  if(eligible&&rounding==FE_TONEAREST&&pattern!=1){
    assert(m.profile.tts_weight_lut_rows==o);
    if(c*k>256)assert(m.profile.tts_weight_lut_hits>0);
    assert(m.profile.tts_weight_lut_hits+m.profile.tts_weight_lut_misses==(uint64_t)o*c*k);
  }else assert(!m.profile.tts_weight_lut_rows&&!m.profile.tts_weight_lut_hits&&!m.profile.tts_weight_lut_misses);
  assert(m.profile.tts_weight_lut_rounding_fallbacks==(eligible&&rounding!=FE_TONEAREST?1u:0u));
  for(unsigned at=0;at<3;at++){
    m=(struct glv_memory){.limit=GLV_MEMORY_LIMIT,.cancelled=cancelled,.tts_parallel_pie=parallel,.tts_weight_lut=true};
    atomic_store(&polls,0);stop_at=(unsigned[]){1,c+2,complete/2}[at];
    assert(glv_tts_pie_conv(&model,&w,x,c,time,b+4,o,time,dilation,pad,bias,true,true)==-ECANCELED&&!m.used);stop_at=0;
  }
  for(unsigned at=1;at<=6;at++){
    m=(struct glv_memory){.limit=GLV_MEMORY_LIMIT,.tts_weight_lut=true};allocations=0;fail_alloc=at;
    assert(glv_tts_pie_conv(&model,&w,x,c,time,b+4,o,time,dilation,pad,bias,true,true)==-ENOMEM&&!m.used);fail_alloc=0;
  }
  assert(!fesetround(FE_TONEAREST));free(x);free(a);free(b);free(bias);free(data);tests++;
}
int main(void)
{
  struct weight_entry table[256]={0};unsigned hits=0,misses=0,seed=41;
  /* All raw Q8 codes, very small/large scales,
   * and generation changes must preserve the original quant_recip result. */
  for(unsigned generation=1;generation<=208;generation++){
    float scale=ldexpf(1.017f,(int)(generation%250)-145),peak=128*scale;
    float factor=127.f/peak;if(!finite_bits(factor)||factor<=0)factor=0;
    for(unsigned i=0;i<8192;i++){
      seed=seed*1664525u+1013904223u;unsigned char code=seed%256;float v=scale*(int8_t)code;
      assert(weight_quant(table,generation,code,scale,peak,factor,&hits,&misses)==quant_recip(v,peak,factor,127));
    }
  }
  memset(table,0,sizeof(table));
  assert(weight_quant(table,1,1,.25f,1,127,&hits,&misses)==32);
  assert(weight_quant(table,2,1,.25f,2,63.5f,&hits,&misses)==16);
  for(unsigned trial=0;trial<20000;trial++){
    seed=seed*1664525u+1013904223u;uint32_t bits=(seed%0x7f7fffffu)+1;float scale;memcpy(&scale,&bits,4);
    unsigned maxcode=0;float reference=0;
    for(unsigned i=0;i<256;i++){
      seed=seed*1664525u+1013904223u;int q=(int8_t)(unsigned char)seed;
      unsigned magnitude=q<0?-q:q;if(magnitude>maxcode)maxcode=magnitude;
      float v=fabsf(scale*q);if(v>reference)reference=v;
    }
    float direct=scale*(float)maxcode;assert(!memcmp(&direct,&reference,4));
  }
  const unsigned dims[][5]={{85,32,3,17,1},{64,31,4,17,2},{64,32,4,17,2},{73,104,7,17,1},{64,63,8,17,2},{64,64,8,17,2},{57,64,9,17,1},{52,52,11,33,1},{104,104,7,17,64},{208,208,11,17,1}};
  for(unsigned d=0;d<10;d++)for(unsigned p=0;p<2;p++)for(unsigned pattern=0;pattern<3;pattern++)
    one(dims[d][0],dims[d][1],dims[d][2],dims[d][3],dims[d][4],FE_TONEAREST,p,pattern);
  for(unsigned mode=0;mode<3;mode++)one(64,64,8,17,2,(int[]){FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}[mode],true,0);
  {
    unsigned c=64,o=32,k=4,time=17,count=c*k,stride=count+4;
    unsigned char *data=calloc(o,stride);float *in=calloc(c*time,4),*out=calloc(o*time,4),bias[32]={0};assert(data&&in&&out);
    struct glv_memory m;struct glv_model model={.memory=&m,.data=data,.bytes=o*stride};
    struct glv_tensor w={.shape={o,c,k},.ndim=3,.type=2,.stride=stride};
    for(unsigned test=0;test<2;test++){
      float scale=test?0x1p-149f:FLT_MAX;
      for(unsigned r=0;r<o;r++){memcpy(data+r*stride,&scale,4);data[r*stride+4]=test?1:2;}
      for(unsigned enabled=0;enabled<2;enabled++){
        m=(struct glv_memory){.limit=GLV_MEMORY_LIMIT,.tts_weight_lut=enabled};
        assert(glv_tts_pie_conv(&model,&w,in,c,time,out,o,time,2,3,bias,false,false)==(test?-ERANGE:-EBADMSG)&&!m.used);
      }
    }
    free(data);free(in);free(out);
  }
  printf("WEIGHT_Q8_CODES_ENDPOINT_GENERATIONS_CANARY_SAME_PEAK_CANCEL_ALLOC_FENV_EXACT_OK cases=%u\n",tests);return 0;
}
