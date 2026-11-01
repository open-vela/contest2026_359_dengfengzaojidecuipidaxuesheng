/* SPDX-License-Identifier: Apache-2.0 */
/* Real worker, SHA and LLM/TTS engines. All device interfaces are inert. */
#include "glass_local_voice.h"
#include "glass_local_capture.h"
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static atomic_uint llm_files,tts_files,wake_files,other_files,unicode_files,budgets,asr_checks,captures,opens,closes,frames;
static atomic_bool card=true;
FILE *__real_fopen(const char *,const char *);
FILE *__wrap_fopen(const char *path,const char *mode)
{
  if(!strncmp(path,GLV_RELEASE_ROOT,strlen(GLV_RELEASE_ROOT))){
    if(strstr(path,"/edgeidiot/tokenizer.bin"))unicode_files++;
    else if(strstr(path,"/edgeidiot/"))llm_files++;
    else if(strstr(path,"/tts-v2/"))tts_files++;
    else if(strstr(path,"/wake/"))wake_files++;
    else other_files++;
  }return __real_fopen(path,mode);
}
bool glv_platform_storage_ready(void){return atomic_load(&card);}
bool glass_ai_start_enter(void){return true;}
void glass_ai_start_leave(void){}
bool glass_ai_config_active(void){return false;}
int glass_ai_config_begin(unsigned n){(void)n;return 0;}
void glass_ai_config_end(void){}
void glv_platform_wake_yield(void){}
int glv_platform_budget(size_t *n,size_t *largest){budgets++;*n=GLV_MEMORY_LIMIT;*largest=8u*1024u*1024u;return 0;}
int glv_platform_audio_open(void **p,bool (*cancel)(void *),void *o)
{if(cancel(o))return -ECANCELED;opens++;*p=(void *)1;return 0;}
int glv_platform_audio_write(void *p,const int16_t *s,unsigned n){assert(p&&s&&n);frames+=n;return 0;}
int glv_platform_audio_finish(void *p,bool drain){(void)drain;if(p)closes++;return 0;}
int glv_asr_assets_check(struct glv_memory *m){asr_checks++;return glv_cancel(m)?-ECANCELED:-ENOENT;}
int glv_capture_platform(struct glv_memory *m,const struct glv_capture_signals *s,const struct glv_capture_options *o,int16_t **p,size_t *n)
{(void)m;(void)s;(void)o;(void)p;(void)n;captures++;assert(!"Missing ASR must not acquire the microphone");return -ENOSYS;}
int glv_asr_transcribe_pcm(const int16_t *p,size_t n,struct glv_memory *m,char *s,size_t cap,int (*t)(void *,unsigned,unsigned),void *o,void (*progress)(void *,unsigned))
{(void)p;(void)n;(void)m;(void)s;(void)cap;(void)t;(void)o;(void)progress;assert(0);return -ENOSYS;}
int glv_asr_test_file(unsigned c,struct glv_memory *m,char *s,size_t cap,int (*t)(void *,unsigned,unsigned),void *o,void (*progress)(void *,unsigned))
{(void)c;return glv_asr_transcribe_pcm(NULL,0,m,s,cap,t,o,progress);}
static struct glv_snapshot idle(void)
{struct glv_snapshot s;for(unsigned i=0;i<60000;i++){glass_local_voice_get(&s,NULL,0);if(!s.busy&&!s.config_pending)return s;usleep(1000);}assert(!"worker timeout");return s;}
int main(int argc,char **argv)
{
  assert(argc==2);assert(!glass_local_voice_start());struct glv_snapshot s=idle();
  assert(!s.request&&!s.error&&!s.models_ready&&!s.asr_ready&&!s.wake_enabled&&!s.profile.sha_bytes);
  usleep(50000);assert(!llm_files&&!tts_files&&!wake_files&&!other_files&&!unicode_files&&!budgets&&!asr_checks&&!captures&&!opens);
  uint32_t id;char text[4096];
  if(!strcmp(argv[1],"cold")){
    assert(!glass_local_voice_enable_wake(true));s=idle();assert(s.wake_enabled&&glass_local_voice_wake_ready()&&!s.models_ready);
    glass_local_voice_wake_failure(-EBADMSG);s=idle();assert(s.wake_error==-EBADMSG&&!glass_local_voice_wake_ready()&&!s.model_error);
    assert(!glass_local_voice_enable_wake(false));s=idle();assert(!s.wake_error&&!glass_local_voice_wake_ready());
    assert(!llm_files&&!tts_files&&!wake_files&&!other_files&&!budgets&&!asr_checks&&!captures&&!opens);
    assert(!glass_local_voice_listen(&id));s=idle();assert(s.error==-ENOENT&&asr_checks==1&&!captures&&!llm_files&&!tts_files&&!wake_files&&!opens);
  }else if(!strncmp(argv[1],"chat",4)){
    assert(!glass_local_voice_send("你好",GLV_DIALOGUE,&id));s=idle();glass_local_voice_get(NULL,text,sizeof(text));
    assert(!s.error&&s.models_ready&&!s.asr_ready&&s.tokens&&text[0]&&!s.first_audio_ms);
    assert(s.profile.sha_bytes==GLV_EXPECT_LLM_BYTES&&s.profile.verified_reuses==1);
    uint32_t hash=2166136261u;for(const unsigned char *p=(const unsigned char *)text;*p;p++)hash=(hash^*p)*16777619u;
    printf("LAZY_CHAT_REPLY tokens=%u bytes=%zu fnv=%08x\n",s.tokens,strlen(text),hash);fflush(stdout);
    assert(llm_files&&!tts_files&&!wake_files&&!asr_checks&&!captures&&!opens&&!frames);
    if(!strcmp(argv[1],"chat-idle-card")){
      unsigned files=llm_files,checks=budgets;unsigned revision=s.revision;
      atomic_store(&card,false);glass_local_voice_get(&s,NULL,0);
      assert(!s.busy&&!s.models_ready&&!s.asr_ready&&s.model_error==-ENODEV&&s.revision>revision);
      assert(!s.error&&s.tokens&&llm_files==files&&budgets==checks);
      atomic_store(&card,true);glass_local_voice_get(&s,NULL,0);
      assert(!s.models_ready&&s.model_error==-ENODEV&&llm_files==files);
      assert(!glass_local_voice_send("你好",GLV_DIALOGUE,&id));s=idle();
      assert(!s.error&&s.models_ready&&!s.model_error&&!opens&&!wake_files&&!asr_checks);
      puts("IDLE_CARD_INVALIDATION_AND_EXPLICIT_RECOVERY_OK");return 0;
    }
    assert(!glass_local_voice_send(text,GLV_READ,&id));s=idle();assert(s.error==-ENOENT&&s.models_ready&&!s.model_error&&!opens);
    assert(!glass_local_voice_send("你好",GLV_DIALOGUE,&id));s=idle();assert(!s.error&&s.models_ready&&!opens&&!wake_files&&!asr_checks);
  }else{
    assert(!glass_local_voice_send("你好。",GLV_READ,&id));
    bool removal=!strcmp(argv[1],"read-card");
    if(removal){
      for(unsigned i=0;i<60000;i++){glass_local_voice_get(&s,NULL,0);if(s.phase==GLV_SYNTHESIZING)break;assert(s.busy);usleep(1000);}
      assert(s.phase==GLV_SYNTHESIZING);atomic_store(&card,false);
    }
    s=idle();assert(!s.models_ready&&!llm_files&&!wake_files&&!asr_checks&&!captures&&tts_files);
    if(removal)assert(s.error==-ENODEV&&!opens&&!frames);
    else {
      printf("READ_RESULT error=%d first_audio=%u opens=%u closes=%u frames=%u sha=%llu expected=%u reused=%u\n",s.error,s.first_audio_ms,(unsigned)opens,(unsigned)closes,(unsigned)frames,(unsigned long long)s.profile.sha_bytes,GLV_EXPECT_TTS_BYTES,s.profile.verified_reuses);fflush(stdout);
      assert(!s.error&&s.first_audio_ms&&opens==1&&closes==1&&frames>3000&&s.profile.sha_bytes==GLV_EXPECT_TTS_BYTES&&s.profile.verified_reuses==1);
    }
  }
  assert(opens==closes);
  printf("LAZY_MODE_OK case=%s llm_files=%u tts_files=%u wake_files=%u asr_checks=%u captures=%u audio_opens=%u sha_bytes=%llu peak=%zu\n",
    argv[1],(unsigned)llm_files,(unsigned)tts_files,(unsigned)wake_files,(unsigned)asr_checks,(unsigned)captures,(unsigned)opens,(unsigned long long)s.profile.sha_bytes,s.memory_peak);
  return 0;
}
