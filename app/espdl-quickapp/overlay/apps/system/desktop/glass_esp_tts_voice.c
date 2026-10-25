/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_esp_tts.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <time.h>

#ifdef GLASS_ESP_TTS_VOICE_SD
#include "esp_tts_voice_config.h"
extern void qpk_dl_sha256(const void *,size_t,unsigned char[32]);
static pthread_mutex_t voice_cache_lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned char *voice_cache;
static unsigned voice_users;
static atomic_bool voice_evict;
static atomic_uint voice_selected;
static uint64_t voice_idle_since;
#define VOICE_CACHE_IDLE_MS UINT64_C(60000)
static uint64_t voice_now_ms(void)
{
  struct timespec now;
  if(clock_gettime(CLOCK_MONOTONIC,&now))return 0;
  return (uint64_t)now.tv_sec*1000+(uint64_t)now.tv_nsec/1000000;
}
#endif

const char *glass_esp_tts_voice_name(void)
{
#ifdef GLASS_ESP_TTS_VOICE_SD
  return glass_esp_tts_voices[atomic_load(&voice_selected)].name;
#else
  return "small";
#endif
}

int glass_esp_tts_voice_select(const char *name)
{
  if(!name)return -EINVAL;
#ifdef GLASS_ESP_TTS_VOICE_SD
  unsigned choice;
  for(choice=0;choice<sizeof(glass_esp_tts_voices)/sizeof(glass_esp_tts_voices[0]);choice++)
    if(!strcmp(name,glass_esp_tts_voices[choice].name))break;
  if(choice==sizeof(glass_esp_tts_voices)/sizeof(glass_esp_tts_voices[0]))return -EINVAL;
  int ret=pthread_mutex_trylock(&voice_cache_lock);if(ret)return -ret;
  if(voice_users){pthread_mutex_unlock(&voice_cache_lock);return -EBUSY;}
  if(choice!=atomic_load(&voice_selected)){
    free(voice_cache);voice_cache=NULL;voice_idle_since=0;atomic_store(&voice_evict,false);
    atomic_store(&voice_selected,choice);
  }
  pthread_mutex_unlock(&voice_cache_lock);return 0;
#else
  return !strcmp(name,"small")?0:-ENOTSUP;
#endif
}

void glass_esp_tts_voice_release(void *owned)
{
#ifdef GLASS_ESP_TTS_VOICE_SD
  if(!owned)return;
  pthread_mutex_lock(&voice_cache_lock);
  if(owned==voice_cache&&voice_users){voice_users--;if(!voice_users)voice_idle_since=voice_now_ms();}
  if(!voice_users&&atomic_load(&voice_evict)){
    free(voice_cache);voice_cache=NULL;voice_idle_since=0;atomic_store(&voice_evict,false);
  }
  pthread_mutex_unlock(&voice_cache_lock);
#else
  (void)owned;
#endif
}
void glass_esp_tts_cache_clear(void)
{
#ifdef GLASS_ESP_TTS_VOICE_SD
  atomic_store(&voice_evict,true);
  if(!pthread_mutex_trylock(&voice_cache_lock)){
    if(!voice_users){free(voice_cache);voice_cache=NULL;voice_idle_since=0;atomic_store(&voice_evict,false);}
    pthread_mutex_unlock(&voice_cache_lock);
  }
#endif
}

size_t glass_esp_tts_cache_trim(void)
{
#ifdef GLASS_ESP_TTS_VOICE_SD
  /* Loading holds this lock during SD reads and checksum verification. The
   * desktop must never wait for it or evict bytes still owned by synthesis. */
  if(pthread_mutex_trylock(&voice_cache_lock))return 0;
  size_t freed=0;
  if(voice_cache&&!voice_users){
    uint64_t now=voice_now_ms();
    if(now){
      if(!voice_idle_since||now<voice_idle_since)voice_idle_since=now;
      else if(now-voice_idle_since>=VOICE_CACHE_IDLE_MS){
        freed=glass_esp_tts_voices[atomic_load(&voice_selected)].bytes;
        free(voice_cache);voice_cache=NULL;voice_idle_since=0;atomic_store(&voice_evict,false);
      }
    }
  }
  pthread_mutex_unlock(&voice_cache_lock);
  return freed;
#else
  return 0;
#endif
}

int glass_esp_tts_voice_load(const unsigned char **data,void **owned,
                             speech_cancel_fn cancel,void *user)
{
  *data=NULL;*owned=NULL;
  if(cancel&&cancel(user))return -ECANCELED;
#ifdef GLASS_ESP_TTS_VOICE_SD
  int locked;
  while((locked=pthread_mutex_trylock(&voice_cache_lock))==EBUSY){
    if(cancel&&cancel(user))return -ECANCELED;
    usleep(10000);
  }
  if(locked)return -locked;
  if(!voice_users&&atomic_exchange(&voice_evict,false)){free(voice_cache);voice_cache=NULL;voice_idle_since=0;}
  if(voice_cache){
    if(cancel&&cancel(user)){pthread_mutex_unlock(&voice_cache_lock);return -ECANCELED;}
    voice_users++;*data=voice_cache;*owned=voice_cache;pthread_mutex_unlock(&voice_cache_lock);return 0;
  }
  const struct glass_esp_voice_source *source=&glass_esp_tts_voices[atomic_load(&voice_selected)];
  FILE *file=fopen(source->path,"rb");
  if(!file){int ret=-errno;pthread_mutex_unlock(&voice_cache_lock);return ret;}
  int ret=0;
  if(fseek(file,0,SEEK_END)||ftell(file)!=(long)source->bytes||fseek(file,0,SEEK_SET)){
    fclose(file);pthread_mutex_unlock(&voice_cache_lock);return -EBADMSG;
  }
  unsigned char *buffer=malloc(source->bytes);
  if(!buffer){fclose(file);pthread_mutex_unlock(&voice_cache_lock);return -ENOMEM;}
  for(size_t offset=0;offset<source->bytes;){
    if(cancel&&cancel(user)){ret=-ECANCELED;break;}
    size_t count=source->bytes-offset;if(count>16384)count=16384;
    if(fread(buffer+offset,1,count,file)!=count){ret=-EIO;break;}
    offset+=count;
  }
  if(!ret&&(fgetc(file)!=EOF||ferror(file)))ret=-EBADMSG;
  if(fclose(file)&&!ret)ret=-EIO;
  if(!ret){
    unsigned char digest[32];qpk_dl_sha256(buffer,source->bytes,digest);
    if(memcmp(digest,source->sha256,sizeof(digest)))ret=-EBADMSG;
  }
  if(!ret&&cancel&&cancel(user))ret=-ECANCELED;
  if(ret){free(buffer);pthread_mutex_unlock(&voice_cache_lock);return ret;}
  voice_cache=buffer;voice_users=1;*data=buffer;*owned=buffer;
  printf("[speech] voice_loaded=%s bytes=%u\n",source->name,(unsigned)source->bytes);
  pthread_mutex_unlock(&voice_cache_lock);
#else
  extern const unsigned char glass_esp_voice_data[];
  *data=glass_esp_voice_data;
#endif
  return 0;
}
