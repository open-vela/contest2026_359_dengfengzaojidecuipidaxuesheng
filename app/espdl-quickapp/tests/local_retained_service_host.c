/* SPDX-License-Identifier: Apache-2.0 */
/* Real worker/SHA/Q4 engine; deterministic media generation and inert audio. */
#include "glass_local_voice.h"
#include "glass_local_platform.h"
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static atomic_uint generation,media=1;
static atomic_bool card=true,hold,entered;
static bool capable=true;
#include "glass_local_llm.h"
int __real_glv_llm_generate_tool(struct glv_llm *,const char *,unsigned,char *,size_t,glv_text_sink,void *);
int __wrap_glv_llm_generate_tool(struct glv_llm *p,const char *prompt,unsigned n,char *reply,size_t cap,glv_text_sink sink,void *arg)
{
 if(atomic_load(&hold)){atomic_store(&entered,true);while(atomic_load(&hold))usleep(1000);}
 return __real_glv_llm_generate_tool(p,prompt,n,reply,cap,sink,arg);
}
static atomic_int pressure;
int glv_platform_memory_pressure(void){return atomic_load(&pressure);}
bool glv_platform_storage_ready(void){return atomic_load(&card);}
int glv_platform_storage_stamp(struct glv_storage_stamp *s)
{if(!capable)return -ENOTSUP;if(!atomic_load(&card))return -ENODEV;s->writes=atomic_load(&generation);s->media=atomic_load(&media);return 0;}
int glv_platform_budget(size_t *n,size_t *largest){*n=GLV_MEMORY_LIMIT;*largest=16u*1024u*1024u;return 0;}
int glv_platform_session_budget(size_t retained,size_t *n,size_t *largest){assert(retained);return glv_platform_budget(n,largest);}
bool glass_ai_start_enter(void){return true;}
void glass_ai_start_leave(void){}
bool glass_ai_config_active(void){return false;}
int glass_ai_config_begin(unsigned n){(void)n;return 0;}
void glass_ai_config_end(void){}
void glv_platform_wake_yield(void){}
int glv_platform_audio_open(void **p,bool (*c)(void *),void *o){(void)p;(void)c;(void)o;assert(!"No audio in retained text test");return -ENOSYS;}
int glv_platform_audio_write(void *p,const int16_t *s,unsigned n){(void)p;(void)s;(void)n;assert(0);return -ENOSYS;}
int glv_platform_audio_finish(void *p,bool d){(void)d;assert(!p);return 0;}
static struct glv_snapshot idle(void)
{
  struct glv_snapshot s;for(unsigned i=0;i<120000;i++){glass_local_voice_get(&s,NULL,0);
    if(!s.busy&&!s.config_pending&&!s.session_pending)return s;usleep(1000);}
  assert(!"Worker timeout");return s;
}
static struct glv_snapshot chat(const char *text,char *answer)
{
  uint32_t id;assert(!glass_local_voice_send(text,GLV_DIALOGUE,&id));struct glv_snapshot s=idle();
  assert(s.request==id&&!s.first_audio_ms);glass_local_voice_get(&s,answer,4096);return s;
}
int main(int argc,char **argv)
{
  assert(argc==3);const char *mode=argv[1];capable=strcmp(mode,"cold")!=0;
  FILE *out=fopen(argv[2],"wb");assert(out);assert(!glass_local_voice_start());struct glv_snapshot s=idle();
  assert(!s.request&&!s.profile.sha_bytes&&!s.session_resident&&!s.session_bytes);
  char answer[4096];s=chat("你好",answer);assert(!s.error&&s.models_ready&&s.tokens>0);
  assert(s.profile.sha_bytes>10046336&&!s.profile.session_reuses);
  assert(s.session_resident==capable);uint32_t bytes=(uint32_t)strlen(answer);
  assert(fwrite(&bytes,4,1,out)==1&&fwrite(answer,1,bytes,out)==bytes);
  if(!strcmp(mode,"cancel")||!strcmp(mode,"close-active")){
    atomic_store(&hold,true);uint32_t id;assert(!glass_local_voice_send("1加1等于几？",GLV_DIALOGUE,&id));
    for(unsigned i=0;i<5000&&!atomic_load(&entered);i++)usleep(1000);assert(atomic_load(&entered));
    assert(glass_local_voice_yield_idle_model(200)==-EBUSY);
    if(!strcmp(mode,"cancel")){assert(!glass_local_voice_new_from(GLV_CANCEL_UI_NEW));glass_local_voice_get(&s,answer,sizeof(answer));assert(!answer[0]&&!s.tokens);}
    else glass_local_voice_release();
    atomic_store(&hold,false);s=idle();assert(!s.session_resident&&!s.session_bytes);
    if(!strcmp(mode,"cancel")){assert(s.error==-ECANCELED&&s.cancel_source==GLV_CANCEL_UI_NEW);glass_local_voice_get(&s,answer,sizeof(answer));assert(!answer[0]);}
    else assert(!s.error&&s.tokens);
  }else if(!strcmp(mode,"pressure")){
    assert(s.profile.llm_kv_bytes>0);size_t hot=s.session_bytes;
    atomic_store(&pressure,1);usleep(700000);s=idle();
    assert(s.session_resident&&s.session_bytes<hot&&!s.profile.llm_kv_bytes);
    atomic_store(&pressure,0);s=chat("你好",answer);assert(!s.error&&s.profile.session_reuses);
    atomic_store(&pressure,2);usleep(700000);s=idle();assert(!s.session_resident&&!s.session_bytes);
    atomic_store(&pressure,0);
  }else if(!strcmp(mode,"idle-yield")){
    assert(!glass_local_voice_yield_idle_model(3000));s=idle();assert(!s.session_resident&&!s.session_bytes);
  }else if(!strcmp(mode,"corrupt")){
    atomic_fetch_add(&generation,1);char path[512];snprintf(path,sizeof(path),"%s/edgeidiot/weights.bin",GLV_RELEASE_ROOT);
    FILE *f=fopen(path,"r+b");assert(f);int old=fgetc(f);assert(!fseek(f,0,SEEK_SET));assert(fputc(old^1,f)!=EOF);fclose(f);atomic_fetch_add(&generation,1);
    usleep(600000);s=chat("你好",answer);assert(s.error==-EBADMSG&&!s.session_resident&&!s.models_ready&&!answer[0]);
  }else if(!strcmp(mode,"media")){
    atomic_store(&card,false);atomic_fetch_add(&media,1);usleep(600000);s=idle();assert(!s.session_resident&&!s.session_bytes&&!s.models_ready);
    atomic_store(&card,true);atomic_fetch_add(&media,1);s=chat("你好",answer);assert(!s.error&&!s.profile.session_reuses&&s.profile.sha_bytes);
  }else{
    if(!strcmp(mode,"new")){assert(!glass_local_voice_new());s=idle();assert(!s.session_resident&&!s.session_bytes);}
    else if(!strcmp(mode,"release")){glass_local_voice_release();s=idle();assert(!s.session_resident&&!s.session_bytes);}
    else if(!strcmp(mode,"audio")){assert(!glass_local_voice_yield_model(3000));s=idle();assert(!s.session_resident&&!s.session_bytes);}
    else if(!strcmp(mode,"config")){glass_local_voice_cancel_from(GLV_CANCEL_CONFIG);s=idle();assert(glass_local_voice_config_idle()&&!s.session_resident);}
    else if(!strcmp(mode,"writer")){atomic_fetch_add(&generation,2);usleep(600000);s=idle();assert(!s.session_resident&&!s.session_bytes);}
    s=chat("1加1等于几？",answer);assert(!s.error&&s.models_ready);
    bool reuse=!strcmp(mode,"warm");
    assert(s.profile.session_reuses==reuse);assert((s.profile.sha_bytes==0)==reuse);
    if(reuse)assert(s.profile.kv_reused_tokens>0&&s.profile.prefill_tokens>0);
    bytes=(uint32_t)strlen(answer);assert(fwrite(&bytes,4,1,out)==1&&fwrite(answer,1,bytes,out)==bytes);
    printf("RETAINED_MEASURE case=%s first_ms=%u sha=%llu reused=%u kv=%u prefill=%u bytes=%zu\n",mode,s.first_token_ms,
      (unsigned long long)s.profile.sha_bytes,s.profile.session_reuses,s.profile.kv_reused_tokens,s.profile.prefill_tokens,s.session_bytes);
  }
  glass_local_voice_release();s=idle();assert(!s.session_resident&&!s.session_pending&&!s.session_bytes);
  fclose(out);printf("RETAINED_SERVICE_OK case=%s\n",mode);return 0;
}
