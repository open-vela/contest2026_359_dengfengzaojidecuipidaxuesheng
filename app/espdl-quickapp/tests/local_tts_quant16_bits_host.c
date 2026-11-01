/* SPDX-License-Identifier: Apache-2.0 */
#include "local_tts_pie.c"
#include <assert.h>
#include <float.h>
int main(void)
{
 unsigned seed=54,cases=0;
 for(unsigned mode=0;mode<4;mode++){
  assert(!fesetround((int[]){FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}[mode]));
  for(unsigned i=0;i<100000;i++){
   seed=seed*1664525u+1013904223u;
   float maximum=ldexpf(1.f+(seed&1023)/1024.f,(int)(seed%181)-90),factor=32767.f/maximum;
   float v=((int)(seed%65535)-32767+.5f)/32767.f*maximum;
   for(unsigned j=0;j<3;j++){
    float value=j==0?nextafterf(v,-INFINITY):j==1?v:nextafterf(v,INFINITY);
    int16_t q=quant_bits16(value,maximum,factor);
    assert(q==quant_recip(value,maximum,factor,32767)&&q==quant(value,maximum,32767));cases++;
   }
  }
  assert(quant_bits16(0,0,0)==0);
  for(unsigned k=0;k<17;k++)for(unsigned sign=0;sign<2;sign++)for(unsigned side=0;side<2;side++){
   float v=ldexpf(side?.53125f:.46875f,(int)k)/32767.f;if(sign)v=-v;
   assert(quant_bits16(v,1.f,32767.f)==quant_recip(v,1.f,32767.f,32767));cases++;
  }
 }
 assert(!fesetround(FE_TONEAREST));
 printf("STABLE16_INTEGER_GUARD_OLD_AND_DIVISION_REFERENCE_FENV_OK cases=%u\\n",cases);return 0;
}
