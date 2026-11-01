/* SPDX-License-Identifier: Apache-2.0 */
/* Real worker/base package; fake ASR/LLM permit deterministic late results. */
#include "glass_local_voice.h"
#include "glass_local_capture.h"
#include "glass_local_llm.h"
#include "glass_local_tts.h"
#include "glass_local_frontend.h"
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static atomic_int mode;
static atomic_bool entered,holding;
static atomic_bool storage=true;
#ifdef GLV_Q4_PIE
static int selftest_result;
static unsigned expected_bits;
int glv_q4_selftest(struct glv_memory *m){return glv_cancel(m)?-ECANCELED:selftest_result;}
#endif
struct glv_llm {struct glv_memory *memory;};
bool glv_platform_storage_ready(void){return atomic_load(&storage);}
bool glass_ai_start_enter(void){return true;}
void glass_ai_start_leave(void){}
bool glass_ai_config_active(void){return false;}
int glass_ai_config_begin(unsigned t){(void)t;return 0;}
void glass_ai_config_end(void){}
void glv_platform_wake_yield(void){}
int glv_platform_budget(size_t *b,size_t *largest){*b=GLV_MEMORY_LIMIT;*largest=8u*1024u*1024u;return 0;}
int glv_platform_audio_open(void **p,bool (*cancel)(void *),void *arg){(void)p;(void)cancel;(void)arg;assert(!"No playback in request isolation tests");return -ENOSYS;}
int glv_platform_audio_write(void *p,const int16_t *v,unsigned n){(void)p;(void)v;(void)n;assert(0);return -ENOSYS;}
int glv_platform_audio_finish(void *p,bool drain){(void)drain;assert(!p);return 0;}
int glv_capture_platform(struct glv_memory *m,const struct glv_capture_signals *s,const struct glv_capture_options *o,int16_t **p,size_t *n)
{(void)o;*n=1600;*p=glv_alloc(m,*n*sizeof(**p));if(!*p)return -ENOMEM;s->level(s->opaque,1000,100);return 0;}
int glv_asr_assets_check(struct glv_memory *m){return glv_cancel(m);}
int glv_asr_transcribe_pcm(const int16_t *p,size_t n,struct glv_memory *m,char *text,size_t cap,int (*token)(void *,unsigned,unsigned),void *opaque,void (*progress)(void *,unsigned))
{assert(p&&n&&cap>strlen("你好"));progress(opaque,1);progress(opaque,2);int ret=glv_cancel(m);if(!ret)ret=token(opaque,1,0);if(!ret)snprintf(text,cap,"你好");return ret;}
int glv_asr_test_file(unsigned c,struct glv_memory *m,char *s,size_t cap,int (*t)(void *,unsigned,unsigned),void *o,void (*p)(void *,unsigned))
{(void)c;(void)m;(void)s;(void)cap;(void)t;(void)o;(void)p;assert(0);return -ENOSYS;}
int glv_llm_open(const char *root,struct glv_memory *m,struct glv_llm **out)
{(void)root;*out=NULL;if(!atomic_load(&mode))return -ECANCELED;*out=glv_alloc(m,sizeof(**out));if(!*out)return -ENOMEM;(*out)->memory=m;return 0;}
void glv_llm_close(struct glv_llm *s){glv_free(s);}
int glv_llm_encode(struct glv_llm *s,const char *text,int *ids,unsigned cap)
{(void)s;assert(text&&ids&&cap);ids[0]=0;return 1;}
int glv_llm_generate(struct glv_llm *s,const char *prompt,unsigned max,char *answer,size_t cap,glv_text_sink sink,void *opaque)
{
#ifdef GLV_Q4_PIE
 assert(s->memory->q4_bits==expected_bits);
#endif
 (void)s;assert(prompt&&max==96&&cap>strlen("旧回答"));
 if(atomic_load(&mode)==4){
  assert(s->memory->llm_progress);
  s->memory->llm_progress(s->memory->opaque,GLV_LLM_TOKENIZING,0,0);usleep(4000);
  s->memory->llm_progress(s->memory->opaque,GLV_LLM_PREFILL,12,24);
  atomic_store(&entered,true);
  while(atomic_load(&holding))usleep(1000);
  int ret=glv_cancel(s->memory);
  /* A late progress callback must not overwrite STOPPING or its cause. */
  s->memory->llm_progress(s->memory->opaque,GLV_LLM_DECODING,1,96);
  return ret;
 }
 if(atomic_load(&mode)==2){assert(!strstr(prompt,"旧回答")&&!strstr(prompt,"迟到回答"));return -ECANCELED;}
 if(atomic_load(&mode)==5){answer[0]=0;return 0;}
 if(atomic_load(&mode)==3){assert(!strstr(prompt,"空回答输入"));snprintf(answer,cap,"旧回答");return 2;}
 snprintf(answer,cap,"旧回答");assert(!sink(opaque,answer,2));atomic_store(&entered,true);
 while(atomic_load(&holding))usleep(1000);
 /* A tardy engine may return success despite cancellation. The service must
  * still suppress its contents and must not publish them to a new session. */
 assert(sink(opaque,"迟到回答",3)==-ECANCELED);return 2;
}
int glv_frontend_plan(const char *r,const char *t,struct glv_memory *m,struct glv_phone_plan **p)
{(void)r;(void)t;(void)p;assert(m->q4_bits==0);if(atomic_load(&mode)==3)return -ENODATA;assert(!"Cancelled request reached TTS");return -ENOSYS;}
int glv_tts_open(const char *r,struct glv_memory *m,struct glv_tts **s){(void)r;(void)m;(void)s;assert(0);return -ENOSYS;}
void glv_tts_close(struct glv_tts *s){assert(!s);}
int glv_tts_phones(struct glv_tts *s,const int *i,const unsigned char *k,unsigned n,bool leading,glv_audio_sink sink,void *arg)
{(void)s;(void)i;(void)k;(void)n;(void)leading;(void)sink;(void)arg;assert(0);return -ENOSYS;}
static struct glv_snapshot idle(void)
{struct glv_snapshot s;for(unsigned i=0;i<20000;i++){glass_local_voice_get(&s,NULL,0);if(!s.busy&&!s.config_pending)return s;usleep(1000);}assert(!"worker timeout");return s;}
int main(int argc,char **argv)
{
 assert(argc==2);assert(!glass_local_voice_start());struct glv_snapshot s=idle();assert(!s.error&&!s.models_ready&&!s.asr_ready&&!s.profile.sha_bytes);
 assert(!glass_local_voice_check(NULL));s=idle();assert(!s.error&&s.models_ready&&s.asr_ready);
 uint32_t id=0,next=0;char text[1025],reply[4096];
#ifdef GLV_Q4_PIE
 if(!strcmp(argv[1],"q4")){
  assert(!s.q4_bits&&!s.q4_selftest_passed);
  assert(glass_local_voice_q4_backend(16)==-EAGAIN);
  assert(glass_local_voice_q4_backend(8)==-EINVAL);
  assert(!glass_local_voice_q4_selftest(&id));s=idle();assert(!s.error&&s.q4_selftest_passed);
  assert(!glass_local_voice_q4_backend(16));expected_bits=16;atomic_store(&mode,3);
  assert(!glass_local_voice_send("实验",GLV_DIALOGUE,&id));
  assert(glass_local_voice_q4_backend(0)==-EBUSY);s=idle();assert(!s.error&&!s.first_audio_ms);
  assert(!glass_local_voice_q4_backend(0));expected_bits=0;
  assert(!glass_local_voice_send("原路径",GLV_DIALOGUE,&id));s=idle();assert(!s.error&&!s.first_audio_ms);
  assert(!glass_local_voice_q4_backend(16));selftest_result=-EILSEQ;
  assert(!glass_local_voice_q4_selftest(&id));s=idle();assert(s.error==-EILSEQ&&!s.q4_bits&&!s.q4_selftest_passed);
  assert(glass_local_voice_q4_backend(16)==-EAGAIN);
  puts("LOCAL_Q4_SERVICE_OK default-scalar/selftest-gate/idle-only/LLM-only/no-audio/failure-disarms");return 0;
 }
#endif
 if(!strcmp(argv[1],"empty")){
  atomic_store(&mode,5);assert(!glass_local_voice_send("空回答输入",GLV_DIALOGUE,&id));s=idle();
  glass_local_voice_get(NULL,reply,sizeof(reply));assert(s.error==-ENODATA&&!reply[0]&&!s.tokens&&!s.first_audio_ms);
  assert(!glass_local_voice_save(&next));s=idle();assert(s.error==-ENODATA);
  atomic_store(&mode,3);assert(!glass_local_voice_send("有效输入",GLV_DIALOGUE,&id));s=idle();
  assert(!s.error&&!s.first_audio_ms);puts("LOCAL_EMPTY_TEXT_OK no-empty-history/no-audio/recovery");return 0;
 }
 if(!strcmp(argv[1],"diagnostics")){
  const enum glv_cancel_source causes[]={GLV_CANCEL_UI_STOP,GLV_CANCEL_UI_NEW,
   GLV_CANCEL_CLI_STOP,GLV_CANCEL_CLI_NEW,GLV_CANCEL_CONFIG,GLV_CANCEL_VIEW_CLOSE,GLV_CANCEL_STORAGE};
  for(unsigned c=0;c<sizeof(causes)/sizeof(causes[0]);c++){
   atomic_store(&mode,4);atomic_store(&entered,false);atomic_store(&holding,true);
   assert(!glass_local_voice_send("诊断",GLV_DIALOGUE,&id));
   for(unsigned i=0;i<20000&&!atomic_load(&entered);i++)usleep(1000);assert(atomic_load(&entered));
   glass_local_voice_get(&s,NULL,0);assert(s.cancel_source==GLV_CANCEL_NONE&&s.phase==GLV_PREFILL);
   assert(s.progress_done==12&&s.progress_total==24&&s.stage_ms[GLV_TOKENIZING]>=3);
   glass_local_voice_stop_from(id-1,GLV_CANCEL_API);usleep(4000);
   glass_local_voice_get(&s,NULL,0);assert(s.cancel_source==GLV_CANCEL_NONE);
   if(causes[c]==GLV_CANCEL_STORAGE)atomic_store(&storage,false);
   else if(causes[c]==GLV_CANCEL_UI_NEW||causes[c]==GLV_CANCEL_CLI_NEW)assert(!glass_local_voice_new_from(causes[c]));
   else glass_local_voice_stop_from(id,causes[c]);
   if(causes[c]!=GLV_CANCEL_STORAGE){
    glass_local_voice_cancel_from(GLV_CANCEL_API);
    glass_local_voice_get(&s,NULL,0);assert(s.cancel_source==causes[c]&&s.cancel_phase==GLV_PREFILL);
   }
   atomic_store(&holding,false);s=idle();assert(s.cancel_source==causes[c]&&s.cancel_phase==GLV_PREFILL);
   assert(s.error==(causes[c]==GLV_CANCEL_STORAGE?-ENODEV:-ECANCELED));
   assert(s.stage_ms[GLV_PREFILL]>=3&&s.stage_ms[GLV_GENERATING]==0&&!s.first_token_ms&&!s.first_audio_ms);
   unsigned frozen=s.stage_ms[GLV_PREFILL];usleep(4000);glass_local_voice_get(&s,NULL,0);assert(s.stage_ms[GLV_PREFILL]==frozen);
   printf("CANCEL_DIAG_OK source=%u request=%u phase=%u elapsed=%u\n",causes[c],id,s.cancel_phase,s.cancel_ms);
   atomic_store(&storage,true);
  }
  atomic_store(&mode,0);assert(!glass_local_voice_send("未归因",GLV_DIALOGUE,&id));s=idle();
  assert(s.error==-ECANCELED&&s.cancel_source==GLV_CANCEL_UNATTRIBUTED);
  assert(s.cancel_phase==GLV_LOADING_LLM&&!s.progress_total);
  puts("LOCAL_DIAGNOSTICS_OK first-cause/stale-id/reset/timing/late-progress/storage/unattributed/no-audio");return 0;
 }
 if(!strcmp(argv[1],"retag")){
  assert(!glass_local_voice_listen(&id));s=idle();assert(s.error==-ECANCELED&&glass_local_voice_get_transcript(id,text,sizeof(text)));
  assert(!glass_local_voice_check(&next)&&next!=id);s=idle();assert(!s.error);
  bool stale=glass_local_voice_get_transcript(next,text,sizeof(text));printf("MODEL_CHECK_RETAG stale_transcript=%u bytes=%zu\n",stale,strlen(text));fflush(stdout);
  assert(!stale&&!text[0]&&!glass_local_voice_get_transcript(id,text,sizeof(text)));
  assert(!glass_local_voice_listen(&id));s=idle();assert(glass_local_voice_get_transcript(id,text,sizeof(text)));
  assert(!glass_local_voice_save(&next));s=idle();assert(s.error==-ENODATA&&!glass_local_voice_get_transcript(next,text,sizeof(text)));
  puts("LOCAL_TRANSCRIPT_OWNER_OK check/save cannot relabel old recognition");return 0;
 }
 assert(!strcmp(argv[1],"new"));atomic_store(&mode,3);
 assert(!glass_local_voice_send("旧用户输入",GLV_DIALOGUE,&id));s=idle();assert(!s.error&&!s.first_audio_ms);
 atomic_store(&mode,1);atomic_store(&holding,true);
 assert(!glass_local_voice_listen(&id));for(unsigned i=0;i<20000&&!atomic_load(&entered);i++)usleep(1000);assert(atomic_load(&entered));
 glass_local_voice_get(&s,reply,sizeof(reply));assert(s.busy&&reply[0]&&glass_local_voice_get_transcript(id,text,sizeof(text)));
 assert(!glass_local_voice_new());glass_local_voice_get(&s,reply,sizeof(reply));bool stale=glass_local_voice_get_transcript(id,text,sizeof(text));
 printf("NEW_SESSION_PENDING_CLEAR stale_reply_bytes=%zu stale_transcript=%u\n",strlen(reply),stale);fflush(stdout);
 assert(s.busy&&!reply[0]&&!stale&&!text[0]);assert(glass_local_voice_send("新输入",GLV_DIALOGUE,NULL)==-EBUSY);
 atomic_store(&holding,false);s=idle();glass_local_voice_get(NULL,reply,sizeof(reply));assert(s.error==-ECANCELED&&!reply[0]&&!glass_local_voice_get_transcript(id,text,sizeof(text)));
 atomic_store(&mode,2);assert(!glass_local_voice_send("新输入",GLV_DIALOGUE,&next));s=idle();assert(s.error==-ECANCELED&&next!=id);
 puts("LOCAL_NEW_SESSION_ISOLATION_OK immediate clear/tardy-engine suppression/no-old-history/no-audio");return 0;
}
