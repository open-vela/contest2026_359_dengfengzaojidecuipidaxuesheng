/* SPDX-License-Identifier: Apache-2.0 */
#include "local_asr_dsp.h"
#include <errno.h>
#include <string.h>
#define dsps_add_f32_ansi glv_esp_dsp_add_f32_ansi
#define dsps_sub_f32_ansi glv_esp_dsp_sub_f32_ansi
#define dsps_mul_f32_ansi glv_esp_dsp_mul_f32_ansi
#include "esp_dsp/dsps_add.h"
#include "esp_dsp/dsps_sub.h"
#include "esp_dsp/dsps_mul.h"

static int invoke(enum glv_asr_binary op,const float *x,const float *z,float *y,unsigned n,unsigned sx,unsigned sz)
{
 if(op==GLV_ASR_ADD)return glv_esp_dsp_add_f32_ansi(x,z,y,(int)n,(int)sx,(int)sz,1);
 if(op==GLV_ASR_SUB)return glv_esp_dsp_sub_f32_ansi(x,z,y,(int)n,(int)sx,(int)sz,1);
 return glv_esp_dsp_mul_f32_ansi(x,z,y,(int)n,(int)sx,(int)sz,1);
}
static bool qualify(void)
{
 const float x[8]={0.f,-0.f,.25f,-.5f,2.f,-3.f,16.f,-32.f};
 const float z[8]={-0.f,0.f,-.5f,.25f,-3.f,2.f,-32.f,16.f};
 for(unsigned op=0;op<3;op++)for(unsigned sx=0;sx<2;sx++)for(unsigned sz=0;sz<2;sz++){
  float expected[8],actual[8];
  for(unsigned i=0;i<8;i++){float a=x[i*sx],b=z[i*sz];expected[i]=op==0?a+b:op==1?a-b:a*b;}
  if(invoke((enum glv_asr_binary)op,x,z,actual,8,sx,sz)||memcmp(expected,actual,sizeof(actual)))return false;
 }
 float alias[8],expected[8];memcpy(alias,x,sizeof(alias));
 for(unsigned i=0;i<8;i++)expected[i]=x[i]+z[i];
 return !invoke(GLV_ASR_ADD,alias,z,alias,8,1,1)&&!memcmp(alias,expected,sizeof(alias));
}
int glv_asr_dsp_binary(struct glv_memory *m,enum glv_asr_binary op,const float *x,const float *z,float *y,unsigned n,unsigned sx,unsigned sz)
{
 if(!m||!x||!z||!y||!n||n>1024||sx>1||sz>1)return -EINVAL;
 if(op>GLV_ASR_MUL||op<GLV_ASR_ADD||m->asr_reference)return 1;
 if(!m->asr_dsp_state){m->asr_dsp_state=qualify()?1:-1;m->profile.asr_dsp_selftests++;}
 if(m->asr_dsp_state<0){m->profile.asr_dsp_fallbacks++;return 1;}
 int ret=invoke(op,x,z,y,n,sx,sz);if(ret)return -EIO;
 m->profile.asr_dsp_calls++;m->profile.asr_dsp_elements+=n;return 0;
}
