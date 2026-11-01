/* SPDX-License-Identifier: Apache-2.0 */
/* Production worker + real base SD package; deterministic ASR check faults. */
#include "glass_local_voice.h"
#include "glass_local_capture.h"
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static atomic_int asr_fault;
static atomic_bool holding,entered,card=true,admit=true;
bool glv_platform_storage_ready(void){return atomic_load(&card);}
bool glass_ai_start_enter(void){return atomic_load(&admit);}
void glass_ai_start_leave(void){}
bool glass_ai_config_active(void){return false;}
int glass_ai_config_begin(unsigned t){(void)t;return 0;}
void glass_ai_config_end(void){}
void glv_platform_wake_yield(void){}
int glv_platform_budget(size_t *b,size_t *largest){*b=GLV_MEMORY_LIMIT;*largest=8u*1024u*1024u;return 0;}
int glv_platform_audio_open(void **p,bool (*cancel)(void *),void *arg){(void)p;(void)cancel;(void)arg;assert(!"resource checks cannot play audio");return -ENOSYS;}
int glv_platform_audio_write(void *p,const int16_t *v,unsigned n){(void)p;(void)v;(void)n;assert(0);return -ENOSYS;}
int glv_platform_audio_finish(void *p,bool drain){(void)drain;assert(!p);return 0;}
int glv_capture_platform(struct glv_memory *m,const struct glv_capture_signals *s,const struct glv_capture_options *o,int16_t **p,size_t *n)
{(void)m;(void)s;(void)o;(void)p;(void)n;assert(!"resource checks cannot capture audio");return -ENOSYS;}
int glv_asr_assets_check(struct glv_memory *m)
{
 atomic_store(&entered,true);
 while(atomic_load(&holding)){int ret=glv_cancel(m);if(ret)return ret;usleep(1000);}
 return atomic_load(&asr_fault);
}
int glv_asr_transcribe_pcm(const int16_t *p,size_t n,struct glv_memory *m,char *s,size_t cap,int (*token)(void *,unsigned,unsigned),void *opaque,void (*progress)(void *,unsigned))
{(void)p;(void)n;(void)m;(void)s;(void)cap;(void)token;(void)opaque;(void)progress;assert(0);return -ENOSYS;}
int glv_asr_test_file(unsigned clip,struct glv_memory *m,char *s,size_t cap,int (*token)(void *,unsigned,unsigned),void *opaque,void (*progress)(void *,unsigned))
{(void)clip;return glv_asr_transcribe_pcm(NULL,0,m,s,cap,token,opaque,progress);}
static struct glv_snapshot idle(void)
{struct glv_snapshot s;for(unsigned i=0;i<10000;i++){glass_local_voice_get(&s,NULL,0);if(!s.busy&&!s.config_pending)return s;usleep(1000);}assert(!"worker timeout");return s;}
static void checking(void)
{for(unsigned i=0;i<10000;i++){if(atomic_load(&entered))return;usleep(1000);}assert(!"ASR check not entered");}
static uint32_t check(void)
{uint32_t id=0;atomic_store(&entered,false);assert(!glass_local_voice_check(&id));return id;}
int main(void)
{
 assert(!glass_local_voice_start());struct glv_snapshot s=idle();assert(!s.error&&!s.models_ready&&!s.asr_ready&&!s.profile.sha_bytes);
 check();s=idle();assert(!s.error&&s.models_ready&&s.asr_ready);
 uint32_t old=check();s=idle();assert(s.asr_ready);
 atomic_store(&holding,true);uint32_t id=check();checking();glass_local_voice_stop(old);
 glass_local_voice_get(&s,NULL,0);assert(s.request==id&&s.busy&&s.phase==GLV_CHECKING_ASR);
 glass_local_voice_stop(id);s=idle();
 printf("ASR_CHECK_CANCEL_RESULT error=%d asr_error=%d ready=%u asr=%u\n",s.error,s.asr_error,s.models_ready,s.asr_ready);fflush(stdout);
 assert(s.error==-ECANCELED&&s.asr_error==-ECANCELED&&s.models_ready&&!s.asr_ready);
 atomic_store(&holding,false);
 int isolated[]={-ENOENT,-EBADMSG,-ENOMEM};
 for(unsigned i=0;i<3;i++){
  atomic_store(&asr_fault,isolated[i]);check();s=idle();assert(!s.error&&s.models_ready&&!s.asr_ready&&s.asr_error==isolated[i]);
  assert(!glass_local_voice_listen(NULL));s=idle();assert(s.error==isolated[i]&&!s.asr_ready);
 }
 int media[]={-EIO,-ENODEV};
 for(unsigned i=0;i<2;i++){
  atomic_store(&asr_fault,media[i]);check();s=idle();assert(s.error==media[i]&&!s.models_ready&&!s.asr_ready);
  atomic_store(&asr_fault,0);check();s=idle();assert(!s.error&&s.models_ready&&s.asr_ready);
 }
 atomic_store(&holding,true);check();checking();atomic_store(&card,false);s=idle();assert(s.error==-ENODEV&&!s.models_ready&&!s.asr_ready);
 atomic_store(&card,true);atomic_store(&holding,false);check();s=idle();assert(!s.error&&s.models_ready&&s.asr_ready);
 atomic_store(&admit,false);assert(glass_local_voice_check(NULL)==-EBUSY);atomic_store(&admit,true);
 puts("LOCAL_RESOURCE_CHECK_OK cancel/stale/ASR-only-fault/media-fault/recovery/admission/no-capture/no-audio");return 0;
}
