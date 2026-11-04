/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_local_voice.h"
#include "glass_local_platform.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdatomic.h>
#include <dirent.h>
static size_t budget=GLV_MEMORY_LIMIT;static unsigned opens,closes,frames;
static bool stop_on_audio;static uint32_t pcm_hash=2166136261u;
static atomic_bool card=true,fixture_hold_budget=false;
bool glv_platform_storage_ready(void){return atomic_load(&card);}
bool glass_ai_start_enter(void){return true;}
void glass_ai_start_leave(void){}
bool glass_ai_config_active(void){return false;}
int glass_ai_config_begin(unsigned timeout)
{(void)timeout;glass_local_voice_cancel();assert(glass_local_voice_config_idle());return 0;}
void glass_ai_config_end(void){}
void glv_platform_wake_yield(void){}
int glv_platform_budget(size_t *b,size_t *largest){while(atomic_load(&fixture_hold_budget))usleep(1000);*b=budget;*largest=8*1024*1024;return budget<4608*1024?-ENOMEM:0;}
int glv_platform_audio_open(void **audio,bool (*cancel)(void *),void *arg)
{if(cancel(arg))return -ECANCELED;assert(opens==closes);opens++;*audio=audio;return 0;}
int glv_platform_audio_write(void *audio,const int16_t *pcm,unsigned n)
{assert(audio&&pcm&&n<=240);frames+=n;for(unsigned i=0;i<n;i++){uint16_t v=(uint16_t)pcm[i];pcm_hash=(pcm_hash^(v&255))*16777619u;pcm_hash=(pcm_hash^(v>>8))*16777619u;}if(stop_on_audio){stop_on_audio=false;glass_local_voice_cancel();}return 0;}
int glv_platform_audio_finish(void *audio,bool drain)
{(void)drain;if(audio)closes++;return 0;}
static struct glv_snapshot wait_idle(void)
{
  struct glv_snapshot s;
  for(unsigned i=0;i<18000;i++){glass_local_voice_get(&s,NULL,0);if(!s.busy&&!s.config_pending)return s;usleep(10000);}
  assert(!"service timeout");return s;
}
static void manifest_flip(void)
{
  FILE *f=fopen(GLV_RELEASE_ROOT "/manifest.bin","r+b");assert(f);assert(!fseek(f,116,SEEK_SET));int v=fgetc(f);assert(v>=0);
  assert(!fseek(f,116,SEEK_SET));assert(fputc(v^1,f)>=0);fclose(f);
}
int main(void)
{
  assert(!glass_local_voice_start());struct glv_snapshot s=wait_idle();assert(!s.error&&!s.models_ready&&!s.asr_ready&&!s.wake_enabled&&!s.profile.sha_bytes);
  assert(!glass_local_voice_check(NULL));s=wait_idle();assert(!s.error&&s.models_ready&&!s.asr_ready);
  assert(!glass_local_voice_enable_wake(true));s=wait_idle();assert(s.wake_enabled);
  uint32_t request=0,old;
  bool bulk=getenv("GLV_TEST_TTS_BULK")!=NULL;assert(!glass_local_voice_tts_rows(bulk));
#ifdef GLV_TTS_PIE
  bool planned=!getenv("GLV_TEST_TTS_LEGACY_WINDOW");assert(!glass_local_voice_tts_windows(planned));
#endif
#if defined(GLV_TTS_PIE) && defined(GLV_TTS_PARALLEL)
  bool input_parallel=getenv("GLV_TEST_TTS_INPUT")!=NULL;assert(!glass_local_voice_tts_input(input_parallel));
#endif
  assert(glass_local_voice_tts_bench_limit(5000)==-EINVAL);
  unsigned cap=getenv("GLV_TEST_TTS_CAP")?4096:0;assert(!glass_local_voice_tts_bench_limit(cap));
  if(getenv("GLV_TEST_TTS_BENCH")){
    assert(!glass_local_voice_tts_kernel(getenv("GLV_TEST_TTS_SINGLE")!=NULL));
#ifdef GLV_TTS_PARALLEL
    bool parallel=!getenv("GLV_TEST_TTS_SERIAL");assert(!glass_local_voice_tts_parallel(parallel));
#endif
#ifdef GLV_TTS_FMA
    bool fma=getenv("GLV_TEST_TTS_FMA")!=NULL;assert(!glass_local_voice_tts_fma(fma));
#endif
    assert(!glass_local_voice_tts_benchmark("你好。",&request));s=wait_idle();
    assert(!s.error&&!opens&&!frames&&!s.first_audio_ms&&!s.tokens);
    assert(s.tts_bench_limit_kib==cap);
#if defined(GLV_TTS_PIE) && defined(GLV_TTS_PARALLEL)
    assert(s.tts_parallel_input==input_parallel);
    if(input_parallel&&!s.tts_serial_fp)assert(s.profile.tts_input_max_jobs>0&&s.profile.tts_input_quant_jobs>0&&!s.profile.tts_input_rounding_fallbacks);
    else assert(!s.profile.tts_input_max_jobs&&!s.profile.tts_input_quant_jobs);
#endif
    assert(s.profile.tts_samples==21000&&s.profile.tts_ready_ms&&s.profile.tts_output_frames==60);
#ifdef GLV_TTS_FMA
    assert(s.tts_bulk_rows==bulk);if(bulk)assert(s.profile.weight_q8_bulk_rows>0);
    assert(s.tts_fma==fma&&s.profile.tts_fma_upsample_calls==(fma?4*s.profile.tts_chunks:0)&&!s.profile.tts_fma_rejections);
#endif
#ifdef GLV_TTS_PARALLEL
    assert(s.tts_serial_fp==!parallel);assert((s.profile.tts_parallel_jobs>0)==parallel);
#endif
#ifdef GLV_TTS_PIE
    assert(s.profile.tts_pie_calls==74*s.profile.tts_chunks&&s.profile.tts_pie_dot_calls&&s.profile.tts_pie_selftests==1);
    assert(s.profile.tts_pie_batches || s.tts_single_dot);
#endif
    assert(!glass_local_voice_new());s=wait_idle();assert(!s.error);
    puts("LOCAL_VOICE_TTS_BENCH_NO_AUDIO_REAL_WORKER_OK");return 0;
  }
  if(getenv("GLV_TEST_SAVE_ONLY")){
    assert(access(GLV_SESSION_ROOT,F_OK));
    assert(!glass_local_voice_send("你好",GLV_DIALOGUE,&request));s=wait_idle();assert(!s.error&&s.tokens&&!opens&&!frames&&!s.first_audio_ms);
    printf("SERVICE_TIMING first_token_ms=%u first_audio_ms=%u elapsed_ms=%u peak=%zu tokens=%u\n",s.first_token_ms,s.first_audio_ms,s.elapsed_ms,s.memory_peak,s.tokens);
    assert(access(GLV_SESSION_ROOT,F_OK));
    assert(!glass_local_voice_save(&request));s=wait_idle();assert(!s.error);
    DIR *dir=opendir(GLV_SESSION_ROOT);assert(dir);struct dirent *item;unsigned count=0;
    while((item=readdir(dir)))if(!strncmp(item->d_name,"session-",8)){
      char path[512],text[8192];snprintf(path,sizeof(path),"%s/%s",GLV_SESSION_ROOT,item->d_name);
      FILE *f=fopen(path,"rb");assert(f);size_t n=fread(text,1,sizeof(text)-1,f);text[n]=0;fclose(f);
      assert(strstr(text,"你好")&&strstr(text,"assistant"));count++;
    }
    closedir(dir);assert(count==1);assert(!glass_local_voice_new());
    assert(!glass_local_voice_save(&request));s=wait_idle();assert(s.error==-ENODATA);
    printf("PCM_HASH frames=%u hash=%08x\n",frames,pcm_hash);
    puts("LOCAL_VOICE_EXPLICIT_SAVE_OK no-implicit-write/real-answer/SD-only/new-session");return 0;
  }
  assert(!glass_local_voice_send("你好。",GLV_READ,&request));usleep(20000);glass_local_voice_get(&s,NULL,0);assert(s.busy&&s.elapsed_ms>0);assert(glass_local_voice_tts_rows(!bulk)==-EBUSY);assert(glass_local_voice_tts_windows(false)==-EBUSY);assert(glass_local_voice_tts_bench_limit(0)==-EBUSY);assert(glass_local_voice_tts_input(false)==-EBUSY);s=wait_idle();assert(!s.error);
  assert(frames>=3000&&opens==1&&closes==1&&s.memory_peak<=GLV_MEMORY_LIMIT&&s.first_audio_ms);
  budget=5376*1024;assert(!glass_local_voice_send("你好。",GLV_READ,&request));s=wait_idle();assert(!s.error&&s.memory_peak<budget);budget=GLV_MEMORY_LIMIT;
  stop_on_audio=true;unsigned opened=opens;
  assert(!glass_local_voice_send("你好。",GLV_READ,&request));s=wait_idle();assert(s.error==-ECANCELED&&!s.busy&&opens==opened+1&&opens==closes);
  DIR *directory=opendir(GLV_SCRATCH_ROOT);assert(directory);struct dirent *entry;
  while((entry=readdir(directory)))assert(strncmp(entry->d_name,".speech-",8));closedir(directory);
  old=request;assert(!glass_local_voice_send("你好。",GLV_DIALOGUE,&request));
  glass_local_voice_stop(old);assert(glass_local_voice_send("新请求",GLV_READ,NULL)==-EBUSY);
  glass_local_voice_new();s=wait_idle();assert(s.error==-ECANCELED);char reply[4096];glass_local_voice_get(NULL,reply,sizeof(reply));assert(!reply[0]);
  for(unsigned i=0;i<12;i++){assert(!glass_local_voice_check(&request));glass_local_voice_stop(request);s=wait_idle();assert(!s.error||s.error==-ECANCELED);}
  budget=1024;assert(!glass_local_voice_check(&request));s=wait_idle();assert(s.error==-ENOMEM);budget=GLV_MEMORY_LIMIT;
  manifest_flip();assert(!glass_local_voice_check(&request));s=wait_idle();assert(s.error==-EBADMSG&&!s.models_ready&&s.model_error==-EBADMSG);
  assert(!glass_local_voice_enable_wake(false));s=wait_idle();assert(!s.error&&!s.models_ready&&s.model_error==-EBADMSG);
  assert(!glass_local_voice_enable_wake(true));s=wait_idle();assert(!s.error&&!s.models_ready&&s.model_error==-EBADMSG);manifest_flip();
  assert(!glass_local_voice_check(&request));s=wait_idle();assert(!s.error&&s.models_ready&&!s.model_error);
  assert(!glass_local_voice_send("你好。",GLV_READ,&request));atomic_store(&card,false);s=wait_idle();assert(s.error==-ENODEV&&!s.busy&&!s.models_ready&&!glass_local_voice_wake_ready());atomic_store(&card,true);
  assert(!rename(GLV_RELEASE_ROOT "/manifest.bin",GLV_RELEASE_ROOT "/manifest.saved"));
  assert(!glass_local_voice_check(&request));s=wait_idle();assert(s.error==-ENOENT&&!s.models_ready);
  assert(!rename(GLV_RELEASE_ROOT "/manifest.saved",GLV_RELEASE_ROOT "/manifest.bin"));
  assert(!glass_local_voice_check(&request));s=wait_idle();assert(!s.error&&s.models_ready);
  assert(!glass_local_voice_save(&request));s=wait_idle();assert(s.error==-ENODATA);
  assert(!glass_local_voice_set_wake(false));s=wait_idle();assert(!s.error&&!s.local_wake);
  assert(glass_local_voice_set_wake(true)==-EPERM);s=wait_idle();assert(!s.error&&!s.local_wake);
  assert(!glass_local_voice_enable_wake(false));s=wait_idle();assert(!s.error&&!s.wake_enabled&&!glass_local_voice_wake_ready());
  assert(!glass_local_voice_set_wake(false));s=wait_idle();assert(!s.error&&!s.wake_enabled&&!s.local_wake);
  assert(!glass_local_voice_enable_wake(true));s=wait_idle();assert(!s.error&&s.wake_enabled&&!s.local_wake);
  assert(glass_local_voice_set_wake(true)==-EPERM);s=wait_idle();assert(!s.error&&s.wake_enabled&&!s.local_wake);
  unsigned elapsed=s.elapsed_ms;uint32_t before=s.request;
  glass_local_voice_wake_failure(-EAGAIN);glass_local_voice_get(&s,NULL,0);assert(s.models_ready);
  atomic_store(&fixture_hold_budget,true);assert(!glass_local_voice_check(&request));
  glass_local_voice_wake_failure(-EBADMSG);glass_local_voice_get(&s,NULL,0);assert(s.busy&&s.models_ready);
  atomic_store(&fixture_hold_budget,false);s=wait_idle();assert(!s.error&&s.models_ready);
  elapsed=s.elapsed_ms;before=s.request;glass_local_voice_wake_failure(-EBADMSG);glass_local_voice_get(&s,NULL,0);
  assert(s.models_ready&&!s.model_error&&s.wake_error==-EBADMSG&&!glass_local_voice_wake_ready()&&s.elapsed_ms==elapsed&&s.request==before);
  assert(!glass_local_voice_enable_wake(false));s=wait_idle();assert(s.models_ready&&!s.wake_error);
  assert(!glass_local_voice_enable_wake(true));s=wait_idle();assert(s.models_ready&&!s.wake_error&&glass_local_voice_wake_ready());
  assert(!glass_local_voice_check(&request));s=wait_idle();assert(s.models_ready&&!s.error&&!s.model_error);
  assert(opens==closes);puts("LOCAL_VOICE_SERVICE_OK cleanup/cancel/stale-request/corrupt/low-memory/target-switch/native-24k");return 0;
}
