#include "glass_material.h"
/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_pinyin.h"
#include "music_vendor/googlepinyin/include/pinyinime.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef __NuttX__
#include <malloc.h>
#endif
using namespace ime_pinyin;
extern "C" {
int glass_local_voice_yield_idle_model(unsigned) __attribute__((weak));
FILE *glass_pinyin_dictionary_open(void)
{
 if(!gmat_verify(GMAT_INPUT_PINYIN_DICT)){FILE *file=gmat_fopen(GMAT_INPUT_PINYIN_DICT);if(file)return file;}
 return gmat_fopen(GMAT_BUILTIN_PINYIN);
}
}
static pthread_mutex_t decoder_lock=PTHREAD_MUTEX_INITIALIZER;
static bool initialized,composing;
static bool loading,close_requested;
static uint64_t retry_at;
static uint64_t now_ms(void)
{struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static unsigned candidates;
static bool utf8(const char16 *text,char *out,size_t cap)
{
  size_t n=0;if(!cap)return false;
  for(;*text;text++){
    unsigned c=*text,k=c<0x80?1:c<0x800?2:3;
    if(n+k>=cap){out[0]=0;return false;}
    if(k==1)out[n++]=(char)c;
    else {if(k==3){out[n++]=(char)(0xe0|(c>>12));out[n++]=(char)(0x80|((c>>6)&63));}
      else out[n++]=(char)(0xc0|(c>>6));out[n++]=(char)(0x80|(c&63));}
  }out[n]=0;return true;
}
static bool candidate_locked(unsigned index,char *out,size_t cap)
{
  char16 text[40]={0};if(cap)out[0]=0;
  return initialized&&index<candidates&&im_get_candidate(index,text,40)&&utf8(text,out,cap);
}
static void *prepare_decoder(void *)
{
  /* Never hold this lock while waiting for the model's owner worker.
   * An active AI/ASR request is never cancelled to open a keyboard. */
  int admission=glass_local_voice_yield_idle_model?glass_local_voice_yield_idle_model(200):0;
  pthread_mutex_lock(&decoder_lock);
#ifdef __NuttX__
  /* Decoder residency is about 1.5 MiB. Leave the existing 4 MiB system
   * reserve untouched, including during its temporary stream/cache buffers. */
  struct mallinfo memory=mallinfo();
  if((size_t)memory.fordblks<6u*1024u*1024u||(size_t)memory.mxordblk<1024u*1024u)admission=-1;
#endif
  if(!initialized&&!close_requested&&!admission){
#ifdef __NuttX__
    mkdir("/data/qpk",0700);mkdir("/data/qpk/pinyin",0700);
    const char *user_path="/data/qpk/pinyin/user.dat";
#else
    const char *user_path="/tmp/music-ime-tests/user.dat";
#endif
    initialized=im_open_decoder("@builtin",user_path);
    if(initialized){im_set_max_lens(32,16);im_reset_search();}else im_close_decoder();
    printf("[ime] Google Pinyin dictionary=%u loaded=%d\n",(unsigned)gmat_size(GMAT_INPUT_PINYIN_DICT),initialized);
  }
  loading=false;retry_at=initialized?0:now_ms()+2000;
  pthread_mutex_unlock(&decoder_lock);return NULL;
}
bool glass_pinyin_init(void)
{
  /* Called by LVGL on attach and on a first keystroke. SD verification,
   * decompression and decoder allocation must never run on that thread. */
  if(pthread_mutex_trylock(&decoder_lock))return false;
  bool ready=initialized;
  if(!ready&&!loading&&now_ms()>=retry_at){
    pthread_attr_t attr;int ret=pthread_attr_init(&attr);
    if(!ret){
      ret=pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
#ifdef __NuttX__
      if(!ret)ret=pthread_attr_setstacksize(&attr,16384);
#endif
      if(!ret){loading=true;close_requested=false;pthread_t worker;ret=pthread_create(&worker,&attr,prepare_decoder,NULL);if(ret)loading=false;}
      pthread_attr_destroy(&attr);
    }
    if(ret)retry_at=now_ms()+2000;
  }
  pthread_mutex_unlock(&decoder_lock);return ready;
}
void glass_pinyin_reset(void)
{
  if(pthread_mutex_trylock(&decoder_lock))return;
  if(initialized)im_reset_search();
  composing=false;candidates=0;pthread_mutex_unlock(&decoder_lock);
}
unsigned glass_pinyin_search(const char *input)
{
  if(!input||strlen(input)>32)return 0;
  if(!*input){glass_pinyin_reset();return 0;}
  for(unsigned retry=0;retry<2;retry++){
    if(!glass_pinyin_init())return 0;
    if(pthread_mutex_trylock(&decoder_lock))return 0;
    if(initialized){composing=true;candidates=(unsigned)im_search(input,strlen(input));unsigned n=candidates;pthread_mutex_unlock(&decoder_lock);return n;}
    pthread_mutex_unlock(&decoder_lock);
  }return 0;
}
bool glass_pinyin_candidate(unsigned index,char *out,size_t cap)
{if(cap)out[0]=0;if(pthread_mutex_trylock(&decoder_lock))return false;bool ok=candidate_locked(index,out,cap);pthread_mutex_unlock(&decoder_lock);return ok;}
bool glass_pinyin_choose(unsigned index,char *out,size_t cap,unsigned *consumed)
{
  if(!consumed)return false;*consumed=0;if(cap)out[0]=0;
  if(pthread_mutex_trylock(&decoder_lock))return false;bool ok=false;
  if(initialized&&index<candidates){
    size_t decoded=0;im_get_sps_str(&decoded);candidates=(unsigned)im_choose(index);
    const uint16 *starts=NULL;size_t syllables=im_get_spl_start_pos(starts);
    ok=true;
    if(candidates==1&&im_get_fixed_len()==syllables){ok=candidate_locked(0,out,cap);if(ok)*consumed=(unsigned)decoded;}
  }
  pthread_mutex_unlock(&decoder_lock);return ok;
}
void glass_pinyin_flush(void)
{if(pthread_mutex_trylock(&decoder_lock))return;if(initialized)im_flush_cache();pthread_mutex_unlock(&decoder_lock);}
void glass_pinyin_close(void)
{pthread_mutex_lock(&decoder_lock);close_requested=true;if(initialized)im_close_decoder();initialized=composing=false;candidates=0;pthread_mutex_unlock(&decoder_lock);}
bool glass_pinyin_reclaim_idle(void)
{
  if(pthread_mutex_trylock(&decoder_lock))return false;
  bool idle=!composing&&!loading;
  if(idle){if(initialized)im_close_decoder();initialized=false;candidates=0;}
  pthread_mutex_unlock(&decoder_lock);return idle;
}
