/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_local_voice.h"
#include "glass_local_model.h"
#include "glass_local_capture.h"
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

int glv_capture_platform(struct glv_memory *m,const struct glv_capture_signals *s,const struct glv_capture_options *o,int16_t **p,size_t *n)
{(void)m;(void)s;(void)o;(void)p;(void)n;assert(!"file job must not open a microphone");return -ENOSYS;}
int glv_capture_platform_stream(struct glv_memory *m,const struct glv_capture_signals *s,const struct glv_capture_options *o,glv_capture_consume consume,void *arg,glv_capture_stream_progress progress)
{(void)m;(void)s;(void)o;(void)consume;(void)arg;(void)progress;assert(!"file job must not open a microphone");return -ENOSYS;}
int main(int argc,char **argv){
 assert(argc==2);assert(!glass_local_voice_start());idle();uint32_t id;
 assert(!glass_local_voice_send("preserved local reply",GLV_READ,&id));idle();char prior[GLV_MAX_REPLY],after[GLV_MAX_REPLY];glass_local_voice_get(NULL,prior,sizeof(prior));assert(!strcmp(prior,"preserved local reply"));
 assert(!glass_local_voice_transcribe_file(argv[1],&id));struct glv_snapshot s=idle();
 printf("FILE_STATE error=%d asr=%d ready=%u model=%u tokens=%u rounds=%u\n",s.error,s.asr_error,s.asr_ready,s.models_ready,s.tokens,s.model_rounds);fflush(stdout);
 assert(!s.error&&s.asr_ready&&!s.models_ready&&!s.tokens&&!s.model_rounds&&!s.first_audio_ms);
 struct glv_file_transcript *text=malloc(sizeof(*text));assert(text);
 assert(glass_local_voice_file_result(id+1,text)==-ESTALE);assert(!glass_local_voice_file_result(id,text));
 assert(text->duration_ms==5611&&text->count==25&&fnv(text->text)==0x04bc9ea5u);
 glass_local_voice_get(NULL,after,sizeof(after));assert(!strcmp(after,prior)&&s.selected_mode==GLV_READ);
 unsigned end=0,ms=0;for(unsigned i=0;i<text->count;i++){
  assert(text->marks[i].end>end&&text->marks[i].end<=strlen(text->text));
  assert(text->marks[i].ms>=ms&&text->marks[i].ms<=text->duration_ms);
  assert((text->text[text->marks[i].end]&0xc0)!=0x80);end=text->marks[i].end;ms=text->marks[i].ms;
 }assert(end==strlen(text->text));free(text);
 atomic_store(&admit,false);assert(glass_local_voice_transcribe_file(argv[1],NULL)==-EBUSY);atomic_store(&admit,true);
 assert(!glass_local_voice_transcribe_file(argv[1],&id));transcribing();glass_local_voice_stop(id+1);glass_local_voice_get(&s,NULL,0);assert(s.busy);
 glass_local_voice_stop(id);s=idle();assert(s.error==-ECANCELED&&!s.tokens&&!s.model_rounds);
 puts("RECORDER_REAL_ASR_FILE_OK: actual model text, duration, monotone audio marks, no microphone/LLM/audio; stale request and cancellation");return 0;
}
