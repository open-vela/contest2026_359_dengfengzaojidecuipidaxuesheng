/* SPDX-License-Identifier: Apache-2.0 */
/* SDK MFCC and generated EON operators are unchanged. Only immutable trained
 * arrays are bound to authenticated SD bytes instead of compiled constants. */
#include "glass_local_wake.h"
#include "glass_custom_wake.h"
#include "glass_local_assets.h"
#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <time.h>
#ifdef GLV_WAKE_TEST
#include <cassert>
#endif
extern "C" int glv_eon_bind(const unsigned char *,size_t);
extern "C" bool glv_platform_storage_ready(void);
static pthread_mutex_t model_lock=PTHREAD_MUTEX_INITIALIZER;
static glv_memory *current_memory;
static bool owned;
union ei_allocation {struct {bool tracked;} info;uint64_t align[2];};
extern "C" void *ei_malloc(size_t n)
{
  if(n>SIZE_MAX-sizeof(ei_allocation))return nullptr;
  auto *p=(ei_allocation *)(current_memory?glv_alloc(current_memory,n+sizeof(ei_allocation)):calloc(1,n+sizeof(ei_allocation)));
  if(!p)return nullptr;p->info.tracked=current_memory!=nullptr;return p+1;
}
extern "C" void *ei_calloc(size_t n,size_t z)
{if(z&&n>SIZE_MAX/z)return nullptr;void *p=ei_malloc(n*z);if(p)memset(p,0,n*z);return p;}
extern "C" void ei_free(void *ptr)
{if(ptr){auto *p=(ei_allocation *)ptr-1;if(p->info.tracked)glv_free(p);else free(p);}}
extern "C" void ei_printf(const char *,...){}
extern "C" void ei_printf_float(float){}
extern "C" void DebugLog(const char *){}
extern "C" uint64_t ei_read_timer_us()
{struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000+t.tv_nsec/1000;}
extern "C" uint64_t ei_read_timer_ms(){return ei_read_timer_us()/1000;}
extern "C" EI_IMPULSE_ERROR ei_run_impulse_check_canceled()
{return current_memory&&glv_cancel(current_memory)?EI_IMPULSE_CANCELED:EI_IMPULSE_OK;}
extern "C" EI_IMPULSE_ERROR ei_sleep(int32_t){return ei_run_impulse_check_canceled();}
struct glv_wake {
  glv_memory memory;unsigned char *weights,*weights_storage;int16_t *ring;
  unsigned at,filled,since,evaluations,cooldown;float score;
};
#ifdef GLV_WAKE_TEST
static unsigned test_skew;
extern "C" void glv_wake_test_skew(unsigned bytes){assert(bytes<=15);test_skew=bytes;}
#endif
extern "C" int glv_wake_create(glv_wake **out)
{
  if(!out)return -EINVAL;*out=nullptr;
  pthread_mutex_lock(&model_lock);bool busy=owned;if(!busy)owned=true;pthread_mutex_unlock(&model_lock);
  if(busy)return -EBUSY;
  auto *s=(glv_wake *)calloc(1,sizeof(glv_wake));int ret=-ENOMEM;
  if(!s)goto fail;s->memory.limit=1024*1024;
  if((ret=glv_asset_check(GLV_RELEASE_ROOT,"wake/weights.bin",&s->memory)))goto fail;
  {
    FILE *f=nullptr;size_t bytes=0;ret=glv_open_asset(GLV_RELEASE_ROOT "/wake/weights.bin",&f,&bytes);
    if(ret)goto fail;
    if(bytes!=7776){fclose(f);ret=-EBADMSG;goto fail;}
    unsigned skew=0;
#ifdef GLV_WAKE_TEST
    skew=test_skew;
#endif
    /* NuttX malloc need not satisfy the generated EON model's 16-byte
     * constant-buffer alignment. Retain the tracked base for every free. */
    s->weights_storage=(unsigned char *)glv_alloc(&s->memory,bytes+15+skew);
    if(s->weights_storage)s->weights=(unsigned char *)(((uintptr_t)s->weights_storage+skew+15)&~(uintptr_t)15);
    s->ring=(int16_t *)glv_alloc(&s->memory,32000*2);
    if(!s->weights||!s->ring)ret=-ENOMEM;else ret=glv_read_exact(f,s->weights,bytes);fclose(f);if(ret)goto fail;
    if(glv_eon_bind(s->weights,bytes)){ret=-EBADMSG;goto fail;}
#ifndef GLV_WAKE_TEST
    printf("[local-wake] weights alignment=%u padding=%u\n",(unsigned)((uintptr_t)s->weights%16),(unsigned)(s->weights-s->weights_storage));
#endif
  }
  current_memory=&s->memory;run_classifier_init();
  if(!ei_default_impulse.post_processing_state){ret=-ENOMEM;run_classifier_deinit();ei_default_impulse.state.reset();current_memory=nullptr;goto fail;}
  *out=s;return 0;
fail:
#ifndef GLV_WAKE_TEST
  printf("[local-wake] create error=%d\n",ret);
#endif
  if(s){glv_free(s->weights_storage);glv_free(s->ring);free(s);}
  pthread_mutex_lock(&model_lock);owned=false;pthread_mutex_unlock(&model_lock);return ret;
}
extern "C" void glv_wake_reset(glv_wake *s)
{if(s){s->at=s->filled=s->since=s->cooldown=0;s->score=0;
  run_classifier_deinit();ei_default_impulse.state.reset();run_classifier_init();}}
extern "C" int glv_wake_feed(glv_wake *s,const int16_t *pcm,size_t n)
{
  if(!s||!pcm||n>4096)return -EINVAL;int event=0;
  if(!glv_platform_storage_ready())return -ENODEV;
  for(size_t i=0;i<n;i++){
    s->ring[s->at++]=pcm[i];if(s->at==32000)s->at=0;
    if(s->filled<32000)s->filled++;s->since++;if(s->cooldown)s->cooldown--;
    if(s->filled<32000||s->since<8000)continue;s->since=0;
    signal_t signal;signal.total_length=32000;
    signal.get_data=[s](size_t offset,size_t count,float *out){
      if(offset>32000||count>32000-offset)return -1;
      for(size_t j=0;j<count;j++)out[j]=s->ring[(s->at+offset+j)%32000];return 0;};
    ei_impulse_result_t result={};auto ret=run_classifier(&signal,&result,false);if(ret)return -EIO;
    s->evaluations++;s->score=result.classification[2].value;
    if(!s->cooldown&&s->score>=static_cast<float>(EI_CLASSIFIER_THRESHOLD)*GLASS_WAKE_ACCEPTANCE_FACTOR){event=1;s->cooldown=16000;}
  }return event;
}
extern "C" void glv_wake_destroy(glv_wake *s)
{
  if(!s)return;
  run_classifier_deinit();ei_default_impulse.state.reset();ei_dsp_clear_continuous_audio_state();
  /* The generated inference releases arena/operator allocations after each
   * call; binding NULL is never used to execute a model. */
  glv_free(s->weights_storage);glv_free(s->ring);
#ifdef GLV_WAKE_TEST
  assert(s->memory.used==0);
#endif
  current_memory=nullptr;free(s);
  pthread_mutex_lock(&model_lock);owned=false;pthread_mutex_unlock(&model_lock);
}
extern "C" unsigned glv_wake_evaluations(const glv_wake *s){return s?s->evaluations:0;}
extern "C" float glv_wake_score(const glv_wake *s){return s?s->score:0;}
