/* SPDX-License-Identifier: Apache-2.0 */
/* Real service, Zipformer, EdgeIdiot and V2. Only capture/audio/card are fakes. */
#include "glass_local_voice.h"
#include "glass_local_capture.h"
#include "glass_local_platform.h"
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static atomic_bool card=true,admit=true,capturing=false;
static atomic_int capture_error;
static atomic_uint captures,retired,opens,closes,frames;
static size_t budget=GLV_MEMORY_LIMIT;
bool glv_platform_storage_ready(void){return atomic_load(&card);}
bool glass_ai_start_enter(void){return atomic_load(&admit);}
void glass_ai_start_leave(void){}
bool glass_ai_config_active(void){return false;}
int glass_ai_config_begin(unsigned ms){(void)ms;return 0;}
void glass_ai_config_end(void){}
void glv_platform_wake_yield(void){}
int glv_platform_budget(size_t *b,size_t *largest){*b=budget;*largest=8u*1024u*1024u;return budget<4608u*1024u?-ENOMEM:0;}
int glv_platform_audio_open(void **p,bool (*cancel)(void *),void *arg)
{assert(!atomic_load(&capturing)&&captures==retired);if(cancel(arg))return -ECANCELED;assert(opens==closes);opens++;*p=&opens;return 0;}
int glv_platform_audio_write(void *p,const int16_t *v,unsigned n)
{assert(p&&v&&n&&n<=240);frames+=n;return 0;}
int glv_platform_audio_finish(void *p,bool drain){(void)drain;if(p)closes++;return 0;}
int glv_capture_platform(struct glv_memory *m,const struct glv_capture_signals *s,
                         const struct glv_capture_options *o,int16_t **pcm,size_t *samples)
{
 (void)o;*pcm=NULL;*samples=0;assert(!atomic_exchange(&capturing,true));captures++;
 int ret=atomic_load(&capture_error);
 while(!ret&&!s->finish(s->opaque)){
   s->level(s->opaque,16384,1000);ret=glv_cancel(m);if(!ret)usleep(1000);
 }
 if(!ret){
   /* Host fixture is the official manifest-bound sample, never microphone data. */
   FILE *f=fopen(GLV_TEST_PCM,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long length=ftell(f);assert(length>0);rewind(f);
   size_t bytes=(size_t)length;assert(!(bytes&1)&&bytes<16000u*60u*2u);*pcm=glv_alloc(m,bytes);
   if(!*pcm)ret=-ENOMEM;else {assert(fread(*pcm,1,bytes,f)==bytes);*samples=bytes/2;}
   fclose(f);
 }
 atomic_store(&capturing,false);retired++;return ret;
}
static struct glv_snapshot idle(void)
{
 struct glv_snapshot s;for(unsigned i=0;i<60000;i++){glass_local_voice_get(&s,NULL,0);if(!s.busy&&!s.config_pending)return s;usleep(10000);}assert(!"worker timeout");return s;
}
static void at(enum glv_phase phase)
{
 struct glv_snapshot s;for(unsigned i=0;i<30000;i++){glass_local_voice_get(&s,NULL,0);if(s.busy&&s.phase==phase)return;assert(s.busy);usleep(1000);}assert(!"phase timeout");
}
static uint32_t fnv(const char *text){uint32_t v=2166136261u;while(*text)v=(v^(unsigned char)*text++)*16777619u;return v;}
static void flip(void)
{FILE *f=fopen(GLV_ASR_RELEASE_ROOT "/encoder-epoch-99-avg-1.plan","r+b");assert(f);assert(!fseek(f,32,SEEK_SET));int v=fgetc(f);assert(v>=0);assert(!fseek(f,32,SEEK_SET));assert(fputc(v^1,f)>=0);fclose(f);}
int main(int argc,char **argv)
{
 assert(argc==2);assert(!glass_local_voice_start());struct glv_snapshot s=idle();assert(!s.error&&!s.models_ready&&!s.asr_ready&&!s.profile.sha_bytes);
 assert(!glass_local_voice_check(NULL));s=idle();assert(!s.error&&s.models_ready&&s.asr_ready);
 uint32_t id=0,old;char text[1025],reply[4096];
 assert(glass_local_voice_finish_capture(s.request)==-ENOENT);
 atomic_store(&admit,false);assert(glass_local_voice_listen(&id)==-EBUSY);atomic_store(&admit,true);
 assert(!glass_local_voice_listen(&id));old=id;at(GLV_RECORDING);
 assert(glass_local_voice_listen(NULL)==-EBUSY&&glass_local_voice_set_wake(false)==-EBUSY);
 assert(glass_local_voice_finish_capture(id+1)==-ENOENT);glass_local_voice_new();s=idle();
 assert(s.error==-ECANCELED&&s.asr_ready&&!atomic_load(&capturing)&&captures==retired&&!opens);
 assert(!glass_local_voice_get_transcript(id,text,sizeof(text))&&!text[0]);
 assert(!glass_local_voice_listen(&id));assert(id!=old);glass_local_voice_stop(old);at(GLV_RECORDING);
 assert(!glass_local_voice_finish_capture(id));at(GLV_TRANSCRIBING);glass_local_voice_cancel();s=idle();
 assert(s.error==-ECANCELED&&s.models_ready&&s.asr_ready&&!opens);
 assert(glass_local_voice_finish_capture(id)==-ENOENT);
 int errors[]={-ENODATA,-E2BIG,-ENOMEM};
 for(unsigned i=0;i<3;i++){capture_error=errors[i];assert(!glass_local_voice_listen(&id));s=idle();assert(s.error==errors[i]&&s.asr_ready&&s.models_ready&&!opens);}
 capture_error=0;budget=1024;unsigned count=captures;assert(!glass_local_voice_listen(&id));s=idle();assert(s.error==-ENOMEM&&captures==count);budget=GLV_MEMORY_LIMIT;
 flip();count=captures;assert(!glass_local_voice_listen(&id));s=idle();
 assert(s.error==-EBADMSG&&s.models_ready&&!s.asr_ready&&!opens&&captures==count);flip();
 assert(!glass_local_voice_check(&id));s=idle();assert(!s.error&&s.models_ready&&s.asr_ready);
 assert(!glass_local_voice_listen(&id));at(GLV_RECORDING);atomic_store(&card,false);s=idle();
 assert(s.error==-ENODEV&&!s.models_ready&&!s.asr_ready&&!atomic_load(&capturing)&&captures==retired);
 atomic_store(&card,true);assert(!glass_local_voice_check(&id));s=idle();assert(!s.error&&s.models_ready&&s.asr_ready);
 puts("LOCAL_MICROPHONE_LIFECYCLE_OK cancellation/finish/stale/admission/card/corrupt/OOM/no-cloud");fflush(stdout);
 old=id;assert(!glass_local_voice_listen(&id));at(GLV_RECORDING);assert(!glass_local_voice_finish_capture(id));s=idle();
 printf("FULL_CHAIN_RESULT error=%d model_error=%d asr_error=%d ready=%u asr_ready=%u phase=%u peak=%zu asr_tokens=%u tokens=%u capture_ms=%u elapsed_ms=%u\n",s.error,s.model_error,s.asr_error,s.models_ready,s.asr_ready,s.phase,s.memory_peak,s.asr_tokens,s.tokens,s.capture_ms,s.elapsed_ms);fflush(stdout);
 if(s.error){glass_local_voice_get(NULL,reply,sizeof(reply));printf("HOST_PUBLIC_FIXTURE_REPLY=%s\n",reply);fflush(stdout);}
 assert(!s.error&&s.models_ready&&s.asr_ready&&s.asr_tokens&&s.asr_first_token_ms&&s.asr_elapsed_ms&&s.tokens&&!s.first_audio_ms);
 assert(s.memory_peak<=GLV_MEMORY_LIMIT&&!frames&&!opens&&!closes&&captures==retired);
 assert(!glass_local_voice_get_transcript(old,text,sizeof(text))&&!text[0]);
 assert(glass_local_voice_get_transcript(id,text,sizeof(text))&&fnv(text)==strtoul(argv[1],NULL,16));
 glass_local_voice_get(NULL,reply,sizeof(reply));assert(reply[0]);
 printf("LOCAL_MICROPHONE_REAL_CHAIN_OK asr_fnv=%08x asr_tokens=%u answer_tokens=%u peak=%zu capture_ms=%u asr_ms=%u first_token_ms=%u first_audio_ms=%u total_ms=%u frames=%u\n",fnv(text),s.asr_tokens,s.tokens,s.memory_peak,s.capture_ms,s.asr_elapsed_ms,s.first_token_ms,s.first_audio_ms,s.elapsed_ms,frames);fflush(stdout);
 assert(!glass_local_voice_new());assert(!glass_local_voice_get_transcript(id,text,sizeof(text))&&!text[0]);
 assert(access(GLV_SESSION_ROOT,F_OK));return 0;
}
