/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_local_voice.h"
#include "glass_local_model.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
static size_t budget=GLV_MEMORY_LIMIT;static atomic_bool card=true,admit=true;
bool glv_platform_storage_ready(void){return atomic_load(&card);}
bool glass_ai_start_enter(void){return atomic_load(&admit);}
void glass_ai_start_leave(void){}
bool glass_ai_config_active(void){return false;}
int glass_ai_config_begin(unsigned timeout){(void)timeout;return 0;}
void glass_ai_config_end(void){}
void glv_platform_wake_yield(void){}
int glv_platform_budget(size_t *b,size_t *largest){*b=budget;*largest=8u*1024u*1024u;return budget<4608u*1024u?-ENOMEM:0;}
int glv_platform_audio_open(void **p,bool (*cancel)(void *),void *arg){(void)p;(void)cancel;(void)arg;assert(!"ASR file gate must not reserve playback");return -ENOSYS;}
int glv_platform_audio_write(void *p,const int16_t *v,unsigned n){(void)p;(void)v;(void)n;assert(0);return -ENOSYS;}
int glv_platform_audio_finish(void *p,bool drain){(void)drain;assert(!p);return 0;}
static struct glv_snapshot idle(void)
{
 struct glv_snapshot s;for(unsigned i=0;i<30000;i++){glass_local_voice_get(&s,NULL,0);if(!s.busy&&!s.config_pending)return s;usleep(10000);}assert(!"worker timeout");return s;
}
static void transcribing(void)
{
 struct glv_snapshot s;for(unsigned i=0;i<30000;i++){glass_local_voice_get(&s,NULL,0);if(s.busy&&s.phase==GLV_TRANSCRIBING)return;assert(s.busy);usleep(1000);}assert(!"ASR phase timeout");
}
static uint32_t fnv(const char *s){uint32_t v=2166136261u;while(*s)v=(v^(unsigned char)*s++)*16777619u;return v;}
static void flip(void)
{
 FILE *f=fopen(GLV_ASR_RELEASE_ROOT "/encoder-epoch-99-avg-1.plan","r+b");assert(f);assert(!fseek(f,32,SEEK_SET));int v=fgetc(f);assert(v>=0);assert(!fseek(f,32,SEEK_SET));assert(fputc(v^1,f)>=0);fclose(f);
}
int main(int argc,char **argv)
{
 assert(argc==3);assert(!glass_local_voice_start());struct glv_snapshot s=idle();assert(!s.error&&!s.models_ready&&!s.asr_ready&&!s.profile.sha_bytes);
 assert(!glass_local_voice_check(NULL));s=idle();assert(!s.error&&s.models_ready&&!s.asr_ready);
 uint32_t id=0,old;char reply[4096];assert(glass_local_voice_asr_test(2,&id)==-EINVAL);
 atomic_store(&admit,false);assert(glass_local_voice_asr_test(0,&id)==-EBUSY);atomic_store(&admit,true);
 assert(!glass_local_voice_asr_test(0,&id));old=id;
 assert(glass_local_voice_asr_test(1,NULL)==-EBUSY&&glass_local_voice_set_wake(false)==-EBUSY);
 transcribing();glass_local_voice_new();s=idle();glass_local_voice_get(NULL,reply,sizeof(reply));
 assert(s.error==-ECANCELED&&!reply[0]&&s.models_ready&&!s.asr_ready);
 for(unsigned sample=0;sample<2;sample++){
   assert(!glass_local_voice_asr_test(sample,&id)&&id!=old);glass_local_voice_stop(old);
   s=idle();glass_local_voice_get(NULL,reply,sizeof(reply));
   assert(!s.error&&s.models_ready&&!s.asr_ready&&s.tokens&&s.first_token_ms&&s.memory_peak<=GLV_MEMORY_LIMIT);
   assert(fnv(reply)==(uint32_t)strtoul(argv[sample+1],NULL,16));old=id;
   printf("ASR_WORKER_SAMPLE_OK sample=%u bytes=%zu fnv=%08x peak=%zu\n",sample,strlen(reply),fnv(reply),s.memory_peak);fflush(stdout);
 }
 budget=1024;assert(!glass_local_voice_asr_test(0,&id));s=idle();assert(s.error==-ENOMEM&&s.models_ready&&!s.asr_ready);budget=GLV_MEMORY_LIMIT;
 flip();assert(!glass_local_voice_asr_test(0,&id));s=idle();assert(s.error==-EBADMSG&&s.models_ready&&!s.asr_ready);flip();
 assert(!glass_local_voice_asr_test(0,&id));transcribing();atomic_store(&card,false);s=idle();
 assert(s.error==-ENODEV&&!s.models_ready&&!s.asr_ready);atomic_store(&card,true);
 assert(!glass_local_voice_check(&id));s=idle();assert(!s.error&&s.models_ready&&!s.asr_ready);
 assert(!glass_local_voice_new());glass_local_voice_get(NULL,reply,sizeof(reply));assert(!reply[0]);
 puts("ASR_WORKER_LIFECYCLE_OK admission/request/cancel/new/stale/memory/manifest/card/no-audio/no-cloud");return 0;
}
