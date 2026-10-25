/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_wake_features.h"
#include <errno.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>

#define WAKE_PI 3.14159265358979323846f
#define WAKE_MELS 20
static float wake_rms(const int16_t *pcm,size_t n)
{return glass_wake_pcm_rms(pcm,n);}
int glass_wake_extract(const int16_t *pcm,size_t samples,float *out)
{
  if(!pcm||!out||samples<400||samples>GLASS_WAKE_MAX_SAMPLES)return -EINVAL;
  float peak=0;
  for(size_t i=0;i+320<=samples;i+=320){float r=wake_rms(pcm+i,320);if(r>peak)peak=r;}
  if(peak<60)return -ENODATA;
  /* The caller includes quiet margins. Estimate their noise floor so added
   * room noise is not stretched into the word's temporal representation. */
  float noise=fminf(wake_rms(pcm,320),wake_rms(pcm+samples-320,320));
  float gate=fmaxf(60,fmaxf(peak*.12f,fminf(peak*.45f,noise*2.5f)));
  size_t start=0,end=samples;
  while(start+320<end&&wake_rms(pcm+start,320)<gate)start+=320;
  while(end>start+400&&wake_rms(pcm+end-320,320)<gate)end-=320;
  if(end-start<400)return -ENODATA;
  float window[400],wr[256],wi[256],dct[GLASS_WAKE_COEFS][WAKE_MELS];
  float cep[398][GLASS_WAKE_COEFS],real[512],imag[512],power[257];
  int bins[WAKE_MELS+2];
  for(unsigned i=0;i<400;i++)window[i]=.5f-.5f*cosf(2*WAKE_PI*i/399);
  for(unsigned i=0;i<256;i++){wr[i]=cosf(2*WAKE_PI*i/512);wi[i]=-sinf(2*WAKE_PI*i/512);}
  for(unsigned k=0;k<GLASS_WAKE_COEFS;k++)for(unsigned m=0;m<WAKE_MELS;m++)
    dct[k][m]=cosf(WAKE_PI*(k+1)*(m+.5f)/WAKE_MELS);
  float lo=2595*log10f(1+80.0f/700),hi=2595*log10f(1+7600.0f/700);
  for(unsigned m=0;m<WAKE_MELS+2;m++){
    float hz=700*(powf(10,(lo+(hi-lo)*m/(WAKE_MELS+1))/2595)-1);
    bins[m]=(int)floorf(513*hz/GLASS_WAKE_RATE);
  }
  unsigned frames=0;
  for(size_t offset=start;offset+400<=end;offset+=160){
    memset(real,0,sizeof(real));memset(imag,0,sizeof(imag));
    for(unsigned i=0;i<400;i++){
      float previous=offset+i?pcm[offset+i-1]:0;
      real[i]=(pcm[offset+i]-.97f*previous)*window[i]/32768;
    }
    for(unsigned i=1,j=0;i<512;i++){
      unsigned bit=256;for(;j&bit;bit>>=1)j^=bit;j^=bit;
      if(i<j){float t=real[i];real[i]=real[j];real[j]=t;}
    }
    for(unsigned size=2;size<=512;size*=2)for(unsigned base=0;base<512;base+=size)
      for(unsigned k=0;k<size/2;k++){
        unsigned a=base+k,b=a+size/2,t=k*512/size;
        float r=wr[t]*real[b]-wi[t]*imag[b],im=wr[t]*imag[b]+wi[t]*real[b];
        real[b]=real[a]-r;imag[b]=imag[a]-im;real[a]+=r;imag[a]+=im;
      }
    for(unsigned i=0;i<=256;i++)power[i]=real[i]*real[i]+imag[i]*imag[i];
    float mel[WAKE_MELS];
    for(unsigned m=0;m<WAKE_MELS;m++){
      double sum=0;
      for(int i=bins[m];i<bins[m+1];i++)sum+=power[i]*(i-bins[m])/(bins[m+1]-bins[m]);
      for(int i=bins[m+1];i<bins[m+2];i++)sum+=power[i]*(bins[m+2]-i)/(bins[m+2]-bins[m+1]);
      mel[m]=logf(fmaxf(sum,1e-10f));
    }
    for(unsigned k=0;k<GLASS_WAKE_COEFS;k++){
      float sum=0;for(unsigned m=0;m<WAKE_MELS;m++)sum+=mel[m]*dct[k][m];cep[frames][k]=sum;
    }
    frames++;
  }
  for(unsigned k=0;k<GLASS_WAKE_COEFS;k++){
    double mean=0,var=0;for(unsigned i=0;i<frames;i++)mean+=cep[i][k];mean/=frames;
    for(unsigned i=0;i<frames;i++){double d=cep[i][k]-mean;var+=d*d;}
    float scale=fmaxf(sqrtf(var/frames),1);
    for(unsigned i=0;i<frames;i++)cep[i][k]=fmaxf(-4,fminf(4,(cep[i][k]-mean)/scale));
    for(unsigned t=0;t<GLASS_WAKE_STEPS;t++){
      float pos=(float)t*(frames-1)/(GLASS_WAKE_STEPS-1);unsigned i=(unsigned)pos,j=i+1<frames?i+1:i;
      out[t*GLASS_WAKE_COEFS+k]=cep[i][k]+(pos-i)*(cep[j][k]-cep[i][k]);
    }
  }
  return 0;
}
static float glass_wake_dtw_bounded(const float *a,const float *b,float cutoff)
{
  float previous[33],current[33];
  for(unsigned i=0;i<33;i++)previous[i]=1e20f;previous[0]=0;
  /* Outside-band cells use the original finite sentinel. Only prune below
   * that sentinel, where a later path cannot re-enter with a lower cost. */
  float limit=cutoff<1e20f/32?cutoff*32:1e20f;
  bool bounded=cutoff>=0&&cutoff<1e20f/32;
  for(int i=1;i<=32;i++){
    for(unsigned j=0;j<33;j++)current[j]=1e20f;
    float row_min=1e20f;
    int first=i>8?i-8:1,last=i+8<32?i+8:32;
    for(int j=first;j<=last;j++){
      float cost=0;
      for(unsigned k=0;k<8;k++){float d=a[(i-1)*13+k]-b[(j-1)*13+k];cost+=d*d*(k<4?2:1);}
      current[j]=cost/12+fminf(previous[j-1],fminf(previous[j],current[j-1]));
      if(current[j]<row_min)row_min=current[j];
    }
    /* Every remaining path crosses this row and adds nonnegative costs.
     * Equal distances cannot replace the group's third neighbor either. */
    if(bounded&&row_min>=limit)return cutoff;
    memcpy(previous,current,sizeof(previous));
  }
  return previous[32]/32;
}

float glass_wake_dtw(const float *a,const float *b)
{return glass_wake_dtw_bounded(a,b,1e20f);}

float glass_wake_ratio_distance(const float *templates,const float *features,float *positive_distance)
{
  float best[2][3]={{1e20f,1e20f,1e20f},{1e20f,1e20f,1e20f}};
  for(unsigned i=0;i<GLASS_WAKE_TEMPLATES;i++){
    unsigned group=i<GLASS_WAKE_POSITIVE?0:1;
    float d=glass_wake_dtw_bounded(templates+i*GLASS_WAKE_INPUTS,features,best[group][2]);
    for(unsigned j=0;j<3;j++)if(d<best[group][j]){
      for(unsigned k=2;k>j;k--)best[group][k]=best[group][k-1];best[group][j]=d;break;
    }
  }
  float p=best[0][0]+best[0][1]+best[0][2],n=best[1][0]+best[1][1]+best[1][2];
  if(positive_distance)*positive_distance=p/3;
  return p/fmaxf(n,1e-9f);
}

float glass_wake_ratio(const float *templates,const float *features)
{return glass_wake_ratio_distance(templates,features,NULL);}
