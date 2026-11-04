/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_local_voice.h"
#ifdef GLV_ASR_STREAMING
#include "local_asr.h"
#ifdef GLV_ASR_ACCEL_ENABLED
#include "local_asr_accel.h"
#endif
#endif
#include "glass_local_assets.h"
#include "glass_local_platform.h"
#include "glass_local_llm.h"
#include "glass_local_tts.h"
#include "glass_local_frontend.h"
#include "glass_ai_config.h"
#include "glass_music.h"
#ifdef GLASS_LOCAL_CONTROL
#include "glass_local_control.h"
#include "glass_portal.h"
#include "glass_board_home.h"
#endif
#ifdef GLV_Q4_PIE
#include "local_q4_pie.h"
#endif
#ifdef GLV_ASR_EVALUATION
#include "glass_local_asr.h"
#endif
#ifdef GLV_ASR_MICROPHONE
#include "glass_local_capture.h"
#endif
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifndef GLV_CONFIG_PATH
#define GLV_CONFIG_PATH "/data/config/local-voice-wake"
#endif
#define INPUT_MAX 1024
#define HISTORY 3
struct retained_llm {
  struct glv_memory memory;struct glv_llm *llm;struct glv_asset_lease *lease;
  struct glv_storage_stamp stamp;unsigned backend;bool control_model;
};
struct local_service {
  struct glv_snapshot state;
  char input[INPUT_MAX+1],reply[GLV_MAX_REPLY],history[HISTORY][INPUT_MAX+GLV_MAX_REPLY+96];
  char replies[3][GLV_MAX_REPLY];
  unsigned history_count,job;enum glv_mode mode;
#ifdef GLASS_LOCAL_CONTROL
  atomic_bool home_cancel;
#endif
  uint32_t next,cancel,finish_capture,transcript_request;bool clear_history,desired_wake,desired_enabled;
  char transcript[INPUT_MAX+1];
  struct glv_file_transcript file;uint32_t file_request;
  uint64_t began,phase_began;
  struct retained_llm kept;bool retire_session;pthread_t owner;
};
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static struct local_service *service;
static bool starting;
static void request_retire_locked(void);
/* The control profile owns receipts, never the ordinary chat context. */
static void clear_selected_history_locked(void)
{
 service->replies[service->mode][0]=0;
 if(service->mode==GLV_DIALOGUE){service->history_count=0;memset(service->history,0,sizeof(service->history));}
}
struct audio_chunk {struct audio_chunk *next;unsigned count;float samples[];};
struct run {bool control,control_generated;uint32_t request;uint64_t began,asr_began;void *audio;struct glv_memory *memory;struct audio_chunk *head,*tail;size_t samples,buffered;float peak;int cancel_reason;unsigned tool_maximum;bool voice,asr_failed,assets_checked,guarded;struct glv_storage_stamp stamp;};
extern int glv_platform_storage_stamp(struct glv_storage_stamp *) __attribute__((weak));
extern int glv_platform_session_budget(size_t,size_t *,size_t *) __attribute__((weak));
extern void glv_platform_memory_begin(void) __attribute__((weak));
extern void glv_platform_memory_end(void) __attribute__((weak));
extern int glv_platform_memory_pressure(void) __attribute__((weak));
static bool same_stamp(const struct glv_storage_stamp *a,const struct glv_storage_stamp *b)
{return a->writes==b->writes&&a->media==b->media;}
static int storage_stamp(struct glv_storage_stamp *s)
{return glv_platform_storage_stamp?glv_platform_storage_stamp(s):-ENOTSUP;}
/* Only the permanent owner worker touches kept objects or their allocator.
 * Callbacks are detached before idle, so no allocation retains a run pointer. */
static void session_drop(void)
{
  struct retained_llm *k=&service->kept;
  glv_llm_close(k->llm);k->llm=NULL;glv_asset_lease_close(k->lease);k->lease=NULL;
  glv_verified_clear(&k->memory);k->memory.cancelled=NULL;k->memory.opaque=NULL;k->memory.llm_progress=NULL;
  pthread_mutex_lock(&lock);service->state.session_resident=false;service->state.session_bytes=0;
  service->state.revision++;pthread_mutex_unlock(&lock);
}
static uint64_t ms(void)
{struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
/* All diagnostic state follows the same request lock as cancellation. */
static void phase_locked(enum glv_phase next)
{
  uint64_t now=ms();enum glv_phase old=service->state.phase;
  if(service->state.busy&&service->phase_began&&old<GLV_PHASE_COUNT)
    service->state.stage_ms[old]+=(unsigned)(now-service->phase_began);
  service->phase_began=now;service->state.phase=next;
}
static void cancel_cause_locked(enum glv_cancel_source source)
{
#ifdef GLASS_LOCAL_CONTROL
  atomic_store(&service->home_cancel,true);
#endif
  if(service->state.cancel_source!=GLV_CANCEL_NONE)return;
  if(source<=GLV_CANCEL_NONE||source>GLV_CANCEL_AUDIO)source=GLV_CANCEL_API;
  service->state.cancel_source=source;service->state.cancel_phase=service->state.phase;
  service->state.cancel_ms=(unsigned)(ms()-service->began);service->state.revision++;
}
static void reset_diagnostics_locked(void)
{
  service->state.cancel_source=GLV_CANCEL_NONE;service->state.cancel_phase=GLV_IDLE;
  service->state.cancel_ms=service->state.progress_done=service->state.progress_total=0;
  service->state.model_rounds=service->state.review_rounds=service->state.tool_rounds=0;
  service->state.asr_streaming=service->state.capture_active=false;service->state.asr_decoded_ms=service->state.asr_backlog_ms=0;
  service->state.q4_matrix_calls=service->state.q4_dot_calls=0;
  memset(service->state.stage_ms,0,sizeof(service->state.stage_ms));service->phase_began=ms();
  memset(&service->state.profile,0,sizeof(service->state.profile));
}
static bool cancelled(void *opaque)
{
  struct run *r=opaque;struct glv_storage_stamp now;int changed_stamp=0;
  if(r->guarded){changed_stamp=storage_stamp(&now);if(!changed_stamp&&!same_stamp(&now,&r->stamp))changed_stamp=-ESTALE;}
  pthread_mutex_lock(&lock);
  bool value=!service||service->state.request!=r->request||service->cancel==r->request;
  bool validated=r->assets_checked||(service&&(service->state.models_ready||service->state.asr_ready));
  if(!value&&validated&&!glv_platform_storage_ready()){r->cancel_reason=-ENODEV;cancel_cause_locked(GLV_CANCEL_STORAGE);value=true;}
  if(!value&&changed_stamp){r->cancel_reason=changed_stamp==-ENODEV?-ENODEV:-ESTALE;cancel_cause_locked(GLV_CANCEL_STORAGE);value=true;}
  pthread_mutex_unlock(&lock);return value;
}
/* Caller holds lock. Never publish an old result after a new-session clear. */
static bool publishable_locked(const struct run *r)
{return service&&service->state.request==r->request&&service->cancel!=r->request&&!service->clear_history;}
/* A generated byte token can end inside a UTF-8 sequence. Publish only complete
 * code points while streaming, replacing malformed final bytes like ByteLevel. */
static void display_text(char *out,size_t cap,const char *text,bool final)
{
  const unsigned char *p=(const unsigned char *)text,*end=p+strlen(text);size_t n=0;
  while(p<end){const unsigned char *begin=p;uint32_t cp;int ret=glv_utf8_read(&p,end,&cp);
    if(ret){p=begin+1;if(!final&&(*begin>=0xc2&&*begin<=0xf4)&&end-begin<4)break;cp=0xfffd;}
    unsigned char bytes[4];unsigned k=glv_utf8_write(cp,bytes);if(n+k>=cap)break;
    memcpy(out+n,bytes,k);n+=k;
  }if(cap)out[n]=0;
}
static void phase(struct run *r,enum glv_phase p)
{pthread_mutex_lock(&lock);if(publishable_locked(r)){phase_locked(p);if(r->memory){service->state.memory_peak=r->memory->peak;service->state.profile=r->memory->profile;}service->state.revision++;}pthread_mutex_unlock(&lock);}
static void llm_progress(void *opaque,enum glv_llm_progress stage,unsigned done,unsigned total)
{
  struct run *r=opaque;if(cancelled(r))return;
  enum glv_phase next=stage==GLV_LLM_TOKENIZING?GLV_TOKENIZING:
    stage==GLV_LLM_PREFILL?GLV_PREFILL:GLV_GENERATING;
  pthread_mutex_lock(&lock);
  if(publishable_locked(r)){
    if(service->state.phase!=next)phase_locked(next);
    service->state.progress_done=done;service->state.progress_total=total;
    if(r->memory)service->state.memory_peak=r->memory->peak;
    if(r->memory){service->state.q4_matrix_calls=r->memory->q4_matrix_calls;service->state.q4_dot_calls=r->memory->q4_dot_calls;service->state.profile=r->memory->profile;}
    service->state.revision++;
  }
  pthread_mutex_unlock(&lock);
}
static int text_sink(void *opaque,const char *text,unsigned tokens)
{
  struct run *r=opaque;if(cancelled(r))return -ECANCELED;
  pthread_mutex_lock(&lock);if(!publishable_locked(r)){pthread_mutex_unlock(&lock);return -ECANCELED;}
  display_text(service->reply,sizeof(service->reply),text,false);
  service->state.tokens=tokens;if(!service->state.first_token_ms)service->state.first_token_ms=(unsigned)(ms()-r->began)+1;
  service->state.revision++;pthread_mutex_unlock(&lock);return 0;
}
static int audio_sink(void *opaque,const float *samples,unsigned count,unsigned rate)
{
  struct run *r=opaque;if(rate!=24000)return -EINVAL;
  if(cancelled(r))return -ECANCELED;
  for(unsigned i=0;i<count;i++){if(!glv_finite(samples[i]))return -EDOM;r->peak=fmaxf(r->peak,fabsf(samples[i]));}
  if(count>24000u*120u-r->samples)return -E2BIG;
  if(!count)return 0;
  uint32_t digest=r->samples?r->memory->profile.tts_wave_fnv:2166136261u;
  const unsigned char *raw=(const unsigned char *)samples;
  for(size_t i=0;i<(size_t)count*sizeof(float);i++)digest=(digest^raw[i])*16777619u;
  r->memory->profile.tts_wave_fnv=digest;
  struct audio_chunk *chunk=glv_alloc(r->memory,sizeof(*chunk)+(size_t)count*sizeof(float));
  if(!chunk)return -ENOMEM;chunk->count=count;memcpy(chunk->samples,samples,(size_t)count*sizeof(float));
  if(r->tail)r->tail->next=chunk;else r->head=chunk;r->tail=chunk;
  r->samples+=count;r->buffered+=count;r->memory->profile.tts_samples=(unsigned)r->samples;
  if(r->buffered>r->memory->profile.tts_peak_buffered_samples)r->memory->profile.tts_peak_buffered_samples=(unsigned)r->buffered;
  return 0;
}
static int play_buffer(struct run *r)
{
  if(cancelled(r))return -ECANCELED;
  if(!r->head||!r->samples)return -ENODATA;
  int ret=glv_platform_audio_open(&r->audio,cancelled,r);if(ret)return ret;
  float gain=r->peak>.98f?.98f/r->peak:1.0f;int16_t pcm[240];size_t submitted=0;
  for(struct audio_chunk *chunk=r->head;chunk;chunk=chunk->next)for(unsigned at=0;at<chunk->count;){
    if(cancelled(r))return -ECANCELED;unsigned n=chunk->count-at>240?240:chunk->count-at;
    for(unsigned i=0;i<n;i++)pcm[i]=(int16_t)(fmaxf(-1,fminf(1,chunk->samples[at+i]*gain))*32767);
    if((ret=glv_platform_audio_write(r->audio,pcm,n)))return ret;
    if(!submitted){pthread_mutex_lock(&lock);if(publishable_locked(r)&&!service->state.first_audio_ms){service->state.first_audio_ms=(unsigned)(ms()-r->began)+1;service->state.revision++;}pthread_mutex_unlock(&lock);}
    at+=n;submitted+=n;
  }return 0;
}
static void drop_audio(struct run *r)
{
  while(r->head){struct audio_chunk *next=r->head->next;glv_free(r->head);r->head=next;}
  r->tail=NULL;r->buffered=0;r->peak=0;
}
static void clean_input(char *text)
{
  static const char *special[]={"<|pad|>","<|unk|>","<|beginoftext|>","<|im_start|>","<|im_end|>"};
  for(unsigned i=0;i<5;i++){char *p;while((p=strstr(text,special[i])))memmove(p,p+strlen(special[i]),strlen(p+strlen(special[i]))+1);}
}
static int build_prompt(struct glv_llm *llm,char *prompt,size_t cap,const char *input,bool control)
{
  int ids[512];unsigned skip=0;
  for(;;){size_t used=0;prompt[0]=0;
    for(unsigned i=control?service->history_count:skip;i<service->history_count;i++){size_t n=strlen(service->history[i]);
      if(n>=cap-used)return -E2BIG;memcpy(prompt+used,service->history[i],n);used+=n;}
    int n=snprintf(prompt+used,cap-used,"<|im_start|>user\n%s<|im_end|>\n<|im_start|>assistant\n",input);
    if(n<0||(size_t)n>=cap-used)return -E2BIG;
    int ret=glv_llm_encode(llm,prompt,ids,control?416:160);if(ret>=0)return 0;
    if(ret!=-E2BIG||control||skip==service->history_count)return ret;skip++;
  }
}
static void history_add(const char *input,const char *reply)
{
  if(service->history_count==HISTORY){memmove(service->history,service->history+1,sizeof(service->history[0])*(HISTORY-1));service->history_count--;}
  snprintf(service->history[service->history_count++],sizeof(service->history[0]),
    "<|im_start|>user\n%s<|im_end|>\n<|im_start|>assistant\n%s<|im_end|>\n",input,reply);
}
static int run_dialogue_impl(struct run *r,const char *input,struct glv_memory *job_memory)
{
  struct retained_llm *k=&service->kept;struct glv_storage_stamp current={0};
  const char *model_root=r->control?GLV_RELEASE_ROOT:GLV_CHAT_ROOT;
  int supported=storage_stamp(&current);unsigned backend;
  pthread_mutex_lock(&lock);backend=service->state.q4_bits;pthread_mutex_unlock(&lock);
  bool reuse=k->llm&&k->control_model==r->control&&supported==0&&backend==k->backend&&same_stamp(&current,&k->stamp)&&!glv_asset_lease_validate(k->lease);
  if(!reuse)session_drop();
  struct glv_memory *memory=&k->memory;r->memory=memory;
  memory->limit=job_memory->limit;
  if(job_memory->used>GLV_MEMORY_LIMIT||memory->limit>GLV_MEMORY_LIMIT-job_memory->used)
    memory->limit=job_memory->used>GLV_MEMORY_LIMIT?0:GLV_MEMORY_LIMIT-job_memory->used;
  memory->peak=memory->used;memory->cancelled=cancelled;memory->opaque=r;memory->llm_progress=llm_progress;
  memory->q4_bits=backend;memory->q4_matrix_calls=memory->q4_dot_calls=0;memory->reuse_kv=true;
  memset(&memory->profile,0,sizeof(memory->profile));memory->profile.session_reuses=reuse;
  memory->profile.llm_resident_bytes=glv_llm_resident_bytes(k->llm);
  memory->profile.llm_kv_bytes=glv_llm_kv_bytes(k->llm);
  int ret=0;
  if(!reuse){
    /* Re-read after retirement, which may close filesystem state. A missing
     * native guard keeps the original cold path; never grants a SHA exemption. */
    supported=storage_stamp(&current);
    if(supported&&supported!=-ENOTSUP){ret=supported==-ENODEV?-ENODEV:-ESTALE;goto done;}
  }
  r->guarded=supported==0;r->stamp=current;
  if(!reuse){
    if(r->guarded&&(ret=glv_asset_lease_open(model_root,memory,&k->lease)))goto done;
    memory->retain_verified=true;char failed[96];phase(r,GLV_CHECKING);
    ret=glv_assets_check_group(model_root,GLV_ASSET_LLM,memory,failed,sizeof(failed));
    pthread_mutex_lock(&lock);service->state.models_ready=!ret;
    if(ret)snprintf(service->state.detail,sizeof(service->state.detail),"%s",failed[0]?failed:"manifest.bin / SD");pthread_mutex_unlock(&lock);
    if(ret)goto done;r->assets_checked=true;
    phase(r,GLV_LOADING_LLM);char model_path[384];snprintf(model_path,sizeof(model_path),"%s/edgeidiot",model_root);
    ret=glv_llm_open(model_path,memory,&k->llm);if(ret)goto done;
    if(r->guarded&&(ret=glv_asset_lease_validate(k->lease)))goto done;
    k->stamp=current;k->backend=backend;k->control_model=r->control;
  }else r->assets_checked=true;
  pthread_mutex_lock(&lock);service->state.resident_control_model=r->control;service->state.session_resident=true;service->state.session_bytes=memory->used;service->state.revision++;pthread_mutex_unlock(&lock);
  char *prompt=glv_alloc(memory,8193),*answer=glv_alloc(memory,GLV_MAX_REPLY);
  if(!prompt||!answer)ret=-ENOMEM;
  if(!ret){phase(r,GLV_TOKENIZING);ret=build_prompt(k->llm,prompt,8193,input,r->control);}
  if(!ret){
    pthread_mutex_lock(&lock);service->state.model_rounds++;service->state.revision++;pthread_mutex_unlock(&lock);
    ret=r->control?glv_llm_generate_tool(k->llm,prompt,r->tool_maximum?r->tool_maximum:96,answer,GLV_MAX_REPLY,text_sink,r):glv_llm_generate(k->llm,prompt,96,answer,GLV_MAX_REPLY,text_sink,r);if(ret>=0)ret=0;
  }
  if(!ret){char safe[GLV_MAX_REPLY];display_text(safe,sizeof(safe),answer,true);memcpy(answer,safe,strlen(safe)+1);
    if(!answer[0])ret=-ENODATA;
    pthread_mutex_lock(&lock);if(publishable_locked(r)){snprintf(service->reply,sizeof(service->reply),"%s",answer);service->state.revision++;}else ret=-ECANCELED;pthread_mutex_unlock(&lock);
    if(!ret&&!r->control&&!cancelled(r))history_add(input,answer);
  }
  glv_free(prompt);glv_free(answer);
  if(!ret&&cancelled(r))ret=-ECANCELED;
done:
  glv_verified_clear(memory);r->guarded=false;
  if(ret||supported!=0)session_drop();
  memory->cancelled=NULL;memory->opaque=NULL;memory->llm_progress=NULL;
  pthread_mutex_lock(&lock);service->state.session_bytes=memory->used;pthread_mutex_unlock(&lock);
  return ret;
}
static int run_dialogue(struct run *r,const char *input,struct glv_memory *memory)
{
#ifdef __NuttX__
  int admission=glass_music_offline_suspend(3000);if(admission)return admission;
#endif
  int ret=run_dialogue_impl(r,input,memory);
#ifdef __NuttX__
  glass_music_offline_resume();
#endif
  return ret;
}
#ifdef GLASS_LOCAL_CONTROL
static int run_control(struct run *r,const char *query,struct glv_memory *memory)
{
 cJSON *settings=portal_config_get("localcontrol");const cJSON *value=cJSON_GetObjectItemCaseSensitive(settings,"catalog");
 struct glc_catalog catalog={0};int ret=cJSON_IsString(value)?glc_open(&catalog,value->valuestring):-ENOENT;cJSON_Delete(settings);
 if(ret){pthread_mutex_lock(&lock);snprintf(service->reply,sizeof(service->reply),"尚未注册有效的设备清单，未操作设备。");service->state.revision++;pthread_mutex_unlock(&lock);return ret;}
 char target[32];bool write,on;
 ret=glc_request(&catalog,query,target,sizeof(target),&write,&on);
 if(ret){pthread_mutex_lock(&lock);snprintf(service->reply,sizeof(service->reply),"请明确一盏灯或清单中的显式灯组，以及打开、关闭或查询状态。条件和多动作指令暂不执行。");service->state.revision++;pthread_mutex_unlock(&lock);glc_close(&catalog);return ret;}
 struct glc_transport transport={.revision=glass_board_home_revision(NULL),.current_revision=glass_board_home_revision,.call=glass_board_home_local_checked,.cancel=&service->home_cancel};
 char *body=glv_alloc(memory,INPUT_MAX+1);if(!body){glc_close(&catalog);return -ENOMEM;}
 ret=glc_prompt(&catalog,query,body,INPUT_MAX+1);r->control=true;r->tool_maximum=64;
 if(!ret)ret=run_dialogue(r,body,memory);glv_free(body);
 if(!ret){
  r->control_generated=true;
  char *buffers=glv_alloc(memory,GLV_MAX_REPLY*2u);
  if(!buffers){glc_close(&catalog);return -ENOMEM;}
  char *proposed=buffers,*result=buffers+GLV_MAX_REPLY;pthread_mutex_lock(&lock);snprintf(proposed,GLV_MAX_REPLY,"%s",service->reply);pthread_mutex_unlock(&lock);
  if(cancelled(r))ret=-ECANCELED;
  else{
   phase(r,GLV_CONTROLLING);
   ret=glc_execute(&catalog,query,proposed,&transport,result,GLV_MAX_REPLY);
   if(!ret){pthread_mutex_lock(&lock);service->state.tool_rounds++;pthread_mutex_unlock(&lock);}
   if(cancelled(r))ret=-ECANCELED;
   pthread_mutex_lock(&lock);
   if(publishable_locked(r)){
    if(!ret)snprintf(service->reply,sizeof(service->reply),"%s\n执行回执：%s",proposed,result);
    else if(ret!=-ENOMSG)snprintf(service->reply,sizeof(service->reply),"%s%s",result[0]?result:"模型调用与清单或请求不符，未继续操作设备。",ret==-EREMOTEIO?" 已停止，不自动重发。":"");
    service->state.revision++;
   }else if(!ret)ret=-ECANCELED;
   pthread_mutex_unlock(&lock);
   if(ret==-ENOMSG)ret=0;
  }
  /* A verified transaction or a normal model reply is terminal.
   * The selected thinking count is a ceiling, never a target. */
  glv_free(buffers);
 }
 glc_close(&catalog);return ret;
}
#endif
#ifndef GLV_TOOL_BENCH_ROOT
#define GLV_TOOL_BENCH_ROOT "/sdcard/local-voice/models/edgeidiot-9m9-w8a8-9000-20261102"
#endif
static int run_tool_benchmark(struct run *r,const char *input,struct glv_memory *memory)
{
  if(!input||input[0]<'0'||input[0]>'2'||input[1])return -EINVAL;
  r->memory=memory;
  pthread_mutex_lock(&lock);
  memory->q4_bits=service->state.q4_bits;
  bool admitted=!memory->q4_bits||service->state.q4_selftest_passed;
  pthread_mutex_unlock(&lock);
  if(!admitted)return -EAGAIN;
  struct glv_asset_lease *lease=NULL;struct glv_llm *llm=NULL;
  void *raw=NULL;size_t bytes=0;char *prompt=NULL,*answer=NULL;
  struct glv_storage_stamp stamp={0};int guard=storage_stamp(&stamp),ret=0;
  if(guard&&guard!=-ENOTSUP)return guard;
  r->guarded=!guard;r->stamp=stamp;
  if(r->guarded&&(ret=glv_asset_lease_open(GLV_TOOL_BENCH_ROOT,memory,&lease)))goto done;
  memory->retain_verified=true;char failed[96];phase(r,GLV_CHECKING);
  ret=glv_assets_check_group(GLV_TOOL_BENCH_ROOT,GLV_ASSET_LLM,memory,failed,sizeof(failed));
  if(ret)goto done;
  r->assets_checked=true;
  char name[24];snprintf(name,sizeof(name),"prompts/%c.txt",input[0]);
  ret=glv_asset_read_small(GLV_TOOL_BENCH_ROOT,name,8192,memory,&raw,&bytes);
  if(ret)goto done;
  if(memchr(raw,0,bytes)){ret=-EBADMSG;goto done;}
  prompt=glv_alloc(memory,bytes+1);answer=glv_alloc(memory,GLV_MAX_REPLY);
  if(!prompt||!answer){ret=-ENOMEM;goto done;}
  memcpy(prompt,raw,bytes);prompt[bytes]=0;glv_free(raw);raw=NULL;
  phase(r,GLV_LOADING_LLM);
  ret=glv_llm_open(GLV_TOOL_BENCH_ROOT "/edgeidiot",memory,&llm);
  if(ret)goto done;
  if(lease&&(ret=glv_asset_lease_validate(lease)))goto done;
  ret=glv_llm_generate_tool(llm,prompt,64,answer,GLV_MAX_REPLY,text_sink,r);
  if(ret>=0){
    ret=cancelled(r)?-ECANCELED:0;
    pthread_mutex_lock(&lock);
    if(!ret&&publishable_locked(r))snprintf(service->reply,sizeof(service->reply),"%s",answer);
    else if(!ret)ret=-ECANCELED;
    pthread_mutex_unlock(&lock);
  }
done:
  glv_llm_close(llm);glv_free(raw);glv_free(prompt);glv_free(answer);
  glv_asset_lease_close(lease);glv_verified_clear(memory);r->guarded=false;
  return ret;
}
/* Same manifest-bound prompt/maximum as tool-bench, using the normal
 * owner-held resident session. History is deliberately absent: identical
 * cold/warm diagnostic inputs, no execution, no inferred observations. */
static int run_cached_tool_benchmark(struct run *r,const char *input,struct glv_memory *memory)
{
 if(!input||input[0]<'0'||input[0]>'2'||input[1])return -EINVAL;
 char name[24];snprintf(name,sizeof(name),"prompts/%c.txt",input[0]);
 void *raw=NULL;size_t bytes=0;
 int ret=glv_asset_read_small(GLV_RELEASE_ROOT,name,8192,memory,&raw,&bytes);
 if(ret)return ret;
 const char prefix[]="<|im_start|>user\n",suffix[]="<|im_end|>\n<|im_start|>assistant\n";
 size_t before=sizeof(prefix)-1,after=sizeof(suffix)-1;
 if(bytes<before+after||memcmp(raw,prefix,before)||memcmp((char *)raw+bytes-after,suffix,after)||memchr(raw,0,bytes)){ret=-EBADMSG;goto done;}
 if(bytes-before-after>INPUT_MAX){ret=-E2BIG;goto done;}
 ((char *)raw)[bytes-after]=0;
 pthread_mutex_lock(&lock);service->history_count=0;memset(service->history,0,sizeof(service->history));pthread_mutex_unlock(&lock);
 r->control=true;r->tool_maximum=64;ret=run_dialogue(r,(char *)raw+before,memory);
 pthread_mutex_lock(&lock);service->history_count=0;memset(service->history,0,sizeof(service->history));pthread_mutex_unlock(&lock);
done:glv_free(raw);return ret;
}
static int run_models(struct run *r,unsigned job,enum glv_mode mode,const char *input,struct glv_memory *memory)
{
  if(job!=2&&mode==GLV_DIALOGUE)return run_dialogue(r,input,memory);
  r->memory=memory;
  pthread_mutex_lock(&lock);memory->tts_single_dot=service->state.tts_single_dot;memory->tts_serial_fp=service->state.tts_serial_fp;memory->tts_fma=service->state.tts_fma;memory->tts_bulk_rows=service->state.tts_bulk_rows;memory->tts_window_plan=service->state.tts_window_plan;memory->tts_parallel_input=service->state.tts_parallel_input;memory->tts_parallel_pie=service->state.tts_parallel_pie;memory->tts_weight_lut=service->state.tts_weight_lut;memory->tts_int8=job==9&&service->state.tts_int8;memory->tts_int8_reduce=job==9&&service->state.tts_int8&&service->state.tts_int8_reduce;memory->tts_quant_fast=service->state.tts_quant_fast;memory->tts_short=service->state.tts_short;memory->tts_upsample_pair=service->state.tts_upsample_pair;memory->tts_coarse_profile=service->state.tts_coarse_profile;
  if(job==9&&service->state.tts_bench_limit_kib&&memory->limit>(size_t)service->state.tts_bench_limit_kib*1024u)memory->limit=(size_t)service->state.tts_bench_limit_kib*1024u;pthread_mutex_unlock(&lock);
  memory->retain_verified=job!=2;
  unsigned groups=job==2?GLV_ASSET_ALL:mode==GLV_DIALOGUE?GLV_ASSET_LLM:GLV_ASSET_TTS;
  char failed[96];phase(r,GLV_CHECKING);int ret=glv_assets_check_group(GLV_RELEASE_ROOT,groups,memory,failed,sizeof(failed));
  if(!ret&&job==2)ret=glv_assets_check_group(GLV_CHAT_ROOT,GLV_ASSET_LLM,memory,failed,sizeof(failed));
  pthread_mutex_lock(&lock);if(groups&GLV_ASSET_LLM)service->state.models_ready=!ret;
  if(!ret)r->assets_checked=true;
  if(job==2&&!ret)service->state.wake_error=0;
  if(ret)snprintf(service->state.detail,sizeof(service->state.detail),"%s",failed[0]?failed:"manifest.bin / SD");
  pthread_mutex_unlock(&lock);if(ret||job==2)return ret;
  char *answer=glv_alloc(memory,GLV_MAX_REPLY);if(!answer)return -ENOMEM;
  {
    snprintf(answer,GLV_MAX_REPLY,"%s",input);
    pthread_mutex_lock(&lock);if(publishable_locked(r)){
      display_text(service->reply,sizeof(service->reply),answer,true);service->state.revision++;
    }else ret=-ECANCELED;pthread_mutex_unlock(&lock);
  }
  struct glv_phone_plan *plan=NULL;struct glv_tts *tts=NULL;
  if(!ret&&cancelled(r))ret=-ECANCELED;
  if(!ret&&!answer[0])ret=-ENODATA;
  if(!ret){phase(r,GLV_FRONTEND);ret=glv_frontend_plan(GLV_RELEASE_ROOT,answer,memory,&plan);}
  if(!ret)memory->profile.tts_planned_clauses=plan->count;
  if(!ret){phase(r,GLV_LOADING_TTS);ret=glv_tts_open(GLV_RELEASE_ROOT "/tts-v2",memory,&tts);}
  if(!ret){phase(r,GLV_SYNTHESIZING);for(unsigned i=0;i<plan->count&&!ret;i++){
    phase(r,GLV_SYNTHESIZING);
    if(i){float silence[240]={0};for(unsigned n=0;n<3456&&!ret;n+=240)ret=audio_sink(r,silence,3456-n>240?240:3456-n,24000);}
    if(!ret)ret=glv_tts_phones(tts,plan->parts[i].ids,plan->parts[i].kind,plan->parts[i].count,i==0,audio_sink,r);
    if(!ret){
      memory->profile.tts_clauses++;
      if(!memory->profile.tts_first_clause_ms)memory->profile.tts_first_clause_ms=(unsigned)(ms()-r->began)+1;
    }
    if(memory->tts_short){
      phase(r,GLV_SYNTHESIZING);
      if(!ret&&job!=9){phase(r,GLV_SPEAKING);ret=play_buffer(r);}
      if(job!=9){int closed=glv_platform_audio_finish(r->audio,!ret&&!cancelled(r));r->audio=NULL;if(!ret)ret=closed;}
      drop_audio(r);
    }
  }}
  glv_tts_close(tts);glv_free(plan);glv_free(answer);
  if(!ret)memory->profile.tts_ready_ms=(unsigned)(ms()-r->began)+1;
  if(!ret&&job!=9&&!memory->tts_short){phase(r,GLV_SPEAKING);ret=play_buffer(r);}
  int closed=job==9?0:glv_platform_audio_finish(r->audio,!ret&&!cancelled(r));r->audio=NULL;
  drop_audio(r);
  return ret?ret:closed;
}
static int save_target(bool local,bool enabled)
{
  char current[2]={0,'1'};FILE *f=fopen(GLV_CONFIG_PATH,"rb");if(f){fread(current,1,2,f);fclose(f);}
  char value[2]={local?'1':'0',enabled?'1':'0'};if(!memcmp(current,value,2))return 0;
  int ret=glass_ai_config_begin(6000);if(ret)return ret;
#ifdef __NuttX__
  if(mkdir("/data/config",0700)&&errno!=EEXIST){ret=-errno;glass_ai_config_end();return ret;}
#endif
  const char *tmp=GLV_CONFIG_PATH ".tmp";int fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC,0600);
  if(fd<0)ret=-errno;else {if(write(fd,value,2)!=2||fsync(fd))ret=-EIO;
    if(close(fd)&&!ret)ret=-EIO;if(!ret&&rename(tmp,GLV_CONFIG_PATH))ret=-errno;}
  if(ret)unlink(tmp);glass_ai_config_end();return ret;
}
static int save_session(struct run *r)
{
  struct stat st;if(lstat(GLV_RELEASE_ROOT,&st)||!S_ISDIR(st.st_mode)||S_ISLNK(st.st_mode))return -ENODEV;
  if(!service->history_count)return -ENODATA;
  /* Only this explicit action creates content. Never select internal storage. */
  const char *folder="/sdcard/local-voice/sessions";
#ifdef GLV_SESSION_ROOT
  folder=GLV_SESSION_ROOT;
#endif
  if(service->mode!=GLV_DIALOGUE)return -ENODATA;
  if(mkdir(folder,0700)&&errno!=EEXIST)return -errno;
  if(lstat(folder,&st)||!S_ISDIR(st.st_mode)||S_ISLNK(st.st_mode))return -EINVAL;
  char path[384];if(snprintf(path,sizeof(path),"%s/session-%lu-%lu.txt",folder,(unsigned long)time(NULL),(unsigned long)r->request)>=(int)sizeof(path))return -ENAMETOOLONG;
  int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0)return -errno;int ret=0;
  for(unsigned i=0;i<service->history_count&&!ret;i++){
    const char *p=service->history[i];size_t n=strlen(p);
    while(n&&!ret){if(cancelled(r)){ret=-ECANCELED;break;}ssize_t wrote=write(fd,p,n);if(wrote<=0){ret=-EIO;break;}p+=wrote;n-=wrote;}
  }
  if(!ret&&fsync(fd))ret=-EIO;if(close(fd)&&!ret)ret=-EIO;if(ret)unlink(path);
  if(!ret){pthread_mutex_lock(&lock);snprintf(service->state.detail,sizeof(service->state.detail),"会话已保存到 SD 卡");pthread_mutex_unlock(&lock);}return ret;
}
#ifdef GLV_ASR_EVALUATION
static int asr_token(void *opaque,unsigned token,unsigned frame)
{
  (void)token;(void)frame;struct run *r=opaque;if(cancelled(r))return -ECANCELED;
  pthread_mutex_lock(&lock);
  if(!publishable_locked(r)){pthread_mutex_unlock(&lock);return -ECANCELED;}
  if(r->voice){service->state.asr_tokens++;if(!service->state.asr_first_token_ms)service->state.asr_first_token_ms=(unsigned)(ms()-r->asr_began)+1;}
  else {service->state.tokens++;if(!service->state.first_token_ms)service->state.first_token_ms=(unsigned)(ms()-r->began)+1;}
  if(r->memory)service->state.memory_peak=r->memory->peak;
  service->state.revision++;pthread_mutex_unlock(&lock);return 0;
}
static void asr_progress(void *opaque,unsigned stage)
{phase(opaque,stage==1?GLV_LOADING_ASR:GLV_TRANSCRIBING);}
static int run_asr_test(struct run *r,const char *input,struct glv_memory *memory)
{
  r->memory=memory;
  if(input[0]<'0'||input[0]>'4'||input[1])return -EINVAL;
#ifdef GLV_ASR_ACCEL_ENABLED
  if(input[0]=='2')return glv_asr_accel_probe(memory);
  if(input[0]=='3'||input[0]=='4')memory->asr_no_pie=true;
  if(input[0]=='3')memory->asr_serial=true;
#endif
  char *text=glv_alloc(memory,GLV_MAX_REPLY);if(!text)return -ENOMEM;
  int ret=glv_asr_test_file(input[0]=='1'?1u:0u,memory,text,GLV_MAX_REPLY,asr_token,r,asr_progress);
  if(!ret&&cancelled(r))ret=-ECANCELED;
  if(!ret){pthread_mutex_lock(&lock);
    if(publishable_locked(r)){
      snprintf(service->reply,sizeof(service->reply),"%s",text);
      snprintf(service->state.detail,sizeof(service->state.detail),"离线样本转写完成 · 麦克风入口待验证");service->state.revision++;
    }else ret=-ECANCELED;pthread_mutex_unlock(&lock);}
  glv_free(text);return ret;
}
#endif
#ifdef GLV_ASR_MICROPHONE
static bool capture_finished(void *opaque)
{struct run *r=opaque;pthread_mutex_lock(&lock);bool value=service->finish_capture==r->request;pthread_mutex_unlock(&lock);return value;}
static void capture_level(void *opaque,unsigned level,unsigned elapsed)
{
  struct run *r=opaque;pthread_mutex_lock(&lock);
  if(publishable_locked(r)){
    service->state.input_level=level;service->state.capture_ms=elapsed;service->state.memory_peak=r->memory->peak;service->state.revision++;
  }pthread_mutex_unlock(&lock);
}
#ifdef GLV_ASR_STREAMING
struct live_asr {struct run *run;struct glv_asr *asr;};
static void live_level(void *opaque,unsigned level,unsigned elapsed)
{
 struct run *r=opaque;pthread_mutex_lock(&lock);
 if(publishable_locked(r)){service->state.input_level=level;service->state.capture_ms=elapsed;service->state.revision++;}
 pthread_mutex_unlock(&lock);
}
static void live_progress(void *opaque,bool active,unsigned captured,unsigned queued)
{
 struct live_asr *live=opaque;struct run *r=live->run;pthread_mutex_lock(&lock);
 if(publishable_locked(r)){
  service->state.capture_active=active;service->state.capture_pending=active;service->state.capture_ms=captured;
  unsigned decoded=service->state.asr_decoded_ms;
  service->state.asr_backlog_ms=captured>decoded?captured-decoded:queued;service->state.revision++;
 }
 pthread_mutex_unlock(&lock);
}
static int live_partial(void *opaque,const char *text,unsigned tokens,unsigned frames)
{
 struct run *r=opaque;int ret=0;pthread_mutex_lock(&lock);
 if(publishable_locked(r)){
  display_text(service->transcript,sizeof(service->transcript),text,false);service->transcript_request=r->request;
  service->state.asr_tokens=tokens;service->state.memory_peak=r->memory->peak;if(tokens&&!service->state.asr_first_token_ms)service->state.asr_first_token_ms=(unsigned)(ms()-r->asr_began)+1;
  unsigned decoded=frames*40u;if(decoded>service->state.capture_ms)decoded=service->state.capture_ms;
  service->state.asr_decoded_ms=decoded;service->state.asr_backlog_ms=service->state.capture_ms-decoded;
  service->state.asr_elapsed_ms=(unsigned)(ms()-r->asr_began);service->state.revision++;
 }else ret=-ECANCELED;
 pthread_mutex_unlock(&lock);return ret;
}
static int live_consume(void *opaque,const int16_t *pcm,size_t samples)
{struct live_asr *live=opaque;return glv_asr_stream_accept(live->asr,pcm,samples);}
static int run_microphone_live(struct run *r,struct glv_memory *memory)
{
 r->memory=memory;r->voice=true;r->asr_began=ms();phase(r,GLV_CHECKING_ASR);
 int ret=glv_asr_assets_check(memory);if(!ret)ret=glv_cancel(memory);
 pthread_mutex_lock(&lock);service->state.asr_ready=!ret;service->state.asr_error=ret;if(!ret)r->assets_checked=true;pthread_mutex_unlock(&lock);
 if(ret){r->asr_failed=true;return ret;}
 struct glv_asr *asr=NULL;char *text=glv_alloc(memory,INPUT_MAX+1);if(!text)return -ENOMEM;
 phase(r,GLV_LOADING_ASR);ret=glv_asr_open(&asr,GLV_ASR_RELEASE_ROOT,memory);
 if(!ret)ret=glv_asr_prepare_partial(asr);
 if(!ret)ret=glv_asr_stream_begin(asr,live_partial,r);
 if(!ret){
  struct live_asr live={r,asr};struct glv_capture_signals signals={capture_finished,live_level,r};
  pthread_mutex_lock(&lock);service->state.asr_streaming=true;service->state.capture_active=false;service->state.capture_pending=true;service->state.asr_decoded_ms=service->state.asr_backlog_ms=0;service->state.revision++;pthread_mutex_unlock(&lock);
  phase(r,GLV_RECORDING);
  ret=glv_capture_platform_stream(memory,&signals,NULL,live_consume,&live,live_progress);
  if(!ret){phase(r,GLV_TRANSCRIBING);ret=glv_asr_stream_finish(asr,text,INPUT_MAX+1,NULL);}
 }
 if(ret&&asr)glv_asr_stream_abort(asr);
 glv_asr_close(asr); /* Helper and microphone are retired before any LLM/tool. */
 if(ret==-ENOSPC)ret=-E2BIG;
 pthread_mutex_lock(&lock);
 service->state.asr_streaming=service->state.capture_active=service->state.capture_pending=false;service->state.input_level=0;
 service->state.asr_error=ret;service->state.asr_elapsed_ms=(unsigned)(ms()-r->asr_began);r->asr_failed=ret!=0;
 if(ret==-ENOBUFS)snprintf(service->state.detail,sizeof(service->state.detail),"识别跟不上录音，12秒缓冲已满，已停止；没有丢弃音频后继续执行。");
 else if(ret==-ENODATA)snprintf(service->state.detail,sizeof(service->state.detail),"未听到有效语音，请重试");
 service->state.revision++;pthread_mutex_unlock(&lock);
 if(!ret&&cancelled(r))ret=-ECANCELED;
 if(!ret){clean_input(text);if(!text[0])ret=-ENODATA;}
 pthread_mutex_lock(&lock);service->state.asr_error=ret;r->asr_failed=ret!=0;pthread_mutex_unlock(&lock);
 if(!ret){pthread_mutex_lock(&lock);if(publishable_locked(r)){snprintf(service->transcript,sizeof(service->transcript),"%s",text);service->transcript_request=r->request;service->state.revision++;}else ret=-ECANCELED;pthread_mutex_unlock(&lock);}
 /* Provisional text never reaches this final-only responder branch. */
 if(!ret){
#ifdef GLASS_LOCAL_CONTROL
  if(r->control)ret=run_control(r,text,memory);else
#endif
  ret=run_models(r,1,GLV_DIALOGUE,text,memory);
 }
 glv_free(text);return ret;
}
/* Recorder files use the same permanent ASR owner and cancellation barrier. */
static int file_partial(void *opaque,const char *text,unsigned tokens,unsigned frames)
{
 struct run *r=opaque;int ret=0;pthread_mutex_lock(&lock);
 if(!publishable_locked(r))ret=-ECANCELED;
 else {
  service->state.asr_tokens=tokens;service->state.memory_peak=r->memory->peak;
  if(tokens&&!service->state.asr_first_token_ms)service->state.asr_first_token_ms=(unsigned)(ms()-r->asr_began)+1;
  unsigned decoded=frames*40u;if(decoded>service->file.duration_ms)decoded=service->file.duration_ms;
  service->state.asr_decoded_ms=decoded;service->state.asr_backlog_ms=service->file.duration_ms-decoded;
  service->state.asr_elapsed_ms=(unsigned)(ms()-r->asr_began);service->state.revision++;
  size_t before=strlen(service->file.text),after=strlen(text);
  if(after>=sizeof(service->file.text)||(after>before&&service->file.count>=GLV_FILE_MARKS))ret=-E2BIG;
  else if(after>before){
   unsigned at=frames?(frames-1)*40u:0;if(at>service->file.duration_ms)at=service->file.duration_ms;
   service->file.marks[service->file.count++]=(struct glv_file_mark){at,(uint16_t)after};
   memcpy(service->file.text,text,after+1);
  }
 }
 pthread_mutex_unlock(&lock);return ret;
}
static int run_recording_file(struct run *r,const char *path,struct glv_memory *memory)
{
 r->memory=memory;r->voice=true;r->asr_began=ms();phase(r,GLV_CHECKING_ASR);
 FILE *f=NULL;size_t size=0;struct glv_asr *asr=NULL;int16_t *pcm=NULL;char *text=NULL;
 bool checked=false;int ret=glv_open_asset(path,&f,&size);unsigned char h[44];unsigned bytes=0;
 if(!ret&&(size<44||glv_read_exact(f,h,44)))ret=-EBADMSG;
 if(!ret){
  bytes=glv_u32(h+40);
  if(memcmp(h,"RIFF",4)||memcmp(h+8,"WAVEfmt ",8)||memcmp(h+36,"data",4)||
     glv_u32(h+4)!=size-8||glv_u32(h+16)!=16||h[20]!=1||h[21]||h[22]!=1||h[23]||
     glv_u32(h+24)!=16000||h[32]!=2||h[33]||h[34]!=16||h[35]||
     !bytes||(bytes&1u)||bytes!=size-44)ret=-EINVAL;
  if(!ret&&bytes>16000u*2u*600u)ret=-E2BIG;
 }
 if(!ret){pthread_mutex_lock(&lock);memset(&service->file,0,sizeof(service->file));
  service->file_request=r->request;service->file.duration_ms=bytes/32u;
  service->state.capture_ms=service->file.duration_ms;pthread_mutex_unlock(&lock);
  ret=glv_asr_assets_check(memory);checked=true;}
 if(!ret)ret=glv_cancel(memory);
 pthread_mutex_lock(&lock);if(checked)service->state.asr_ready=!ret;service->state.asr_error=ret;pthread_mutex_unlock(&lock);
 if(!ret){r->assets_checked=true;phase(r,GLV_LOADING_ASR);ret=glv_asr_open(&asr,GLV_ASR_RELEASE_ROOT,memory);}
 if(!ret)ret=glv_asr_prepare_partial(asr);
 if(!ret){pcm=glv_alloc(memory,3200);text=glv_alloc(memory,GLV_MAX_REPLY);if(!pcm||!text)ret=-ENOMEM;}
 if(!ret){pthread_mutex_lock(&lock);service->state.asr_streaming=true;service->state.revision++;pthread_mutex_unlock(&lock);
  phase(r,GLV_TRANSCRIBING);ret=glv_asr_stream_begin(asr,file_partial,r);}
 for(unsigned at=0;at<bytes&&!ret;){
  ret=glv_cancel(memory);unsigned n=bytes-at;if(n>3200)n=3200;
  if(!ret)ret=glv_read_exact(f,pcm,n);
  if(!ret)ret=glv_asr_stream_accept(asr,pcm,n/2u);at+=n;
 }
 if(!ret)ret=glv_asr_stream_finish(asr,text,GLV_MAX_REPLY,NULL);
 if(!ret&&(!text[0]||cancelled(r)))ret=text[0]?-ECANCELED:-ENODATA;
 if(ret&&asr)glv_asr_stream_abort(asr);
 glv_asr_close(asr);if(f)fclose(f);glv_free(pcm);
 pthread_mutex_lock(&lock);
 service->state.asr_streaming=false;service->state.asr_error=ret;service->state.asr_elapsed_ms=(unsigned)(ms()-r->asr_began);
 if(!ret&&publishable_locked(r))snprintf(service->state.detail,sizeof(service->state.detail),"录音转文字完成");
 r->asr_failed=ret!=0;service->state.revision++;pthread_mutex_unlock(&lock);
 glv_free(text);return ret;
}
#endif
static int run_microphone(struct run *r,struct glv_memory *memory)
{
#ifdef GLV_ASR_STREAMING
  return run_microphone_live(r,memory);
#else
  r->memory=memory;r->voice=true;
  phase(r,GLV_CHECKING_ASR);int checked=glv_asr_assets_check(memory);
  if(!checked)checked=glv_cancel(memory);
  pthread_mutex_lock(&lock);service->state.asr_ready=!checked;service->state.asr_error=checked;if(!checked)r->assets_checked=true;pthread_mutex_unlock(&lock);
  if(checked){r->asr_failed=true;return checked;}
  struct glv_capture_signals signals={capture_finished,capture_level,r};
  int16_t *pcm=NULL;size_t samples=0;phase(r,GLV_RECORDING);
  int ret=glv_capture_platform(memory,&signals,NULL,&pcm,&samples);
  pthread_mutex_lock(&lock);service->state.capture_pending=false;service->state.revision++;pthread_mutex_unlock(&lock);
  if(ret){pthread_mutex_lock(&lock);snprintf(service->state.detail,sizeof(service->state.detail),"%s",ret==-ENODATA?"未听到有效语音，请重试":ret==-E2BIG?"单次录音最多 60 秒，请缩短后重试":"麦克风采集未完成");pthread_mutex_unlock(&lock);return ret;}
  char *text=glv_alloc(memory,INPUT_MAX+1);if(!text){glv_free(pcm);return -ENOMEM;}
  r->asr_began=ms();phase(r,GLV_CHECKING);
  ret=glv_asr_transcribe_pcm(pcm,samples,memory,text,INPUT_MAX+1,asr_token,r,asr_progress);
  if(ret==-ENOSPC)ret=-E2BIG;
  glv_free(pcm); /* DMA retired by capture; discard PCM before LLM allocation. */
  pthread_mutex_lock(&lock);service->state.asr_elapsed_ms=(unsigned)(ms()-r->asr_began);
  service->state.input_level=0;service->state.asr_error=ret;r->asr_failed=ret!=0;pthread_mutex_unlock(&lock);
  if(!ret&&cancelled(r))ret=-ECANCELED;
  if(!ret){clean_input(text);if(!text[0])ret=-ENODATA;}
  if(!ret){pthread_mutex_lock(&lock);
    if(publishable_locked(r)){
      snprintf(service->transcript,sizeof(service->transcript),"%s",text);service->transcript_request=r->request;service->state.revision++;
    }else ret=-ECANCELED;pthread_mutex_unlock(&lock);}
  /* glv_asr_transcribe_pcm has closed all ASR graphs/cache before this call.
   * The pre-existing LLM -> release -> V2 sequence remains the only responder. */
  if(!ret){
#ifdef GLASS_LOCAL_CONTROL
    if(r->control)ret=run_control(r,text,memory);else
#endif
    ret=run_models(r,1,GLV_DIALOGUE,text,memory);
  }
  glv_free(text);return ret;
#endif
}
#endif
/* Budget admission runs only in the owning worker, before model/tool work.
 * A retained cache must not turn recoverable memory pressure into a failed
 * request. Retire it, keep textual history, and recompute the cold budget.
 */
static int request_memory_budget(struct glv_memory *memory,size_t *largest)
{
  int ret=service->kept.llm&&glv_platform_session_budget?
    glv_platform_session_budget(service->kept.memory.used,&memory->limit,largest):
    glv_platform_budget(&memory->limit,largest);
  if(!ret&&service->kept.llm&&glv_platform_session_budget&&
     (memory->limit<=service->kept.memory.used||memory->limit-service->kept.memory.used<256u*1024u)){
    glv_llm_trim_kv(service->kept.llm);
    ret=glv_platform_session_budget(service->kept.memory.used,&memory->limit,largest);
  }
  if(ret==-ENOMEM&&service->kept.llm&&glv_platform_session_budget){
    glv_llm_trim_kv(service->kept.llm);
    ret=glv_platform_session_budget(service->kept.memory.used,&memory->limit,largest);
  }
  if(ret==-ENOMEM&&service->kept.llm){
    session_drop();
    ret=glv_platform_budget(&memory->limit,largest);
  }
  return ret;
}
static void *worker(void *unused)
{
  service->owner=pthread_self();
#if defined(__NuttX__) && defined(CONFIG_SMP) && CONFIG_SMP_NCPUS > 1
  /* Keep the offline AI owner on the otherwise idle core. Peripheral IRQs
   * and the existing GUI owner retain their current CPU routing.
   */
  cpu_set_t cpus;
  CPU_ZERO(&cpus);CPU_SET(1,&cpus);
  int affinity=sched_setaffinity(0,sizeof(cpus),&cpus);
  printf("LOCAL_VOICE_CPU requested=1 affinity=%d\n",affinity);
#endif
  (void)unused;FILE *f=fopen(GLV_CONFIG_PATH,"rb");int target=f?fgetc(f):'0',enabled=f?fgetc(f):'1';if(f)fclose(f);
  if(target=='1')printf("[local-voice] legacy global target ignored; original assistant retained\n");
  pthread_mutex_lock(&lock);service->state.local_wake=false; /* Global wake belongs to the original assistant. */service->state.wake_enabled=enabled=='1';service->state.config_pending=false;service->state.revision++;pthread_mutex_unlock(&lock);
  for(;;){
    pthread_mutex_lock(&lock);
    while(!service->job&&!service->retire_session){
      struct timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_nsec+=500000000;
      if(until.tv_nsec>=1000000000){until.tv_sec++;until.tv_nsec-=1000000000;}
      int waited=pthread_cond_timedwait(&changed,&lock,&until);
      if(waited==ETIMEDOUT&&service->kept.llm&&!service->job){
        pthread_mutex_unlock(&lock);
        /* Only this owner mutates the resident model. UI/audio request
         * retirement through the mailbox, never by freeing another thread. */
        if(glv_platform_memory_pressure&&glv_platform_memory_pressure()){
          glv_llm_trim_kv(service->kept.llm);
          pthread_mutex_lock(&lock);
          service->state.session_bytes=service->kept.memory.used;
          service->state.profile.llm_kv_bytes=0;service->state.revision++;
          pthread_mutex_unlock(&lock);
          if(glv_platform_memory_pressure()>1)session_drop();
        }
        struct glv_storage_stamp current;
        int check=service->kept.llm?storage_stamp(&current):0;
        if(service->kept.llm&&!check&&!same_stamp(&current,&service->kept.stamp))check=-ESTALE;
        if(service->kept.llm&&!check)check=glv_asset_lease_validate(service->kept.lease);
        pthread_mutex_lock(&lock);
        if(check){service->retire_session=true;service->state.session_pending=true;
          service->state.models_ready=false;service->state.model_error=check==-ENODEV?-ENODEV:0;service->state.revision++;}
      }
    }
    unsigned job=service->job?service->job:8;enum glv_mode mode=service->mode;bool desired=service->desired_wake,desired_enabled=service->desired_enabled;
    if(job==8)service->state.busy=true;
    struct run r={.control=job==12||job==13||job==11||job==14,.request=service->state.request,.began=ms()};service->job=0;
    if(job!=3&&job!=8){service->began=r.began;service->phase_began=r.began;}
    bool clear=service->clear_history;service->clear_history=false;
    bool retire=service->retire_session||clear||(job!=1&&job!=11&&job!=12&&job!=14)||(mode!=GLV_DIALOGUE&&mode!=GLV_CONTROL);
    service->retire_session=false;
    if(clear){clear_selected_history_locked();}
    char input[INPUT_MAX+1];memcpy(input,service->input,sizeof(input));pthread_mutex_unlock(&lock);
    if(retire)session_drop();
    pthread_mutex_lock(&lock);service->state.session_pending=service->retire_session;pthread_mutex_unlock(&lock);
    if(job==8){
      pthread_mutex_lock(&lock);
      if(service->clear_history){clear_selected_history_locked();service->clear_history=false;}
      service->retire_session=false;service->state.session_pending=false;service->state.busy=false;service->state.revision++;
      if(service->state.phase==GLV_STOPPING)phase_locked(GLV_IDLE);
      pthread_mutex_unlock(&lock);continue;
    }
    int ret=0;struct glv_memory memory={.limit=GLV_MEMORY_LIMIT,.cancelled=cancelled,.opaque=&r,.llm_progress=llm_progress};size_t largest=0;
    bool memory_phase=job!=3&&job!=4;
    if(memory_phase&&glv_platform_memory_begin)glv_platform_memory_begin();
    if(job==3){ret=save_target(desired,desired_enabled);pthread_mutex_lock(&lock);if(!ret){service->state.local_wake=desired;service->state.wake_enabled=desired_enabled;service->state.wake_error=0;}
      pthread_mutex_unlock(&lock);}
    else if(job==4)ret=save_session(&r);
    else {ret=request_memory_budget(&memory,&largest);if(!ret){
#ifdef GLASS_LOCAL_CONTROL
      if(job==14)ret=run_cached_tool_benchmark(&r,input,&memory);else if(job==12)ret=run_control(&r,input,&memory);else
#ifdef GLV_ASR_MICROPHONE
      if(job==13)ret=run_microphone(&r,&memory);else
#endif
#endif
      if(job==11)ret=run_cached_tool_benchmark(&r,input,&memory);else
#ifdef GLV_Q4_PIE
      if(job==7){ret=glv_q4_selftest(&memory);pthread_mutex_lock(&lock);service->state.q4_selftest_passed=!ret;if(ret)service->state.q4_bits=0;pthread_mutex_unlock(&lock);}else
      if(job==10)ret=run_tool_benchmark(&r,input,&memory);else
#endif
#ifdef GLV_ASR_EVALUATION
#if defined(GLV_ASR_STREAMING) && defined(GLV_ASR_MICROPHONE)
      if(job==15)ret=run_recording_file(&r,input,&memory);else
#endif
      if(job==5)ret=run_asr_test(&r,input,&memory);else
#endif
#ifdef GLV_ASR_MICROPHONE
      if(job==6)ret=run_microphone(&r,&memory);else
#endif
      ret=run_models(&r,job,mode,input,&memory);}}
#ifdef GLV_ASR_MICROPHONE
    if(job==2){int asr=ret;if(!ret){phase(&r,GLV_CHECKING_ASR);asr=glv_asr_assets_check(&memory);}if(!asr)asr=glv_cancel(&memory);
      pthread_mutex_lock(&lock);service->state.asr_ready=!asr;service->state.asr_error=asr;pthread_mutex_unlock(&lock);
      /* A missing/corrupt ASR package leaves text features available. A stop
       * or media fault must still terminate this check with its real result. */
      if(!ret&&(asr==-ECANCELED||asr==-EIO||asr==-ENODEV))ret=asr;}
#endif
    glv_verified_clear(&memory);
    if(ret==-ECANCELED&&r.cancel_reason)ret=r.cancel_reason;
    if(ret)session_drop();
    if(memory_phase&&glv_platform_memory_end)glv_platform_memory_end();
    struct glv_memory *reported=r.memory?r.memory:&memory;
    pthread_mutex_lock(&lock);
    if(service->clear_history){clear_selected_history_locked();service->clear_history=false;service->reply[0]=service->transcript[0]=0;service->transcript_request=0;}
    if(ret==-ECANCELED)cancel_cause_locked(GLV_CANCEL_UNATTRIBUTED);
    if(!ret&&!service->clear_history&&(job==1||job==6||job==12||job==13))snprintf(service->replies[mode],GLV_MAX_REPLY,"%s",service->reply);
    service->state.error=ret;phase_locked(ret&&ret!=-ECANCELED?GLV_FAILED:GLV_IDLE);
    bool llm_job=job==2||(job==1&&mode==GLV_DIALOGUE)||job==6||job==11||job==12||job==13;
    if(llm_job)service->state.model_error=r.control_generated||(job==12&&!r.memory)?0:ret;
    /* A failed SD read invalidates readiness too: background wake must wait
     * for a successful package check instead of silently rearming. */
    if(job!=3&&!r.control_generated&&(ret==-EIO||ret==-ENODEV||ret==-ESTALE||(ret==-EBADMSG&&llm_job&&!r.asr_failed))){service->state.models_ready=false;service->state.model_error=ret;}
    if(ret==-EIO||ret==-ENODEV||((r.asr_failed||job==5)&&(ret==-EBADMSG||ret==-ENOENT))){service->state.asr_ready=false;service->state.asr_error=ret;}
    service->state.input_level=0;service->state.capture_pending=false;
    service->state.q4_matrix_calls=reported->q4_matrix_calls;service->state.q4_dot_calls=reported->q4_dot_calls;
    service->state.profile=reported->profile;
    if(job!=3){service->state.elapsed_ms=(unsigned)(ms()-r.began);service->state.memory_peak=reported->peak>memory.peak?reported->peak:memory.peak;}
    if(job==3)service->state.config_pending=false;
    service->state.busy=service->retire_session;service->state.session_pending=service->retire_session;
    service->state.session_bytes=service->kept.memory.used;service->state.revision++;
    if(memory.used){service->state.error=-EFAULT;service->state.phase=GLV_FAILED;}
    enum glv_cancel_source cause=service->state.cancel_source;
    enum glv_phase cancelled_phase=service->state.cancel_phase;unsigned cancelled_ms=service->state.cancel_ms;
    pthread_mutex_unlock(&lock);
    if(ret)printf("[local-voice] request=%lu result=%d cancel_source=%u cancel_phase=%u cancel_ms=%u\n",
      (unsigned long)r.request,ret,cause,cancelled_phase,cancelled_ms);
  }return NULL;
}
int glass_local_voice_start(void)
{
  pthread_mutex_lock(&lock);if(service||starting){int ret=service?0:-EBUSY;pthread_mutex_unlock(&lock);return ret;}starting=true;pthread_mutex_unlock(&lock);
  struct local_service *s=calloc(1,sizeof(*s));if(!s){pthread_mutex_lock(&lock);starting=false;pthread_mutex_unlock(&lock);return -ENOMEM;}
#ifdef GLASS_LOCAL_CONTROL
  atomic_init(&s->home_cancel,false);
#endif
#ifdef GLV_TTS_PIE
  s->state.tts_window_plan=true;
  s->state.tts_quant_fast=true;s->state.tts_coarse_profile=true;s->state.tts_short=true;
#endif
#if defined(GLV_TTS_PIE) && defined(GLV_TTS_PARALLEL)
  s->state.tts_parallel_input=true;
  s->state.tts_parallel_pie=true;
#endif
  s->state.thinking_rounds=5;s->state.local_wake=false;s->state.config_pending=true;s->began=s->phase_began=ms();
  pthread_mutex_lock(&lock);service=s;pthread_mutex_unlock(&lock);
  pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,32768);pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
  pthread_t thread;int ret=pthread_create(&thread,&attr,worker,NULL);pthread_attr_destroy(&attr);
  pthread_mutex_lock(&lock);starting=false;if(ret){service=NULL;free(s);}pthread_mutex_unlock(&lock);return -ret;
}
static int submit(unsigned job,const char *text,enum glv_mode mode,uint32_t *request)
{
  if(mode!=GLV_DIALOGUE&&mode!=GLV_READ&&mode!=GLV_CONTROL)return -EINVAL;
  if((job==1||job==5||job==9||job==12||job==14||job==15)&&(!text||!text[0]||strnlen(text,INPUT_MAX+1)>INPUT_MAX))return -EINVAL;
  if(!glass_ai_start_enter())return -EBUSY;
  pthread_mutex_lock(&lock);int ret=0;
  if(!service)ret=-ENODEV;
  else if(service->state.busy||service->state.config_pending||service->retire_session)ret=-EBUSY;
  else {
    struct glv_snapshot *s=&service->state;s->request=++service->next;if(!s->request)s->request=++service->next;
    s->busy=true;s->phase=GLV_CHECKING;s->error=0;s->tokens=s->first_token_ms=s->first_audio_ms=s->elapsed_ms=0;s->memory_peak=0;s->detail[0]=0;service->began=ms();reset_diagnostics_locked();
    snprintf(service->input,sizeof(service->input),"%s",text?text:"");clean_input(service->input);
#ifdef GLASS_LOCAL_CONTROL
    atomic_store(&service->home_cancel,false);
#endif
    if(job==1||job==5||job==6||job==9||job==10||job==11||job==12||job==13){service->reply[0]=service->transcript[0]=0;service->transcript_request=0;service->finish_capture=0;
      if((job==1||job==9)&&mode==GLV_READ)display_text(service->reply,sizeof(service->reply),service->input,true);
      s->input_level=s->capture_ms=s->asr_tokens=s->asr_first_token_ms=s->asr_elapsed_ms=0;}
    if(job==15){s->input_level=s->capture_ms=s->asr_tokens=s->asr_first_token_ms=s->asr_elapsed_ms=0;}
    s->capture_pending=job==6||job==13;if(job==6||job==13||job==15)s->asr_error=0;
    service->state.selected_mode=mode;service->mode=mode;service->job=job;s->revision++;
    if(request)*request=s->request;pthread_cond_signal(&changed);
  }
  pthread_mutex_unlock(&lock);glass_ai_start_leave();if(!ret)glv_platform_wake_yield();return ret;
}
int glass_local_voice_transcribe_file(const char *path,uint32_t *request)
{
#if defined(GLV_ASR_STREAMING) && defined(GLV_ASR_MICROPHONE) && defined(GLV_ASR_EVALUATION)
 struct glv_snapshot s;glass_local_voice_get(&s,NULL,0);return submit(15,path,s.selected_mode,request);
#else
 (void)path;(void)request;return -ENOTSUP;
#endif
}
int glass_local_voice_file_result(uint32_t request,struct glv_file_transcript *out)
{
 if(!out)return -EINVAL;pthread_mutex_lock(&lock);int ret=-ESTALE;
 if(service&&service->state.request==request&&service->file_request==request){
  ret=service->state.busy?-EAGAIN:service->state.error;if(!ret)*out=service->file;
 }
 pthread_mutex_unlock(&lock);return ret;
}
int glass_local_voice_send(const char *text,enum glv_mode mode,uint32_t *request)
{
 if(mode==GLV_CONTROL){
#ifdef GLASS_LOCAL_CONTROL
  return submit(12,text,GLV_CONTROL,request);
#else
  return -ENOTSUP;
#endif
 }
 if(mode!=GLV_DIALOGUE&&mode!=GLV_READ)return -EINVAL;
 return submit(1,text,mode,request);
}
int glass_local_voice_listen_control(uint32_t *request)
{
#if defined(GLASS_LOCAL_CONTROL) && defined(GLV_ASR_MICROPHONE)
 return submit(13,NULL,GLV_CONTROL,request);
#else
 (void)request;return -ENOTSUP;
#endif
}
int glass_local_voice_tts_benchmark(const char *text,uint32_t *request){return submit(9,text,GLV_READ,request);}
int glass_local_voice_tool_benchmark(unsigned index,uint32_t *request)
{
  if(index>2)return -EINVAL;
  char text[2]={(char)('0'+index),0};return submit(10,text,GLV_DIALOGUE,request);
}
int glass_local_voice_tts_kernel(bool single)
{
  if(!glass_ai_start_enter())return -EBUSY;
  pthread_mutex_lock(&lock);int ret=0;
  if(!service)ret=-ENODEV;
  else if(service->state.busy||service->state.config_pending)ret=-EBUSY;
  else {service->state.tts_single_dot=single;service->state.revision++;}
  pthread_mutex_unlock(&lock);glass_ai_start_leave();return ret;
}
int glass_local_voice_tts_parallel(bool enabled)
{
#ifndef GLV_TTS_PARALLEL
  (void)enabled;return -ENOTSUP;
#else
  if(!glass_ai_start_enter())return -EBUSY;
  pthread_mutex_lock(&lock);int ret=0;
  if(!service)ret=-ENODEV;
  else if(service->state.busy||service->state.config_pending)ret=-EBUSY;
  else {service->state.tts_serial_fp=!enabled;service->state.revision++;}
  pthread_mutex_unlock(&lock);glass_ai_start_leave();return ret;
#endif
}
int glass_local_voice_tts_fma(bool enabled)
{
#ifndef GLV_TTS_FMA
  (void)enabled;return -ENOTSUP;
#else
  if(!glass_ai_start_enter())return -EBUSY;
  pthread_mutex_lock(&lock);int ret=0;
  if(!service)ret=-ENODEV;
  else if(service->state.busy||service->state.config_pending)ret=-EBUSY;
  else if(enabled&&service->state.tts_upsample_pair)ret=-EINVAL;
  else {service->state.tts_fma=enabled;service->state.revision++;}
  pthread_mutex_unlock(&lock);glass_ai_start_leave();return ret;
#endif
}
int glass_local_voice_tts_rows(bool bulk)
{
  if(!glass_ai_start_enter())return -EBUSY;
  pthread_mutex_lock(&lock);int ret=0;
  if(!service)ret=-ENODEV;
  else if(service->state.busy||service->state.config_pending)ret=-EBUSY;
  else {service->state.tts_bulk_rows=bulk;service->state.revision++;}
  pthread_mutex_unlock(&lock);glass_ai_start_leave();return ret;
}
int glass_local_voice_tts_input(bool enabled)
{
#if !defined(GLV_TTS_PIE) || !defined(GLV_TTS_PARALLEL)
  if(enabled)return -ENOTSUP;
#endif
  if(!glass_ai_start_enter())return -EBUSY;
  pthread_mutex_lock(&lock);int ret=0;
  if(!service)ret=-ENODEV;
  else if(service->state.busy||service->state.config_pending)ret=-EBUSY;
  else {service->state.tts_parallel_input=enabled;service->state.revision++;}
  pthread_mutex_unlock(&lock);glass_ai_start_leave();return ret;
}
int glass_local_voice_tts_weight_lut(bool enabled)
{
#ifndef GLV_TTS_PIE
  if(enabled)return -ENOTSUP;
#endif
  if(!glass_ai_start_enter())return -EBUSY;
  pthread_mutex_lock(&lock);int ret=0;
  if(!service)ret=-ENODEV;
  else if(service->state.busy||service->state.config_pending)ret=-EBUSY;
  else {service->state.tts_weight_lut=enabled;service->state.revision++;}
  pthread_mutex_unlock(&lock);glass_ai_start_leave();return ret;
}
int glass_local_voice_tts_int8(bool enabled)
{
#ifndef GLV_TTS_INT8
  if(enabled)return -ENOTSUP;
#endif
  if(!glass_ai_start_enter())return -EBUSY;
  pthread_mutex_lock(&lock);int ret=0;
  if(!service)ret=-ENODEV;
  else if(service->state.busy||service->state.config_pending)ret=-EBUSY;
  else {service->state.tts_int8=enabled;service->state.revision++;}
  pthread_mutex_unlock(&lock);glass_ai_start_leave();return ret;
}
int glass_local_voice_tts_int8_reduce(bool enabled)
{
#ifndef GLV_TTS_INT8
  if(enabled)return -ENOTSUP;
#endif
  if(!glass_ai_start_enter())return -EBUSY;
  pthread_mutex_lock(&lock);int ret=0;
  if(!service)ret=-ENODEV;
  else if(service->state.busy||service->state.config_pending)ret=-EBUSY;
  else {service->state.tts_int8_reduce=enabled;service->state.revision++;}
  pthread_mutex_unlock(&lock);glass_ai_start_leave();return ret;
}
int glass_local_voice_tts_quant_fast(bool enabled)
{
#ifndef GLV_TTS_PIE
  if(enabled)return -ENOTSUP;
#endif
  if(!glass_ai_start_enter())return -EBUSY;
  pthread_mutex_lock(&lock);int ret=0;
  if(!service)ret=-ENODEV;
  else if(service->state.busy||service->state.config_pending)ret=-EBUSY;
  else {service->state.tts_quant_fast=enabled;service->state.revision++;}
  pthread_mutex_unlock(&lock);glass_ai_start_leave();return ret;
}
int glass_local_voice_tts_short(bool enabled)
{
#ifndef GLV_TTS_PIE
  if(enabled)return -ENOTSUP;
#endif
  if(!glass_ai_start_enter())return -EBUSY;
  pthread_mutex_lock(&lock);int ret=0;
  if(!service)ret=-ENODEV;
  else if(service->state.busy||service->state.config_pending)ret=-EBUSY;
  else {service->state.tts_short=enabled;service->state.revision++;}
  pthread_mutex_unlock(&lock);glass_ai_start_leave();return ret;
}
int glass_local_voice_tts_upsample_pair(bool enabled)
{
#ifndef GLV_TTS_PARALLEL
  if(enabled)return -ENOTSUP;
#endif
  if(!glass_ai_start_enter())return -EBUSY;
  pthread_mutex_lock(&lock);int ret=0;
  if(!service)ret=-ENODEV;
  else if(service->state.busy||service->state.config_pending)ret=-EBUSY;
  else if(enabled&&service->state.tts_fma)ret=-EINVAL;
  else {service->state.tts_upsample_pair=enabled;service->state.revision++;}
  pthread_mutex_unlock(&lock);glass_ai_start_leave();return ret;
}
int glass_local_voice_tts_pie_parallel(bool enabled)
{
#if !defined(GLV_TTS_PIE) || !defined(GLV_TTS_PARALLEL)
  if(enabled)return -ENOTSUP;
#endif
  if(!glass_ai_start_enter())return -EBUSY;
  pthread_mutex_lock(&lock);int ret=0;
  if(!service)ret=-ENODEV;
  else if(service->state.busy||service->state.config_pending)ret=-EBUSY;
  else {service->state.tts_parallel_pie=enabled;service->state.revision++;}
  pthread_mutex_unlock(&lock);glass_ai_start_leave();return ret;
}
int glass_local_voice_tts_profile(bool coarse)
{
#ifndef GLV_TTS_PIE
  if(coarse)return -ENOTSUP;
#endif
  if(!glass_ai_start_enter())return -EBUSY;
  pthread_mutex_lock(&lock);int ret=0;
  if(!service)ret=-ENODEV;
  else if(service->state.busy||service->state.config_pending)ret=-EBUSY;
  else {service->state.tts_coarse_profile=coarse;service->state.revision++;}
  pthread_mutex_unlock(&lock);glass_ai_start_leave();return ret;
}
int glass_local_voice_tts_windows(bool planned)
{
#ifndef GLV_TTS_PIE
  if(planned)return -ENOTSUP;
#endif
  if(!glass_ai_start_enter())return -EBUSY;
  pthread_mutex_lock(&lock);int ret=0;
  if(!service)ret=-ENODEV;
  else if(service->state.busy||service->state.config_pending)ret=-EBUSY;
  else {service->state.tts_window_plan=planned;service->state.revision++;}
  pthread_mutex_unlock(&lock);glass_ai_start_leave();return ret;
}
int glass_local_voice_tts_bench_limit(unsigned kib)
{
  if(kib&&kib!=4096)return -EINVAL;
  if(!glass_ai_start_enter())return -EBUSY;
  pthread_mutex_lock(&lock);int ret=0;
  if(!service)ret=-ENODEV;
  else if(service->state.busy||service->state.config_pending)ret=-EBUSY;
  else {service->state.tts_bench_limit_kib=kib;service->state.revision++;}
  pthread_mutex_unlock(&lock);glass_ai_start_leave();return ret;
}
int glass_local_voice_tool_cached_benchmark(unsigned index,uint32_t *request)
{if(index>2)return -EINVAL;char text[2]={(char)('0'+index),0};return submit(11,text,GLV_DIALOGUE,request);}
int glass_local_voice_select(enum glv_mode mode)
{
 if(mode!=GLV_DIALOGUE&&mode!=GLV_CONTROL&&mode!=GLV_READ)return -EINVAL;
 if(!glass_ai_start_enter())return -EBUSY;
 pthread_mutex_lock(&lock);int ret=service?0:-ENODEV;
 if(service&&(service->state.busy||service->state.config_pending))ret=-EBUSY;
 if(!ret){service->mode=mode;service->state.selected_mode=mode;snprintf(service->reply,sizeof(service->reply),"%s",service->replies[mode]);service->state.revision++;}
 pthread_mutex_unlock(&lock);glass_ai_start_leave();return ret;
}
int glass_local_voice_thinking(unsigned rounds)
{
 if(rounds<4||rounds>6)return -EINVAL;
 if(!glass_ai_start_enter())return -EBUSY;
 pthread_mutex_lock(&lock);int ret=service?0:-ENODEV;
 if(service&&(service->state.busy||service->state.config_pending))ret=-EBUSY;
 if(!ret&&service->state.thinking_rounds!=rounds){service->state.thinking_rounds=rounds;service->state.revision++;}
 pthread_mutex_unlock(&lock);glass_ai_start_leave();return ret;
}
int glass_local_voice_thinking_benchmark(unsigned index,uint32_t *request)
{
#ifdef GLASS_LOCAL_CONTROL
 if(index>2)return -EINVAL;char text[2]={(char)('0'+index),0};return submit(14,text,GLV_CONTROL,request);
#else
 (void)index;(void)request;return -ENOTSUP;
#endif
}
int glass_local_voice_check(uint32_t *request){return submit(2,NULL,GLV_READ,request);}
int glass_local_voice_save(uint32_t *request){return submit(4,NULL,GLV_READ,request);}
int glass_local_voice_q4_selftest(uint32_t *request)
{
#ifdef GLV_Q4_PIE
  return submit(7,NULL,GLV_READ,request);
#else
  (void)request;return -ENOTSUP;
#endif
}
int glass_local_voice_q4_backend(unsigned bits)
{
  if(bits!=0&&bits!=16)return -EINVAL;
#ifndef GLV_Q4_PIE
  if(bits)return -ENOTSUP;
#endif
  if(!glass_ai_start_enter())return -EBUSY;
  pthread_mutex_lock(&lock);int ret=0;
  if(!service)ret=-ENODEV;
  else if(service->state.busy||service->state.config_pending)ret=-EBUSY;
  else if(bits&&!service->state.q4_selftest_passed)ret=-EAGAIN;
  else {if(service->state.q4_bits!=bits){service->state.q4_bits=bits;request_retire_locked();}service->state.revision++;}
  pthread_mutex_unlock(&lock);glass_ai_start_leave();return ret;
}
int glass_local_voice_listen(uint32_t *request)
{
#ifdef GLV_ASR_MICROPHONE
  return submit(6,NULL,GLV_DIALOGUE,request);
#else
  (void)request;return -ENOTSUP;
#endif
}
int glass_local_voice_finish_capture(uint32_t request)
{
  pthread_mutex_lock(&lock);int ret=-ENOENT;
  if(service&&service->state.busy&&service->state.phase==GLV_RECORDING&&(!service->state.asr_streaming||service->state.capture_active)&&service->state.request==request){service->finish_capture=request;ret=0;}
  pthread_mutex_unlock(&lock);return ret;
}
bool glass_local_voice_get_transcript(uint32_t request,char *text,size_t cap)
{if(!text||!cap)return false;pthread_mutex_lock(&lock);bool match=service&&!service->clear_history&&request&&service->state.request==request&&service->transcript_request==request;
  snprintf(text,cap,"%s",match?service->transcript:"");pthread_mutex_unlock(&lock);return match&&text[0];}
int glass_local_voice_asr_test(unsigned clip,uint32_t *request)
{
#ifdef GLV_ASR_EVALUATION
  if(clip>4)return -EINVAL;char text[2]={(char)('0'+clip),0};return submit(5,text,GLV_READ,request);
#else
  (void)clip;(void)request;return -ENOTSUP;
#endif
}
void glass_local_voice_stop_from(uint32_t id,enum glv_cancel_source source)
{
  pthread_mutex_lock(&lock);
  if(service&&id&&service->state.request==id&&service->state.busy){
    cancel_cause_locked(source);service->cancel=id;phase_locked(GLV_STOPPING);service->state.revision++;
  }
  pthread_mutex_unlock(&lock);
}
void glass_local_voice_stop(uint32_t id){glass_local_voice_stop_from(id,GLV_CANCEL_API);}
static void request_retire_locked(void)
{
  if(!service||(!service->state.session_resident&&!service->state.busy&&!service->state.session_pending))return;
  service->retire_session=true;service->state.session_pending=true;
  if(!service->state.busy&&!service->job&&!service->state.config_pending){service->job=8;service->state.busy=true;}
  service->state.revision++;pthread_cond_signal(&changed);
}
void glass_local_voice_release(void)
{pthread_mutex_lock(&lock);request_retire_locked();pthread_mutex_unlock(&lock);}
int glass_local_voice_yield_idle_model(unsigned timeout_ms)
{
  uint64_t began=ms();pthread_mutex_lock(&lock);
  if(!service||pthread_equal(service->owner,pthread_self())){pthread_mutex_unlock(&lock);return 0;}
  /* Input-method admission must never cancel an active AI/ASR request. */
  if(service->state.busy||service->state.config_pending){pthread_mutex_unlock(&lock);return -EBUSY;}
  request_retire_locked();pthread_mutex_unlock(&lock);
  for(;;){
    pthread_mutex_lock(&lock);bool done=!service||(!service->state.session_resident&&!service->state.session_pending);pthread_mutex_unlock(&lock);
    if(done)return 0;if(ms()-began>=timeout_ms)return -EBUSY;usleep(1000);
  }
}
int glass_local_voice_yield_model(unsigned timeout_ms)
{
  uint64_t began=ms();pthread_mutex_lock(&lock);
  bool need=service&&(service->state.session_resident||service->state.session_pending);
  if(!need||pthread_equal(service->owner,pthread_self())){pthread_mutex_unlock(&lock);return 0;}
  if(service->state.busy){cancel_cause_locked(GLV_CANCEL_AUDIO);service->cancel=service->state.request;phase_locked(GLV_STOPPING);}
  request_retire_locked();pthread_mutex_unlock(&lock);
  for(;;){
    pthread_mutex_lock(&lock);bool done=!service||(!service->state.session_resident&&!service->state.session_pending);pthread_mutex_unlock(&lock);
    if(done)return 0;if(ms()-began>=timeout_ms)return -EBUSY;usleep(1000);
  }
}
void glass_local_voice_cancel_from(enum glv_cancel_source source)
{
  pthread_mutex_lock(&lock);
  if(service&&service->state.busy){cancel_cause_locked(source);service->cancel=service->state.request;
    phase_locked(GLV_STOPPING);service->state.revision++;}
  request_retire_locked();
  pthread_mutex_unlock(&lock);
}
void glass_local_voice_cancel(void){glass_local_voice_cancel_from(GLV_CANCEL_API);}
int glass_local_voice_new_from(enum glv_cancel_source source)
{
  pthread_mutex_lock(&lock);int ret=service?0:-ENODEV;
  if(service){
    if(service->state.busy)cancel_cause_locked(source);
    service->cancel=service->state.request;service->clear_history=true;
    service->replies[service->mode][0]=0;service->reply[0]=service->transcript[0]=0;service->transcript_request=0;
    service->state.tokens=service->state.asr_tokens=service->state.input_level=0;
    if(service->state.busy)phase_locked(GLV_STOPPING);
    else {clear_selected_history_locked();service->clear_history=false;}
    service->state.revision++;
    request_retire_locked();
  }
  pthread_mutex_unlock(&lock);return ret;
}
int glass_local_voice_new(void){return glass_local_voice_new_from(GLV_CANCEL_API);}
bool glass_local_voice_config_idle(void)
{pthread_mutex_lock(&lock);bool idle=!service||(!service->state.busy&&!service->state.session_pending);pthread_mutex_unlock(&lock);return idle;}
void glass_local_voice_get(struct glv_snapshot *out,char *reply,size_t cap)
{pthread_mutex_lock(&lock);
  /* Readiness is a verified-model receipt, not a guarantee that the medium
   * still exists. This is an atomic platform flag; never probe storage here.
   * A returning card must be checked again by an explicit job. */
  if(service&&!service->state.busy&&!glv_platform_storage_ready()&&
     (service->state.models_ready||service->state.asr_ready)){
    service->state.models_ready=service->state.asr_ready=false;
    service->state.model_error=service->state.asr_error=-ENODEV;service->state.revision++;
  }
  if(out){if(service){*out=service->state;if(out->busy){uint64_t now=ms();out->elapsed_ms=(unsigned)(now-service->began);
    if(service->phase_began&&out->phase<GLV_PHASE_COUNT)out->stage_ms[out->phase]+=(unsigned)(now-service->phase_began);}}else memset(out,0,sizeof(*out));}
  if(reply&&cap)snprintf(reply,cap,"%s",service&&!service->clear_history?service->reply:"");pthread_mutex_unlock(&lock);}
bool glass_local_voice_wake_selected(void)
{pthread_mutex_lock(&lock);bool value=service&&service->state.local_wake;pthread_mutex_unlock(&lock);return value;}
bool glass_local_voice_wake_enabled(void)
{pthread_mutex_lock(&lock);bool value=service&&service->state.wake_enabled;pthread_mutex_unlock(&lock);return value;}
bool glass_local_voice_wake_ready(void)
{pthread_mutex_lock(&lock);bool value=service&&service->state.wake_enabled&&service->state.local_wake&&!service->state.wake_error&&!service->state.busy&&!service->state.config_pending;pthread_mutex_unlock(&lock);return value&&glv_platform_storage_ready();}
static int set_wake(bool target,bool value)
{
  if(!glass_ai_start_enter())return -EBUSY;pthread_mutex_lock(&lock);int ret=0;
  if(!service)ret=-ENODEV;else if(service->state.busy||service->state.config_pending)ret=-EBUSY;
  else if((target?service->state.local_wake:service->state.wake_enabled)!=value){
    service->desired_wake=target?value:service->state.local_wake;service->desired_enabled=target?service->state.wake_enabled:value;
    service->state.config_pending=true;service->job=3;service->state.revision++;pthread_cond_signal(&changed);}
  pthread_mutex_unlock(&lock);glass_ai_start_leave();return ret;
}
int glass_local_voice_set_wake(bool local){return local?-EPERM:set_wake(true,false);}
int glass_local_voice_enable_wake(bool enabled){return set_wake(false,enabled);}
const char *glass_local_voice_error(int e)
{
  switch(e){case 0:return "就绪";case -ECANCELED:return "已停止";case -ENOMEM:return "内存不足，已保留界面和音频余量";
  case -ENOENT:case -ENODEV:return "未找到 SD 卡或模型包";case -EBADMSG:return "模型损坏或运行时版本不兼容";
  case -EIO:return "SD 卡读取失败，请检查卡片";case -EBUSY:return "设备正忙，请稍后重试";
  case -ENOSPC:return "SD 卡空间不足，未能完成合成或保存";
  case -ESTALE:return "SD 内容或挂载已变化，请重新发送以校验模型";
  case -E2BIG:return "文本或上下文过长，请新建对话或缩短文本";case -ENOTSUP:return "V2 中文前端暂不支持这段文字";
  case -EPERM:return "全局唤醒固定使用原助手，本地语音仅在 APP 中使用";
  case -ENOBUFS:return "识别积压过大，录音已停止";
  case -ENODATA:return "未获得有效语音或文字";default:return "本地处理失败，已释放资源";}
}

/* Background wake owns no foreground request. Publish fatal model/media
 * errors without overwriting another request's result or timing. */
void glass_local_voice_wake_failure(int error)
{
  if(error!=-ENOENT&&error!=-ENODEV&&error!=-EIO&&error!=-EBADMSG)return;
  pthread_mutex_lock(&lock);
  if(service&&service->state.local_wake&&service->state.wake_enabled&&
     !service->state.busy&&!service->state.config_pending){
    service->state.wake_error=error;
    if(error==-EIO||error==-ENODEV){service->state.models_ready=service->state.asr_ready=false;service->state.model_error=error;service->state.asr_error=error;}
    service->state.revision++;
  }
  pthread_mutex_unlock(&lock);
}
