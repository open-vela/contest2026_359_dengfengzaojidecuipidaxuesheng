/* SPDX-License-Identifier: Apache-2.0 */
/* Actual LLM with instrumented stdio; no model/audio/network substitutes. */
#include "glass_local_llm.h"
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static bool g_measure;
static size_t g_reads, g_bytes, g_seeks;
static size_t g_rope_pows;
#ifdef GLV_TEST_ROPE_COUNT
float __real_powf(float, float);
float __wrap_powf(float x, float y)
{
  if(g_measure)g_rope_pows++;
  return __real_powf(x, y);
}
#endif
size_t __real_fread(void *, size_t, size_t, FILE *);
int __real_fseek(FILE *, long, int);
size_t __wrap_fread(void *data, size_t size, size_t count, FILE *file)
{
  size_t result = __real_fread(data, size, count, file);
  if(g_measure){g_reads++;g_bytes += size * result;}
  return result;
}
int __wrap_fseek(FILE *file, long offset, int origin)
{
  if(g_measure)g_seeks++;
  return __real_fseek(file, offset, origin);
}
struct cancel_state {unsigned calls;bool enabled;};
#ifdef GLV_TEST_PROGRESS
static unsigned progress_calls,last_done,last_total;
static enum glv_llm_progress last_stage;
static void observe(void *opaque,enum glv_llm_progress stage,unsigned done,unsigned total)
{
  assert(opaque&&done<=total);
  if(progress_calls&&stage==last_stage)assert(done>=last_done&&total==last_total);
  last_stage=stage;last_done=done;last_total=total;progress_calls++;
}
#endif
static bool cancelled(void *opaque)
{
  struct cancel_state *state = opaque;
  return state->enabled && ++state->calls >= 1800;
}
static double seconds(void)
{
  struct timespec now;
  assert(!clock_gettime(CLOCK_MONOTONIC, &now));
  return now.tv_sec + now.tv_nsec / 1e9;
}
int main(int argc, char **argv)
{
  assert(argc == 6);
  const char *mode = argv[1];
  unsigned count = (unsigned)strtoul(argv[3], NULL, 10);
  assert(count && count <= 512);
  struct cancel_state cancel = {0};
  struct glv_memory memory = {.limit=GLV_MEMORY_LIMIT,
    .cancelled=cancelled,.opaque=&cancel};
#ifdef GLV_TEST_PROGRESS
  memory.llm_progress=observe;
#endif
  struct glv_llm *llm = NULL;
  int ret = glv_llm_open(argv[2], &memory, &llm);
  assert(ret == 0);
  size_t opened = memory.used;
  if(!strcmp(argv[5], "tight"))memory.limit=opened+6291456u+220u*1024u;
  if(!strcmp(argv[5], "tiny"))memory.limit=opened+6291456u+4096u;
  int ids[512];
  for(unsigned i=0;i<count;i++)ids[i]=(int)((i*7919u+31u)%8192u);
  float *logits = calloc(8192, sizeof(float));assert(logits);
  g_measure=true;
  double began=seconds();
  if(!strcmp(mode,"cancel"))cancel.enabled=true;
  if(!strcmp(mode,"sequential")){
    for(unsigned i=0;i<count&&!ret;i++)
      ret=glv_llm_forward(llm,ids[i],i+1==count?logits:NULL);
  }else if(!strcmp(mode,"generate")){
    char reply[4096];
    ret=glv_llm_generate(llm,"<|im_start|>user\n你好<|im_end|>\n<|im_start|>assistant\n",count,reply,sizeof(reply),NULL,NULL);
    if(ret>=0){FILE *f=fopen(argv[4],"wb");assert(f);assert(fwrite(reply,1,(size_t)ret,f)==(size_t)ret);fclose(f);ret=0;}
  }else ret=glv_llm_prefill(llm,ids,count,logits);
  double elapsed=seconds()-began;
  g_measure=false;
  if(!strcmp(mode,"cancel"))assert(ret==-ECANCELED);
  else if(!strcmp(argv[5],"tiny"))assert(ret==-ENOMEM);
  else {
    assert(ret==0);
    if(strcmp(mode,"generate")){
      for(unsigned i=0;i<8192;i++)assert(glv_finite(logits[i]));
      FILE *f=fopen(argv[4],"wb");assert(f);
      assert(fwrite(logits,sizeof(float),8192,f)==8192);fclose(f);
    }
  }
  free(logits);glv_llm_close(llm);assert(memory.used==0);
#ifdef GLV_TEST_PROGRESS
  assert(progress_calls);
  if(!strcmp(mode,"prefill"))assert(last_stage==GLV_LLM_PREFILL&&last_done==count*12&&last_total==count*12);
  if(!strcmp(mode,"generate"))assert(last_stage==GLV_LLM_DECODING&&last_done==count);
#endif
  printf("{\"error\":%d,\"elapsed_ms\":%.3f,\"fread_calls\":%zu,"
         "\"fread_bytes\":%zu,\"fseek_calls\":%zu,\"peak_bytes\":%zu,"
         "\"remaining_bytes\":%zu,\"limit_bytes\":%zu,\"opened_bytes\":%zu,\"rope_pow_calls\":%zu}\n",
         ret,elapsed*1000,g_reads,g_bytes,g_seeks,memory.peak,memory.used,
         memory.limit,opened,g_rope_pows);
  return 0;
}
