/* SPDX-License-Identifier: Apache-2.0 */
#include "local_tts_int8.c"
#include <assert.h>
#include <float.h>
#include <stdlib.h>
int main(void)
{
 unsigned seed=53,cases=0;
 for(unsigned mode=0;mode<4;mode++){
  assert(!fesetround((int[]){FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}[mode]));
  for(unsigned i=0;i<100000;i++){
   seed=seed*1664525u+1013904223u;
   float scale=ldexpf(1.f+(seed&1023)/1024.f,(int)(seed%181)-90),factor=1.f/scale;
   int zero=(int)(seed%256)-128;float v=((int)(seed%511)-255+.5f)*scale;
   for(unsigned j=0;j<3;j++){
    float value=j==0?nextafterf(v,-INFINITY):j==1?v:nextafterf(v,INFINITY);
    int8_t q=affine_quant_bits(value,scale,factor,zero);
    assert(q==affine_quant(value,scale,factor,zero));
    float z=value/scale;int expected=(int)(z>=0?z+.5f:z-.5f)+zero;
    if(expected< -128)expected=-128;if(expected>127)expected=127;assert(q==expected);cases++;
   }
  }
  for(unsigned exponent=0;exponent<10;exponent++)for(unsigned sign=0;sign<2;sign++)for(unsigned side=0;side<2;side++){
   float value=ldexpf(side?.53125f:.46875f,(int)exponent);if(sign)value=-value;
   for(unsigned j=0;j<3;j++){
    float v=j==0?nextafterf(value,-INFINITY):j==1?value:nextafterf(value,INFINITY);
    assert(affine_quant_bits(v,1.f,1.f,0)==affine_quant(v,1.f,1.f,0));cases++;
   }
  }
  for(unsigned i=0;i<16;i++){
   float v=(float)((int)i-8)*.125f;
   assert(affine_quant_bits(v,1.f,0,-7)==affine_quant(v,1.f,0,-7));cases++;
   v*=4096;assert(affine_quant_bits(v,1.f,1.f,7)==affine_quant(v,1.f,1.f,7));cases++;
  }
 }
 assert(!fesetround(FE_TONEAREST));
 printf("INTEGER_MANTISSA_GUARDED_ROUNDING_OLD_AND_DIVISION_REFERENCE_FENV_OK cases=%u\\n",cases);return 0;
}
