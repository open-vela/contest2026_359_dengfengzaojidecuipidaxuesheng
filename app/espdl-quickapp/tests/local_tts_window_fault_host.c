/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_local_tts.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static bool active,allocation_failed;
static const char *mode;
static unsigned reads,checks,sinks;
void *__real_calloc(size_t,size_t);
size_t __real_fread(void *,size_t,size_t,FILE *);
void *__wrap_calloc(size_t n,size_t size)
{
  if(active&&!strcmp(mode,"allocation")&&!allocation_failed&&n*size>=1000000u){
    allocation_failed=true;errno=ENOMEM;return NULL;
  }
  return __real_calloc(n,size);
}
size_t __wrap_fread(void *data,size_t size,size_t count,FILE *file)
{
  if(active&&!strcmp(mode,"io")&&++reads==3){errno=EIO;return 0;}
  return __real_fread(data,size,count,file);
}
static bool cancelled(void *unused)
{(void)unused;return active&&!strcmp(mode,"cancel")&&++checks>=1000;}
static int sink(void *opaque,const float *audio,unsigned count,unsigned rate)
{
  assert(rate==24000);sinks++;
  if(!strcmp(mode,"sink"))return -EPIPE;
  return fwrite(audio,sizeof(float),count,opaque)==count?0:-EIO;
}
int main(int argc,char **argv)
{
  assert(argc==5);mode=argv[1];
  struct glv_memory memory={.limit=!strcmp(mode,"io")?5u*1024u*1024u:GLV_MEMORY_LIMIT,.cancelled=cancelled};
  memory.tts_window_plan=getenv("GLV_TEST_TTS_WINDOWS")!=NULL;
  struct glv_tts *tts=NULL;assert(!glv_tts_open(argv[2],&memory,&tts));
  FILE *f=fopen(argv[3],"rb");assert(f);unsigned frames=0;assert(fread(&frames,4,1,f)==1);
  float *mel=glv_alloc(&memory,(size_t)80*frames*4);assert(mel);
  assert(fread(mel,4,(size_t)80*frames,f)==(size_t)80*frames);fclose(f);
  f=fopen(argv[4],"wb");assert(f);active=true;
  int ret=glv_tts_vocode(tts,mel,frames,sink,f);active=false;fclose(f);
  if(!strcmp(mode,"allocation"))assert(!ret&&allocation_failed&&memory.profile.tts_memory_fallbacks);
  else if(!strcmp(mode,"io"))assert(ret==-EIO&&reads==3&&!sinks&&!memory.profile.tts_memory_fallbacks);
  else if(!strcmp(mode,"cancel"))assert(ret==-ECANCELED&&!sinks&&!memory.profile.tts_memory_fallbacks);
  else if(!strcmp(mode,"sink"))assert(ret==-EPIPE&&sinks==1&&!memory.profile.tts_memory_fallbacks);
  else assert(!"bad mode");
  glv_free(mel);glv_tts_close(tts);assert(!memory.used);
  printf("TTS_WINDOW_FAULT_OK mode=%s result=%d sinks=%u fallback=%u peak=%zu remaining=%zu\n",
    mode,ret,sinks,memory.profile.tts_memory_fallbacks,memory.peak,memory.used);
  return 0;
}
