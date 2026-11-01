/* SPDX-License-Identifier: Apache-2.0 */
#include "local_tts_int8.c"
#include <assert.h>
#include <float.h>
#include <stdlib.h>
int main(void)
{
 unsigned seed=52,cases=0;
 for(unsigned mode=0;mode<4;mode++){
  assert(!fesetround((int[]){FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}[mode]));
  for(unsigned test=0;test<5000;test++){
   float x[39],lo=0,hi=0,a=0,b=0;int8_t y[41];unsigned n=test%34;bool leaky=test&1;
   for(unsigned i=0;i<n;i++){
    seed=seed*1664525u+1013904223u;
    x[i]=ldexpf((float)((int)(seed%511)-255),(int)(seed%241)-120);
    if(test%7==0)x[i]=(i&1)?-0.f:0.f;
    if(test%11==0)x[i]=nextafterf(0.f,(i&1)?-1.f:1.f);
    if(test%13==0)x[i]=(i&1)?-FLT_MAX:FLT_MAX;
    float v=x[i];if(leaky&&v<0)v*=.1f;if(v<a)a=v;if(v>b)b=v;
   }
   assert(!range_row4(x,n,leaky,&lo,&hi)&&!memcmp(&lo,&a,4)&&!memcmp(&hi,&b,4));
   float scale=hi>lo?(hi-lo)/255.f:1.f;
   if(finite_bits(scale)&&scale>0){
    float factor=1.f/scale;if(!finite_bits(factor)||factor<=0)factor=0;
    int zero=hi>lo?(int)(-lo/scale+.5f)-128:0;if(zero< -128)zero=-128;if(zero>127)zero=127;
    memset(y,93,sizeof(y));quant_row4(x,y+1,n,scale,factor,zero,leaky);
    assert(y[0]==93&&y[n+1]==93);
    for(unsigned i=0;i<n;i++){
     float v=x[i];if(leaky&&v<0)v*=.1f;float qf=v/scale;int q=(int)(qf>=0?qf+.5f:qf-.5f)+zero;
     if(q< -128)q=-128;if(q>127)q=127;assert(y[i+1]==q);
    }
   }
   cases++;
  }
 }
 assert(!fesetround(FE_TONEAREST));float bad[4]={0,1,2,NAN},lo=0,hi=0;assert(range_row4(bad,4,false,&lo,&hi)==-EDOM);
 bad[3]=INFINITY;assert(range_row4(bad,4,true,&lo,&hi)==-EDOM);
 printf("INPUT4_RANGE_QUANT_INDEPENDENT_REFERENCE_CANARY_SUBNORMAL_EXTREME_FENV_OK cases=%u\\n",cases);return 0;
}
