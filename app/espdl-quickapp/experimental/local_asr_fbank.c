/* SPDX-License-Identifier: Apache-2.0 */
/* Fixed-profile independent C implementation of Kaldi-style fbank equations.
 * Reference behavior: kaldi-native-fbank (Apache-2.0), Xiaomi Corporation /
 * Kaldi authors. See experimental/ASR-SOURCES.txt for pinned references. */
#include "local_asr_fbank.h"
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <string.h>

#define FFT 512u
#define WINDOW 400u
#define BINS 80u
#define MEL_WIDTH 32u
struct glv_asr_fbank {
 struct glv_memory *memory;
 float window[WINDOW],cosine[FFT/2],sine[FFT/2];
 float mel[BINS][MEL_WIDTH];unsigned first[BINS],length[BINS];
 float real[FFT],imag[FFT],feature[BINS];
};
static float mel(float hz){return 1127.0f*logf(1.0f+hz/700.0f);}
void glv_asr_fbank_destroy(struct glv_asr_fbank *f){glv_free(f);}
int glv_asr_fbank_create(struct glv_asr_fbank **out,struct glv_memory *memory)
{
 if(!out||!memory)return -EINVAL;*out=NULL;int ret=glv_cancel(memory);if(ret)return ret;
 struct glv_asr_fbank *f=glv_alloc(memory,sizeof(*f));if(!f)return -ENOMEM;f->memory=memory;
 const double pi=3.14159265358979323846;
 for(unsigned i=0;i<WINDOW;i++)f->window[i]=(float)pow(0.5-0.5*cos((2*pi/(WINDOW-1))*i),0.85);
 for(unsigned i=0;i<FFT/2;i++){f->cosine[i]=(float)cos(2*pi*i/FFT);f->sine[i]=(float)-sin(2*pi*i/FFT);}
 float low=mel(20.0f),high=mel(7600.0f),delta=(high-low)/(BINS+1);
 for(unsigned bin=0;bin<BINS;bin++){
   float left=low+bin*delta,center=low+(bin+1)*delta,right=low+(bin+2)*delta;
   unsigned first=0,last=0;bool found=false;
   for(unsigned i=0;i<FFT/2;i++){
     float value=mel(31.25f*i);
     if(value>left&&value<right){
       if(!found){first=i;found=true;}last=i;
       if(i-first>=MEL_WIDTH){glv_free(f);return -EOVERFLOW;}
       f->mel[bin][i-first]=value<=center?(value-left)/(center-left):(right-value)/(right-center);
     }
   }
   if(!found){glv_free(f);return -EINVAL;}
   f->first[bin]=first;f->length[bin]=last-first+1;
 }
 if((ret=glv_cancel(memory))){glv_free(f);return ret;}*out=f;return 0;
}
static void fft(struct glv_asr_fbank *f)
{
 for(unsigned i=0;i<FFT;i++){
   unsigned j=0,v=i;for(unsigned b=0;b<9;b++){j=(j<<1)|(v&1u);v>>=1;}
   if(i<j){float t=f->real[i];f->real[i]=f->real[j];f->real[j]=t;}
   f->imag[i]=0;
 }
 for(unsigned length=2;length<=FFT;length*=2){unsigned half=length/2,step=FFT/length;
   for(unsigned base=0;base<FFT;base+=length)for(unsigned j=0;j<half;j++){
     unsigned a=base+j,b=a+half,index=j*step;float c=f->cosine[index],s=f->sine[index];
     float re=c*f->real[b]-s*f->imag[b],im=s*f->real[b]+c*f->imag[b];
     f->real[b]=f->real[a]-re;f->imag[b]=f->imag[a]-im;f->real[a]+=re;f->imag[a]+=im;
   }
 }
 for(unsigned i=0;i<FFT/2;i++)f->real[i]=f->real[i]*f->real[i]+f->imag[i]*f->imag[i];
}
int glv_asr_fbank_pcm(struct glv_asr_fbank *f,const int16_t *pcm,size_t samples,size_t tail,
                      int (*callback)(void *,const float *,unsigned),void *opaque)
{
 if(!f||(!pcm&&samples)||!callback||samples>INT32_MAX||tail>INT32_MAX-samples)return -EINVAL;
 size_t total=samples+tail;if(!total)return 0;
 unsigned frames=(unsigned)((total+80)/160);
 for(unsigned frame=0;frame<frames;frame++){
   int ret=glv_cancel(f->memory);if(ret)return ret;
   int64_t first=(int64_t)frame*160-120;
   float sum=0;
   for(unsigned i=0;i<WINDOW;i++){
     int64_t index=first+i;
     while(index<0||(uint64_t)index>=total)index=index<0?-index-1:2*(int64_t)total-1-index;
     float v=(uint64_t)index<samples?(float)pcm[index]*(1.0f/32768):0;
     f->real[i]=v;sum+=v;
   }
   float mean=sum/WINDOW;for(unsigned i=0;i<WINDOW;i++)f->real[i]-=mean;
   for(unsigned i=WINDOW-1;i>0;i--)f->real[i]-=0.97f*f->real[i-1];f->real[0]*=1.0f-0.97f;
   for(unsigned i=0;i<WINDOW;i++)f->real[i]*=f->window[i];
   memset(f->real+WINDOW,0,(FFT-WINDOW)*sizeof(float));fft(f);
   for(unsigned bin=0;bin<BINS;bin++){
     float energy=0;for(unsigned i=0;i<f->length[bin];i++)energy+=f->mel[bin][i]*f->real[f->first[bin]+i];
     f->feature[bin]=logf(energy>FLT_EPSILON?energy:FLT_EPSILON);
   }
   ret=callback(opaque,f->feature,BINS);if(ret)return ret;
 }
 return glv_cancel(f->memory);
}
