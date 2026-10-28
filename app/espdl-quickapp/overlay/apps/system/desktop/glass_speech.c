/* SPDX-License-Identifier: Apache-2.0 */
#include <nuttx/config.h>
#include "glass_speech.h"
#include "glass_ai_config.h"
#include "glass_voice.h"
#include "glass_speech_sr.h"
#include "glass_speech_tts.h"
#ifdef GLASS_ESP_TTS
#include "glass_esp_tts.h"
#endif
#include "glass_speech_level.h"
#include "glass_speech_gain.h"
#include "glass_wake_model.h"
#include "glass_custom_wake.h"
#include "glass_music.h"
#include "glass_music_pcm.h"
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define VOICE_RATE 16000
#define VOICE_BUFFERS 4
#define VOICE_BYTES 960
#define VOICE_FRAMES (VOICE_BYTES/4)
#define VOICE_SETTLE_FRAMES 1024
static int speech_audio_lock(void);
#define VOICE_CAPTURE_LOCK() speech_audio_lock()
static bool speech_cancelled(void *unused);
static bool speech_background_capture(void);
#define VOICE_CAPTURE_CANCELLED() speech_cancelled(NULL)
#define VOICE_CAPTURE_BACKGROUND() speech_background_capture()
#include "glass_voice_capture.inc"

static pthread_mutex_t speech_lock=PTHREAD_MUTEX_INITIALIZER;
static struct speech_snapshot speech;
static uint64_t speech_level_at;
static uint64_t speech_milliseconds(void)
{struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);return (uint64_t)now.tv_sec*1000+now.tv_nsec/1000000;}
static atomic_bool speech_cancel;
static int speech_audio_lock(void)
{
  uint64_t end=speech_milliseconds()+5000;
  while(!atomic_load(&speech_cancel)){
    int ret=music_pcm_session_trylock();if(ret!=-EBUSY)return ret;
    if(speech_background_capture())return -EBUSY;
    if(speech_milliseconds()>=end)return -ETIMEDOUT;
    usleep(10000);
  }
  return -ECANCELED;
}
struct speech_job;
static struct speech_job *speech_pending;
static bool speech_pending_cancelled;
static bool speech_background_capture(void)
{pthread_mutex_lock(&speech_lock);bool background=speech.background_wake;pthread_mutex_unlock(&speech_lock);return background;}
static atomic_uint speech_volume=75;
unsigned glass_speech_volume(void){return atomic_load(&speech_volume);}
static const struct speech_command default_commands[]={
  {1,"打开灯光","da kai deng guang"},{2,"关闭灯光","guan bi deng guang"},
  {3,"打开窗帘","da kai chuang lian"},{4,"关闭窗帘","guan bi chuang lian"},
  {5,"打开风扇","da kai feng shan"},{6,"关闭风扇","guan bi feng shan"},
  {7,"返回桌面","fan hui zhuo mian"},{8,"停止聆听","ting zhi ling ting"}};
/* Recognition is exposed as text/events. HA actions need an explicit mapping
 * to a real entity and are deliberately not inferred from a spoken room name. */
static void speech_publish(enum speech_phase phase,int error,const char *text,int command,unsigned level)
{
  pthread_mutex_lock(&speech_lock);speech.phase=phase;speech.error=error;
  if(text)snprintf(speech.text,sizeof(speech.text),"%s",text);
  speech.command=command;speech.level=level;if(!++speech.revision)speech.revision=1;
  if(command>0){speech.last_command=command;speech.command_ms=(uint32_t)speech_milliseconds();if(!++speech.command_event)speech.command_event=1;}
  pthread_mutex_unlock(&speech_lock);
}
void glass_speech_get(struct speech_snapshot *out)
{if(out){pthread_mutex_lock(&speech_lock);*out=speech;
  if(out->phase==SPEECH_SPEAKING&&speech_milliseconds()-speech_level_at>180)out->level=0;
  pthread_mutex_unlock(&speech_lock);}}
void glass_speech_capture_woke(void)
{
  pthread_mutex_lock(&speech_lock);
  if(!++speech.wake_event)speech.wake_event=1;
  speech.wake_ms=(uint32_t)speech_milliseconds();speech.revision++;
  pthread_mutex_unlock(&speech_lock);
}
static void speech_stop(uint32_t request)
{
  pthread_mutex_lock(&speech_lock);
  if(!request||speech.request==request){
    if(speech_pending)speech_pending_cancelled=true;
#ifndef GLASS_ESP_TTS
    glass_speech_clear_cache();
#endif
    if(speech.wake_ready){speech.wake_ready=false;speech.revision++;}
    if(speech.active){atomic_store(&speech_cancel,true);speech.phase=SPEECH_STOPPING;speech.revision++;}
  }
  pthread_mutex_unlock(&speech_lock);
}
void glass_speech_stop(void){speech_stop(0);}
bool glass_speech_yield_background(void)
{
  pthread_mutex_lock(&speech_lock);
  bool yielding=speech.active && speech.background_wake;
  if(yielding){
    atomic_store(&speech_cancel,true);
    speech.wake_ready=false;speech.phase=SPEECH_STOPPING;speech.revision++;
  }
  pthread_mutex_unlock(&speech_lock);
  return yielding;
}
void glass_speech_clear_cache(void)
{
#ifndef GLASS_ESP_TTS
  glass_sano_cache_clear();
#else
  glass_esp_tts_cache_clear();
#endif
}
void glass_speech_trim_cache(void)
{
#ifdef GLASS_ESP_TTS
  size_t freed=glass_esp_tts_cache_trim();
  if(freed)printf("[speech] idle_voice_cache_released=%u\n",(unsigned)freed);
#endif
}
#ifdef GLASS_ESP_TTS
static atomic_bool speech_prepared;
static void *speech_prepare_worker(void *unused)
{
  (void)unused;const unsigned char *data;void *owned;
  uint64_t started=speech_milliseconds();
  int ret=glass_esp_tts_voice_load(&data,&owned,NULL,NULL);
  if(!ret)glass_esp_tts_voice_release(owned);
  else atomic_store(&speech_prepared,false);
  printf("[speech] voice_preload result=%d elapsed_ms=%lu\n",ret,(unsigned long)(speech_milliseconds()-started));
  glass_ai_start_leave();
  return NULL;
}
#endif
void glass_speech_prepare(void)
{
#ifdef GLASS_ESP_TTS
  if(!glass_ai_start_enter())return;
  if(atomic_exchange(&speech_prepared,true)){glass_ai_start_leave();return;}
  pthread_attr_t attr;int ret=pthread_attr_init(&attr);
  if(!ret){
    ret=pthread_attr_setstacksize(&attr,16384);
    if(!ret)ret=pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
    if(!ret){pthread_t tid;ret=pthread_create(&tid,&attr,speech_prepare_worker,NULL);}
    pthread_attr_destroy(&attr);
  }
  if(ret){atomic_store(&speech_prepared,false);glass_ai_start_leave();}
#endif
}
const char *glass_speech_backend(void)
{
#ifdef GLASS_ESP_TTS
  return "esp-tts";
#else
  return "sanotts";
#endif
}
void glass_speech_stop_request(uint32_t request){if(request)speech_stop(request);}
static bool speech_cancelled(void *unused)
{(void)unused;return atomic_load(&speech_cancel);}
#include "glass_speech_barge.inc"
struct speech_play {struct music_pcm pcm;struct speech_barge *barge;bool opened,barge_attempted;unsigned rate,volume;uint64_t started,first_write;};
static int speech_write(void *opaque,const int16_t *data,size_t frames)
{
  struct speech_play *play=opaque;if(speech_cancelled(NULL))return -ECANCELED;
  if(!play->opened){int ret=music_pcm_try_open(&play->pcm,play->rate,play->volume);if(ret)return ret;play->opened=true;play->first_write=speech_milliseconds();
    printf("[speech] tts first_pcm_ms=%lu\n",(unsigned long)(speech_milliseconds()-play->started));}
  int ret=music_pcm_write(&play->pcm,data,frames,1);
  if(!ret&&play->pcm.started&&!play->barge_attempted){
    play->barge_attempted=true;play->barge=speech_barge_start(play->pcm.fd);
  }
  if(!ret){unsigned level=glass_speech_pcm_level(data,frames);
    pthread_mutex_lock(&speech_lock);speech.level=level;speech_level_at=speech_milliseconds();pthread_mutex_unlock(&speech_lock);}
  return ret;
}
struct speech_buffer {int16_t *data;size_t frames,capacity;};
static int speech_buffer_write(void *opaque,const int16_t *data,size_t frames)
{
  struct speech_buffer *buffer=opaque;
  const size_t limit=16000u*180u;
  if(speech_cancelled(NULL))return -ECANCELED;
  if(frames>limit-buffer->frames)return -EFBIG;
  size_t needed=buffer->frames+frames;
  if(needed>buffer->capacity){
    size_t capacity=buffer->capacity?buffer->capacity*2:16000;
    if(capacity<needed)capacity=needed;
    if(capacity>limit)capacity=limit;
    int16_t *next=realloc(buffer->data,capacity*sizeof(*next));
    if(!next)return -ENOMEM;
    buffer->data=next;buffer->capacity=capacity;
  }
  memcpy(buffer->data+buffer->frames,data,frames*sizeof(*data));
  buffer->frames=needed;return 0;
}
#ifdef GLASS_ESP_TTS
static int speech_esp_play(const char *text,struct speech_play *play)
{
  struct speech_buffer buffer={0};
  int ret=glass_esp_synthesize(text,speech_buffer_write,speech_cancelled,&buffer);
  /* The vendor can produce uneven chunks. Finish this bounded UI phrase
   * before opening DMA, so playback never waits for parser/decoder work. */
  printf("[speech] prebuffer frames=%u render_ms=%lu result=%d\n",(unsigned)buffer.frames,
    (unsigned long)(speech_milliseconds()-play->started),ret);
  if(!ret){unsigned gain=100;ret=glass_speech_normalize(buffer.data,buffer.frames,speech_cancelled,NULL,&gain);
    printf("[speech] loudness mode=lookahead gain_percent=%u volume=%u result=%d\n",gain,glass_speech_volume(),ret);}
  for(size_t offset=0;!ret&&offset<buffer.frames;){
    size_t count=buffer.frames-offset;
    if(count>MUSIC_PCM_FRAMES)count=MUSIC_PCM_FRAMES;
    ret=speech_write(play,buffer.data+offset,count);
    offset+=count;
  }
  free(buffer.data);return ret;
}
#else
static int speech_sano_play(const char *text,struct speech_play *play)
{
  /* The current Chinese neural decoder cannot yet sustain real-time audio
   * on P4. Render before taking DMA ownership, so compute pauses cannot turn
   * into missing syllables. Keep its raw PCM for a fair quality comparison. */
  struct speech_buffer buffer={0};
  int ret=glass_sano_synthesize("/sdcard/speech",text,speech_buffer_write,
                               speech_cancelled,&buffer);
  printf("[speech] prebuffer frames=%u render_ms=%lu result=%d\n",
    (unsigned)buffer.frames,(unsigned long)(speech_milliseconds()-play->started),ret);
  for(size_t offset=0;!ret&&offset<buffer.frames;){
    size_t count=buffer.frames-offset;
    if(count>MUSIC_PCM_FRAMES)count=MUSIC_PCM_FRAMES;
    ret=speech_write(play,buffer.data+offset,count);offset+=count;
  }
  free(buffer.data);return ret;
}
#endif
static int speech_wait_wake(void)
{
  struct glass_custom_wake *wake=NULL;
  struct voice_capture capture={.fd=-1,.mq=(mqd_t)-1};
  int ret=glass_custom_wake_create("/sdcard/speech",&wake);
  if(ret)return ret;
  if(speech_cancelled(NULL)){glass_custom_wake_destroy(wake);return -ECANCELED;}
  ret=capture_open(&capture);
  if(!ret){
    speech_publish(SPEECH_WAKE,0,"请说：" GLASS_WAKE_PHRASE,0,0);
    while(!speech_cancelled(NULL)){
      int16_t mono[VOICE_FRAMES];unsigned level=0;
      int n=capture_next(&capture,mono,&level);if(n<0){ret=n;break;}if(!n)continue;
      int event=glass_custom_wake_feed(wake,mono,n);if(event<0){ret=event;break;}
      pthread_mutex_lock(&speech_lock);
      speech.frames+=n;speech.level=level;
      speech.wake_evaluations=glass_custom_wake_evaluations(wake);
      speech.wake_score=glass_custom_wake_score(wake);
      if(event>0)speech.wake_events++;
      pthread_mutex_unlock(&speech_lock);
      if(event>0)break;
    }
  }
  /* The UI may open cloud ASR only after worker completion is published. */
  uint64_t closed=speech_milliseconds();
  capture_close(&capture);glass_custom_wake_destroy(wake);
  printf("[speech] wake_release elapsed_ms=%lu result=%d\n",(unsigned long)(speech_milliseconds()-closed),ret);return ret;
}
static int speech_recognize(void)
{
  struct glass_sr *sr=NULL;struct voice_capture capture={.fd=-1,.mq=(mqd_t)-1};
  int ret=glass_sr_create("/sdcard/speech",default_commands,sizeof(default_commands)/sizeof(*default_commands),&sr);
  if(ret)return ret;
  if(speech_cancelled(NULL)){glass_sr_destroy(sr);return -ECANCELED;}
  ret=capture_open(&capture);
  if(!ret){
    speech_publish(SPEECH_WAKE,0,"请说：" GLASS_WAKE_PHRASE,0,0);
    while(!speech_cancelled(NULL)){
      int16_t mono[VOICE_FRAMES];unsigned level=0;
      int n=capture_next(&capture,mono,&level);if(n<0){ret=n;break;}if(!n)continue;
      int command=0,event=glass_sr_feed(sr,mono,n,&command);if(event<0){ret=event;break;}
      unsigned evaluations=0;float score=0;glass_sr_metrics(sr,&evaluations,&score);
      pthread_mutex_lock(&speech_lock);speech.wake_evaluations=evaluations;speech.wake_score=score;
      speech.frames+=n;if(event==1){
        speech.wake_events++;
        if(!speech_cancelled(NULL)){if(!++speech.wake_event)speech.wake_event=1;speech.wake_ms=(uint32_t)speech_milliseconds();}
      }pthread_mutex_unlock(&speech_lock);
      if(event==3)speech_publish(SPEECH_WAKE,0,"等待唤醒：" GLASS_WAKE_PHRASE,0,level);
      if(event==1)speech_publish(SPEECH_COMMAND,0,"请说中文指令",0,level);
      if(event==2){
        for(unsigned i=0;i<sizeof(default_commands)/sizeof(*default_commands);i++)
          if(command==default_commands[i].id){speech_publish(SPEECH_WAKE,0,default_commands[i].text,command,level);break;}
        if(command==8)break;
      }
    }
  }
  capture_close(&capture);glass_sr_destroy(sr);return ret;
}
static int speech_probe(void)
{
  const char *names[]={"wake-positive.pcm","wake-negative.pcm"};
  for(unsigned i=0;i<2;i++){
    struct glass_sr *sr=NULL;int ret=glass_sr_create("/sdcard/speech",default_commands,sizeof(default_commands)/sizeof(*default_commands),&sr);
    if(ret)return ret;
    char path[128];snprintf(path,sizeof(path),"/sdcard/speech/test/%s",names[i]);FILE *file=fopen(path,"rb");
    if(!file){ret=-errno;glass_sr_destroy(sr);return ret;}
    unsigned events=0,total=0;int16_t pcm[1024];size_t n;
    uint64_t started=speech_milliseconds();
    while((n=fread(pcm,sizeof(int16_t),1024,file))>0){
      if(speech_cancelled(NULL)){ret=-ECANCELED;break;}
      total+=n;if(total>16000*30){ret=-EFBIG;break;}
      int command=0,event=glass_sr_feed(sr,pcm,n,&command);if(event<0){ret=event;break;}if(event==1)events++;
    }
    if(ferror(file)&&!ret)ret=-EIO;fclose(file);
    unsigned evaluations;float score;glass_sr_metrics(sr,&evaluations,&score);
    printf("[speech] fixture=%s events=%u evaluations=%u score=%.4f samples=%u compute_ms=%lu\n",
      names[i],events,evaluations,(double)score,total,(unsigned long)(speech_milliseconds()-started));
    if(!ret&&events!=(i==0?1:0))ret=-EBADMSG;
    glass_sr_destroy(sr);if(ret)return ret;
  }
  speech_publish(SPEECH_IDLE,0,"唤醒模型离线测试通过（非真人测试）",0,0);return 0;
}
#ifdef GLASS_NANO_PROBE
#include "glass_nano_probe.h"
#endif
#ifdef GLASS_ZH_Q8_PROBE
#include "glass_zh_q8.h"
#endif
#ifdef GLASS_ESP_TTS_PROBE
#include "glass_esp_tts_probe.h"
#endif
struct speech_job {char *text;bool probe,wake_only;int nano;};
static void speech_complete_locked(int ret,bool wake_only)
{
  bool cancelled=speech_cancelled(NULL);
  if(ret==-ECANCELED||cancelled)ret=0;
  speech.wake_ready=wake_only&&!ret&&!cancelled&&speech.wake_events>0;
  if(speech.wake_ready){
    if(!++speech.wake_event)speech.wake_event=1;
    speech.wake_ms=(uint32_t)speech_milliseconds();
  }
  speech.active=false;speech.phase=ret?SPEECH_ERROR:SPEECH_IDLE;
  speech.error=ret;speech.level=0;speech.revision++;
}
static void speech_complete(int ret,bool wake_only)
{pthread_mutex_lock(&speech_lock);speech_complete_locked(ret,wake_only);pthread_mutex_unlock(&speech_lock);}
static struct speech_job *speech_next_job(int ret,bool wake_only)
{
  pthread_mutex_lock(&speech_lock);
  struct speech_job *job=speech_pending;speech_pending=NULL;
  bool discard=speech_pending_cancelled;speech_pending_cancelled=false;
  if(job&&!discard){
    atomic_store(&speech_cancel,false);speech.background_wake=false;
    speech.wake_events=speech.wake_evaluations=speech.frames=0;speech.wake_score=0;
    speech.phase=SPEECH_LOADING;speech.revision++;
  }else speech_complete_locked(ret,wake_only&&!job);
  pthread_mutex_unlock(&speech_lock);
  if(job&&discard){free(job->text);free(job);job=NULL;}
  return job;
}
static void *speech_worker(void *argument)
{
#if defined(__riscv) && defined(CONFIG_ARCH_CHIP_ESP32P4)
#if !CONFIG_ARCH_RISCV_INTXCPT_EXTENSIONS
#error "P4 speech kernels require saved PIE and hardware-loop context"
#endif
  /* NuttX initializes both per-task coprocessors disabled. Enable them in
   * this worker, as the ESP-DL worker does; the extended-context hook keeps
   * their registers isolated across preemption and CPU migration. */
  __asm__ volatile("csrwi 0x7f2, 1\ncsrwi 0x7f1, 1" ::: "memory");
#endif
  struct speech_job *job=argument;
again:
  ;char *text=job->text;bool probe=job->probe,wake_only=job->wake_only;int nano=job->nano;free(job);int ret;
  (void)nano;
  printf("[speech] worker mode=%s\n",text?"tts":wake_only?"wake":"recognize");
#ifdef GLASS_P4TTS_PROBE
  if(nano>=30){
    extern int glass_p4tts_probe(int,bool (*)(void *),void *);
    speech_publish(SPEECH_SPEAKING,0,"User P4TTS performance diagnostic",0,0);
    ret=glass_p4tts_probe(nano-30,speech_cancelled,NULL);
  }
  else
#endif
#ifdef GLASS_ESP_TTS_PROBE
  if(nano>=20){speech_publish(SPEECH_SPEAKING,0,"Espressif Chinese TTS diagnostic",0,0);ret=glass_esp_tts_probe(nano-20,speech_cancelled,NULL);}
  else
#endif
#ifdef GLASS_ZH_Q8_PROBE
  if(nano>=10){speech_publish(SPEECH_SPEAKING,0,"Custom Chinese Q8 diagnostic",0,0);ret=glass_zh_q8_probe(nano-10,speech_cancelled,NULL);}
  else
#endif
#ifdef GLASS_NANO_PROBE
  if(nano){speech_publish(SPEECH_SPEAKING,0,"Official English INT8 diagnostic",0,0);ret=glass_nano_probe(nano-1,speech_cancelled,NULL);}
  else
#endif
  if(probe)ret=speech_probe();
  else if(wake_only)ret=speech_wait_wake();
  else if(text){
    uint64_t started=speech_milliseconds();
    struct speech_play play={.started=started,.volume=glass_speech_volume(),
#ifdef GLASS_ESP_TTS
      .rate=GLASS_ESP_TTS_RATE
#else
      .rate=22050
#endif
    };uint64_t played=0;
    speech_publish(SPEECH_SPEAKING,0,"正在合成中文语音",0,0);
#ifdef GLASS_ESP_TTS
    ret=speech_esp_play(text,&play);
#else
    ret=speech_sano_play(text,&play);
#endif
    if(play.opened){
      if(!ret&&!speech_cancelled(NULL))ret=music_pcm_drain(&play.pcm);
      played=play.pcm.played_samples;
      uint64_t elapsed=speech_milliseconds()-play.first_write;
      printf("[speech] playback frames=%llu expected_ms=%llu elapsed_ms=%llu result=%d\n",
        (unsigned long long)played,(unsigned long long)(played*1000/play.rate),(unsigned long long)elapsed,ret);
      speech_barge_stop(play.barge);
      music_pcm_close(&play.pcm);
    }
#ifdef GLASS_ESP_TTS
    if(ret==-ENOMEM)glass_speech_clear_cache();
#else
    if(ret||speech_cancelled(NULL))glass_speech_clear_cache();
#endif
    printf("[speech] tts done result=%d elapsed_ms=%lu played_frames=%llu\n",ret,(unsigned long)(speech_milliseconds()-started),(unsigned long long)played);
    free(text);
  }else ret=speech_recognize();
  /* Capture/model teardown has finished before handing the same worker to a
   * foreground request. The UI never waits for microphone release. */
  job=speech_next_job(ret,wake_only);
  if(job)goto again;
  return NULL;
}
static int speech_start_mode_ungated(const char *text,bool probe,bool wake_only,uint32_t *request,int nano,bool background)
{
  char *copy=NULL;if(text){if(!text[0]||strlen(text)>1024)return -EINVAL;copy=strdup(text);if(!copy)return -ENOMEM;}
  struct speech_job *job=calloc(1,sizeof(*job));if(!job){free(copy);return -ENOMEM;}job->text=copy;job->probe=probe;job->wake_only=wake_only;job->nano=nano;
  pthread_mutex_lock(&speech_lock);
  bool handoff=!background&&speech.active&&(wake_only||speech.background_wake);
  if((background&&music_pcm_foreground_pending())||(speech.active&&!handoff)||(music_pcm_session_active()&&!handoff&&!wake_only)){pthread_mutex_unlock(&speech_lock);free(copy);free(job);return -EBUSY;}
  if(handoff){
    if(speech_pending){free(speech_pending->text);free(speech_pending);}
    speech_pending=job;speech_pending_cancelled=false;atomic_store(&speech_cancel,true);
    speech.background_wake=false;speech.wake_ready=false;
    speech.error=speech.command=0;speech.text[0]=0;speech.phase=SPEECH_LOADING;
    speech.command_event=speech.command_ms=0;speech.last_command=0;
    if(!++speech.request)speech.request=1;speech.revision++;
    if(request)*request=speech.request;
    pthread_mutex_unlock(&speech_lock);return 0;
  }
#ifndef GLASS_ESP_TTS
  if(!text)glass_speech_clear_cache();
#endif
  atomic_store(&speech_cancel,false);speech.active=true;speech.phase=SPEECH_LOADING;
  speech.error=speech.command=0;speech.text[0]=0;speech.revision++;
  speech.wake_ready=false;
  speech.background_wake=background;
  if(!++speech.request)speech.request=1;
  speech.frames=speech.wake_events=speech.wake_evaluations=0;speech.wake_score=0;
  speech.command_event=speech.command_ms=0;speech.last_command=0;
  pthread_attr_t attr;int ret=pthread_attr_init(&attr);
  if(!ret){ret=pthread_attr_setstacksize(&attr,96*1024);
    if(!ret)ret=pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
    if(!ret){pthread_t tid;ret=pthread_create(&tid,&attr,speech_worker,job);}pthread_attr_destroy(&attr);}
  if(ret){speech.active=false;speech.phase=SPEECH_ERROR;speech.error=-ret;free(copy);free(job);}
  else if(request)*request=speech.request;
  pthread_mutex_unlock(&speech_lock);return -ret;
}
static int speech_start_mode(const char *text,bool probe,bool wake_only,uint32_t *request,int nano,bool background)
{
  if(request)*request=0;
  if(!glass_ai_start_enter())return -EBUSY;
  int ret=speech_start_mode_ungated(text,probe,wake_only,request,nano,background);
  glass_ai_start_leave();return ret;
}
static int speech_start(const char *text,bool probe,bool wake_only,uint32_t *request)
{return speech_start_mode(text,probe,wake_only,request,0,false);}
int glass_speech_listen(void){return glass_voice_start();}
int glass_speech_wait_wake(uint32_t *request){if(!request)return -EINVAL;*request=0;glass_voice_cancel();return speech_start(NULL,false,true,request);}
int glass_speech_background_wake(uint32_t *request)
{
  if(!request)return -EINVAL;*request=0;
  return speech_start_mode(NULL,false,true,request,0,true);
}
int glass_speech_say_request(const char *text,uint32_t *request){if(!text)return -EINVAL;return speech_start(text,false,false,request);}
int glass_speech_say(const char *text){return glass_speech_say_request(text,NULL);}
int glass_speech_probe(void){return speech_start(NULL,true,false,NULL);}
int glass_speech_nano(int mode)
{
#ifdef GLASS_NANO_PROBE
  if(mode<0||mode>2)return -EINVAL;
  return speech_start_mode(NULL,false,false,NULL,mode+1,false);
#else
  (void)mode;return -ENOSYS;
#endif
}
int glass_speech_zh_q8(int mode)
{
#ifdef GLASS_ZH_Q8_PROBE
  if(mode<0||mode>5)return -EINVAL;
  return speech_start_mode(NULL,false,false,NULL,mode+10,false);
#else
  (void)mode;return -ENOSYS;
#endif
}
int glass_speech_esp_tts(int mode)
{
#ifdef GLASS_ESP_TTS_PROBE
  if(mode<0||mode>2)return -EINVAL;
  return speech_start_mode(NULL,false,false,NULL,mode+20,false);
#else
  (void)mode;return -ENOSYS;
#endif
}
int glass_speech_p4tts(int mode)
{
#ifdef GLASS_P4TTS_PROBE
  if(mode<0||mode>2)return -EINVAL;
  return speech_start_mode(NULL,false,false,NULL,mode+30,false);
#else
  (void)mode;return -ENOSYS;
#endif
}
int glass_speech_cli(int argc,char **argv)
{
  if((argc==3||argc==4)&&!strcmp(argv[2],"volume")){
    if(argc==4){char *end;unsigned long value=strtoul(argv[3],&end,10);
      if(!argv[3][0]||argv[3][0]=='-'||*end||value>100)return -EINVAL;
      atomic_store(&speech_volume,(unsigned)value);}
    printf("speech volume=%u range=0..100\n",glass_speech_volume());return 0;
  }
#ifdef GLASS_ESP_TTS
  if(argc==3&&!strcmp(argv[2],"cache-clear")){glass_speech_clear_cache();return 0;}
  if((argc==3||argc==4)&&!strcmp(argv[2],"voice")){
    if(argc==4){int ret=glass_esp_tts_voice_select(argv[3]);if(ret)return ret;}
    printf("speech voice=%s choices=xiaoxin,xiaole,small\n",glass_esp_tts_voice_name());return 0;
  }
  if((argc==3||argc==4)&&!strcmp(argv[2],"speed")){
    if(argc==4){
      if(argv[3][0]<'0'||argv[3][0]>'5'||argv[3][1])return -EINVAL;
      int ret=glass_esp_tts_set_speed((unsigned)(argv[3][0]-'0'));if(ret)return ret;
    }
    printf("speech backend=esp-tts speed=%u range=0..5\n",glass_esp_tts_speed());return 0;
  }
#endif
  if(argc==3&&!strcmp(argv[2],"listen"))return glass_speech_listen();
  if(argc==4&&!strcmp(argv[2],"say"))return glass_speech_say(argv[3]);
  if(argc==3&&!strcmp(argv[2],"stop")){glass_speech_stop();glass_voice_stop();return 0;}
  if(argc==3&&!strcmp(argv[2],"status")){struct speech_snapshot s;glass_speech_get(&s);
    printf("speech active=%u phase=%d error=%d command=%d wakes=%u evaluations=%u score=%.4f frames=%u text=%s backend=%s\n",
      s.active,s.phase,s.error,s.command,s.wake_events,s.wake_evaluations,(double)s.wake_score,s.frames,s.text,glass_speech_backend());return 0;}
  puts("desktop speech listen | say <Chinese text> | stop | status | volume [0..100]");
#ifdef GLASS_ESP_TTS
  puts("desktop speech speed [0..5] | esp-tts | esp-tts-play | esp-tts-export");
#endif
  return -EINVAL;
}
