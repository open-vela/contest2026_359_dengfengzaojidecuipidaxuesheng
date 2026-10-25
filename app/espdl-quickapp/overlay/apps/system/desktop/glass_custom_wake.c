/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_custom_wake.h"
#include "glass_wake_features.h"
#include "glass_wake_model.h"
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
extern void qpk_dl_sha256(const void *,size_t,uint8_t[32]);
_Static_assert(GLASS_WAKE_MODEL_BYTES==32+GLASS_WAKE_WEIGHTS*4,"Wake model shape");

struct glass_custom_wake {
  uint8_t *model;
  float *weights,threshold,noise,score,initial_floor,initial_peak;
  int16_t pcm[GLASS_WAKE_MAX_SAMPLES],block[320],pre[2560];
  unsigned samples,used,pre_at,pre_count,above,below,cooldown,evaluations,warmup,initial_clipped;
  bool speaking;
};
static uint32_t wake_u32(const uint8_t *p)
{return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
void glass_custom_wake_destroy(struct glass_custom_wake *s)
{if(s){free(s->model);free(s);}}
int glass_custom_wake_create(const char *root,struct glass_custom_wake **out)
{
  if(!root||!out)return -EINVAL;*out=NULL;
  char path[256];if(snprintf(path,sizeof(path),"%s/custom/nihao_openvela.gkw",root)>=(int)sizeof(path))return -ENAMETOOLONG;
  FILE *file=fopen(path,"rb");if(!file)return -errno;
  struct stat st;int ret=-EBADMSG;
  uint8_t *data=NULL;struct glass_custom_wake *s=NULL;
  if(fstat(fileno(file),&st)||st.st_size!=GLASS_WAKE_MODEL_BYTES)goto done;
  data=malloc(GLASS_WAKE_MODEL_BYTES);if(!data){ret=-ENOMEM;goto done;}
  if(fread(data,1,GLASS_WAKE_MODEL_BYTES,file)!=GLASS_WAKE_MODEL_BYTES)goto done;
  uint8_t digest[32];qpk_dl_sha256(data,GLASS_WAKE_MODEL_BYTES,digest);
  if(memcmp(digest,glass_wake_model_sha,32)||memcmp(data,"GKW3",4)||
     wake_u32(data+4)!=GLASS_WAKE_INPUTS||wake_u32(data+8)!=GLASS_WAKE_POSITIVE||
     wake_u32(data+12)!=GLASS_WAKE_NEGATIVE||wake_u32(data+16)!=16000||
     wake_u32(data+24)!=GLASS_WAKE_MAX_SAMPLES||wake_u32(data+28)!=1)goto done;
  s=calloc(1,sizeof(*s));if(!s){ret=-ENOMEM;goto done;}
  memcpy(&s->threshold,data+20,4);
  if(!isfinite(s->threshold)||s->threshold<.5f||s->threshold>=1)goto done;
  /* The authenticated file is already malloc-aligned, and its 32-byte header
   * preserves float alignment. Own that allocation rather than copy a second
   * 329472-byte weight array during initialization. */
  _Static_assert(sizeof(float)==4&&32%_Alignof(float)==0,"Wake weight alignment");
  s->model=data;s->weights=(float *)(data+32);data=NULL;
  for(unsigned i=0;i<GLASS_WAKE_WEIGHTS;i++)if(!isfinite(s->weights[i]))goto done;
  s->noise=50;s->initial_floor=1e20f;s->warmup=4;*out=s;s=NULL;ret=0;
done:
  fclose(file);free(data);glass_custom_wake_destroy(s);return ret;
}
void glass_custom_wake_reset(struct glass_custom_wake *s)
{
  if(!s)return;
  s->samples=s->used=s->pre_at=s->pre_count=s->above=s->below=0;s->speaking=false;
}
static int wake_block(struct glass_custom_wake *s)
{
  float rms=glass_wake_pcm_rms(s->block,320);int event=0;
  if(s->cooldown){s->cooldown=s->cooldown>320?s->cooldown-320:0;return 0;}
  if(!s->speaking){
    /* pre_at advances by whole 320-sample blocks; 2560 is exactly 8 blocks. */
    memcpy(s->pre+s->pre_at,s->block,sizeof(s->block));
    s->pre_at+=320;if(s->pre_at==2560)s->pre_at=0;
    s->pre_count=s->pre_count+320>2560?2560:s->pre_count+320;
    /* Estimate the actual initial bed before arming. Otherwise a constant
     * noise bed above 150 starts a false utterance while noise stays at 50,
     * and its endpoint can never fall below the fixed 110 threshold. The
     * 80 ms estimate plus attack fits inside the existing 160 ms preroll. */
    if(s->warmup){
      s->initial_floor=fminf(s->initial_floor,rms);
      s->initial_peak=fmaxf(s->initial_peak,rms);
      for(unsigned i=0;i<320;i++)
        if(s->block[i]>=32000||s->block[i]<=-32000)s->initial_clipped++;
      if(--s->warmup)return 0;
      /* A user can already be speaking in the first capture block. Bound
       * that initial estimate; subsequent idle blocks track the actual bed. */
      s->noise=fmaxf(10,fminf(500,s->initial_floor));
      /* A stationary bed above the capped floor's 2.5x attack level would
       * start a false, endless utterance. Only this clearly loud/stable
       * startup uses its measured floor. Variable onset (a user already
       * speaking), quiet/far speech and the classifier keep their policy. */
      if(!s->initial_clipped&&s->initial_floor>1250&&s->initial_peak<s->initial_floor*1.3f)
        s->noise=s->initial_floor;
    }
    if(rms>fmaxf(90,s->noise*2.5f))s->above++;else{s->above=0;s->noise=.98f*s->noise+.02f*rms;}
    if(s->above>=2){
      unsigned first=(s->pre_at+2560-s->pre_count)%2560;
      unsigned tail=2560-first;if(tail>s->pre_count)tail=s->pre_count;
      memcpy(s->pcm,s->pre+first,tail*sizeof(int16_t));
      memcpy(s->pcm+tail,s->pre,(s->pre_count-tail)*sizeof(int16_t));
      s->samples=s->pre_count;s->speaking=true;s->below=0;
    }
    return 0;
  }
  memcpy(s->pcm+s->samples,s->block,320*sizeof(int16_t));s->samples+=320;
  if(rms<fmaxf(110,s->noise*1.8f))s->below++;else s->below=0;
  /* Quiet/far speech enters the same authenticated classifier. A strong
   * match may finish after 320ms; uncertain or interrupted words retain the
   * original 640ms endpoint and unchanged final acceptance threshold. */
  bool final=s->below>=32||s->samples>=GLASS_WAKE_MAX_SAMPLES;
  bool early=s->below==16&&s->samples>=8000;
  if(final||early){
    if(s->samples>=8000){
      float feature[GLASS_WAKE_INPUTS];int ret=glass_wake_extract(s->pcm,s->samples,feature);
      if(!ret){
        float distance;float ratio=glass_wake_ratio_distance(s->weights,feature,&distance);
        s->score=1/(1+expf(fmaxf(-30,fminf(30,20*(ratio-s->threshold)))));
        s->evaluations++;
        /* A relative winner can still be far from every wake template.
         * Keep the model threshold/endpoint policy, but reject that OOD match.
         * Both distances come from the same existing 198-template DTW pass. */
        bool relative=ratio<s->threshold-(early&&!final?.03f:0);
        event=relative&&distance<=GLASS_WAKE_MAX_POSITIVE_DISTANCE;
#ifdef __NuttX__
        if(relative&&!event)printf("[wake-filter] rejected=distance ratio=%.4f distance=%.4f limit=%.2f samples=%u\n",
          (double)ratio,(double)distance,(double)GLASS_WAKE_MAX_POSITIVE_DISTANCE,s->samples);
#endif
      }
      else if(ret!=-ENODATA)return ret;
    }
    if(final||event){s->speaking=false;s->samples=s->above=s->below=s->pre_count=0;}
    if(event)s->cooldown=16000;
  }
  return event;
}
int glass_custom_wake_feed(struct glass_custom_wake *s,const int16_t *pcm,size_t samples)
{
  if(!s||!pcm||samples>4096)return -EINVAL;int event=0;
  while(samples){
    size_t n=320-s->used;if(n>samples)n=samples;
    memcpy(s->block+s->used,pcm,n*sizeof(int16_t));s->used+=n;pcm+=n;samples-=n;
    if(s->used==320){s->used=0;int ret=wake_block(s);if(ret<0)return ret;if(ret)event=1;}
  }
  return event;
}
unsigned glass_custom_wake_evaluations(const struct glass_custom_wake *s){return s?s->evaluations:0;}
float glass_custom_wake_score(const struct glass_custom_wake *s){return s?s->score:0;}
