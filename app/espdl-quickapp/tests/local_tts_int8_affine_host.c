/* SPDX-License-Identifier: Apache-2.0 */
#include "local_tts_int8.c"
#include <assert.h>
#include <stdlib.h>
static void one(unsigned channels,unsigned time,unsigned kernel,unsigned outputs,unsigned dilation,bool leaky,bool add)
{
 unsigned count=channels*kernel,stride=(count+15u)&~15u,pad=dilation*(kernel-1)/2;size_t n=(size_t)outputs*time;
 float *x=malloc((size_t)channels*time*4),*y=malloc((n+2)*4),*expected=malloc(n*4),*bias=malloc(outputs*4);
 int8_t *qx=malloc((size_t)channels*time);unsigned char *data=malloc((size_t)outputs*(count+4));assert(x&&y&&expected&&bias&&qx&&data);
 unsigned seed=count+time;float lo=0,hi=0;
 for(unsigned i=0;i<channels*time;i++){seed=seed*1664525u+1013904223u;x[i]=((int)(seed%1001)-300)*.007f;float v=x[i];if(leaky&&v<0)v*=.1f;if(v<lo)lo=v;if(v>hi)hi=v;}
 float xs=hi>lo?(hi-lo)/255.f:1.f;int zero=hi>lo?(int)(-lo/xs+.5f)-128:0;if(zero< -128)zero=-128;if(zero>127)zero=127;
 for(unsigned i=0;i<channels*time;i++){float v=x[i];if(leaky&&v<0)v*=.1f;float a=v/xs;int q=(int)(a>=0?a+.5f:a-.5f)+zero;if(q< -128)q=-128;if(q>127)q=127;qx[i]=(int8_t)q;}
 for(unsigned o=0;o<outputs;o++){
  float scale=.003f+o*.0001f;memcpy(data+(size_t)o*(count+4),&scale,4);int32_t rowsum=0;bias[o]=o*.003f;
  for(unsigned i=0;i<count;i++){seed=seed*1664525u+1013904223u;int q=i?(int)(seed%255)-127:127;data[(size_t)o*(count+4)+4+i]=(unsigned char)(int8_t)q;rowsum+=q;}
  float ws=(scale*127.f)/127.f,adjusted=bias[o]-((float)(rowsum*zero)*ws)*xs;
  for(unsigned t=0;t<time;t++){
   float value=0;
   for(unsigned block=0;block<stride;block+=256){
    int32_t sum=0;unsigned end=block+256;if(end>count)end=count;
    for(unsigned i=block;i<end;i++){unsigned c=i/kernel,k=i%kernel;int pos=(int)t+(int)(k*dilation)-(int)pad;int q=(unsigned)pos<time?qx[(size_t)c*time+(unsigned)pos]:zero;sum+=(int8_t)data[(size_t)o*(count+4)+4+i]*q;}
    value+=((float)sum*ws)*xs;
   }
   value+=adjusted;if(add)value+=.125f;expected[(size_t)o*time+t]=value;
  }
 }
 unsigned char marker=1;struct glv_memory m={.limit=GLV_MEMORY_LIMIT,.tts_parallel_input=true,.tts_parallel_pie=true,.tts_int8_policy=&marker};
 struct glv_model model={.memory=&m,.data=data,.bytes=(size_t)outputs*(count+4)};struct glv_tensor w={.ndim=3,.type=2,.shape={outputs,channels,kernel},.stride=count+4};
 for(unsigned mode=0;mode<4;mode++){
  m.tts_single_dot=mode&1;m.tts_weight_lut=(mode&2)!=0;
  for(size_t i=0;i<n;i++)y[i+1]=.125f;y[0]=y[n+1]=12345.5f;
  assert(!glv_tts_int8_conv(&model,&w,x,channels,time,y+1,outputs,time,dilation,pad,bias,leaky,add));
  assert(!memcmp(expected,y+1,n*4)&&y[0]==12345.5f&&y[n+1]==12345.5f&&!m.used);
 }
 free(data);free(qx);free(bias);free(expected);free(y);free(x);
}
int main(void)
{
 unsigned cases=0;
 for(unsigned c=0;c<5;c++)for(unsigned t=0;t<5;t++){one((unsigned[]){1,3,16,33,208}[c],(unsigned[]){1,7,8,9,17}[t],c&1?3:11,13,t&1?3:1,c&1,t&1);cases++;}
 one(208,17,11,208,3,true,true);cases++;
 unsigned seed=91;
 for(unsigned i=0;i<100000;i++){
  seed=seed*1664525u+1013904223u;float scale=ldexpf(1.f+(seed&1023)/1024.f,(int)(seed%180)-90),factor=1.f/scale;int zero=(int)(seed%256)-128;
  float v=((int)(seed%511)-255+.5f)*scale;
  for(unsigned j=0;j<3;j++){
   float value=j==0?nextafterf(v,-INFINITY):j==1?v:nextafterf(v,INFINITY);float a=value/scale;int q=(int)(a>=0?a+.5f:a-.5f)+zero;if(q< -128)q=-128;if(q>127)q=127;
   assert(affine_quant(value,scale,factor,zero)==q);
  }
 }
 printf("AFFINE_INTEGER_REFERENCE_PADDING_BIAS_RESIDUAL_RECIPROCAL_CANARY_OK cases=%u x4\n",cases);return 0;
}
