/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#ifndef GROK_FRAME_CACHE_BUDGET
#define GROK_FRAME_CACHE_BUDGET (512u * 1024u)
#endif
#define GROK_FRAME_CACHE_MAX_FRAMES 128u

/* The reader owns each cache. Only budget accounting is shared by players.
 * Budget includes requested payload/metadata bytes, excluding heap overhead. */
struct grok_frame_cache_entry {
  uint8_t *data;
  size_t size;
  bool attempted;
};
struct grok_frame_cache {
  struct grok_frame_cache_entry *entries;
  unsigned count;
  size_t bytes;
  bool attempted;
};
static pthread_mutex_t grok_cache_budget_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t grok_cache_budget_used;

static size_t grok_frame_cache_total(void)
{
  pthread_mutex_lock(&grok_cache_budget_lock);
  size_t used=grok_cache_budget_used;
  pthread_mutex_unlock(&grok_cache_budget_lock);
  return used;
}
static bool grok_cache_reserve(size_t bytes)
{
  pthread_mutex_lock(&grok_cache_budget_lock);
  bool fits=bytes<=GROK_FRAME_CACHE_BUDGET-grok_cache_budget_used;
  if(fits)grok_cache_budget_used+=bytes;
  pthread_mutex_unlock(&grok_cache_budget_lock);
  return fits;
}
static void grok_cache_release(size_t bytes)
{
  pthread_mutex_lock(&grok_cache_budget_lock);
  grok_cache_budget_used-=bytes;
  pthread_mutex_unlock(&grok_cache_budget_lock);
}

/* RGB565 and alpha are separate planes in LVGL's RGB565A8 format. Runs never
 * cross the plane boundary: (count,lo,hi) for RGB, (count,value) for alpha. */
static size_t grok_rle_size(const uint8_t *pixels,size_t count)
{
  size_t size=0;
  for(size_t i=0;i<count;){
    size_t run=1;
    while(run<255&&i+run<count&&pixels[(i+run)*2]==pixels[i*2]&&
          pixels[(i+run)*2+1]==pixels[i*2+1])run++;
    size+=3;i+=run;
  }
  pixels+=count*2;
  for(size_t i=0;i<count;){
    size_t run=1;while(run<255&&i+run<count&&pixels[i+run]==pixels[i])run++;
    size+=2;i+=run;
  }
  return size;
}
static void grok_rle_encode(uint8_t *out,const uint8_t *pixels,size_t count)
{
  for(size_t i=0;i<count;){
    size_t run=1;
    while(run<255&&i+run<count&&pixels[(i+run)*2]==pixels[i*2]&&
          pixels[(i+run)*2+1]==pixels[i*2+1])run++;
    *out++=(uint8_t)run;*out++=pixels[i*2];*out++=pixels[i*2+1];i+=run;
  }
  pixels+=count*2;
  for(size_t i=0;i<count;){
    size_t run=1;while(run<255&&i+run<count&&pixels[i+run]==pixels[i])run++;
    *out++=(uint8_t)run;*out++=pixels[i];i+=run;
  }
}
static bool grok_rle_decode(uint8_t *out,size_t count,const uint8_t *in,size_t size)
{
  size_t at=0,done=0;
  while(done<count){
    if(size-at<3)return false;
    unsigned run=in[at++];uint8_t lo=in[at++],hi=in[at++];
    if(!run||run>count-done)return false;
    for(unsigned i=0;i<run;i++){*out++=lo;*out++=hi;}done+=run;
  }
  done=0;
  while(done<count){
    if(size-at<2)return false;
    unsigned run=in[at++];uint8_t alpha=in[at++];
    if(!run||run>count-done)return false;
    memset(out,alpha,run);out+=run;done+=run;
  }
  return at==size;
}
static void grok_frame_cache_clear(struct grok_frame_cache *cache)
{
  if(cache->entries){
    for(unsigned i=0;i<cache->count;i++)free(cache->entries[i].data);
    free(cache->entries);grok_cache_release(cache->bytes);
  }
  memset(cache,0,sizeof(*cache));
}
static bool grok_frame_cache_prepare(struct grok_frame_cache *cache,unsigned count)
{
  if(cache->entries)return cache->count==count;
  if(cache->attempted)return false;
  cache->attempted=true;
  if(!count||count>GROK_FRAME_CACHE_MAX_FRAMES)return false;
  size_t bytes=count*sizeof(*cache->entries);
  if(!grok_cache_reserve(bytes))return false;
  cache->entries=calloc(count,sizeof(*cache->entries));
  if(!cache->entries){grok_cache_release(bytes);return false;}
  cache->count=count;cache->bytes=bytes;return true;
}
static bool grok_frame_cache_load(struct grok_frame_cache *cache,unsigned frame,
                                 uint8_t *pixels,size_t count)
{
  if(!cache->entries||frame>=cache->count)return false;
  struct grok_frame_cache_entry *entry=&cache->entries[frame];
  if(!entry->data)return false;
  if(grok_rle_decode(pixels,count,entry->data,entry->size))return true;
  free(entry->data);grok_cache_release(entry->size);cache->bytes-=entry->size;
  entry->data=NULL;entry->size=0;return false;
}
static void grok_frame_cache_store(struct grok_frame_cache *cache,unsigned frame,
                                  const uint8_t *pixels,size_t count)
{
  if(!cache->entries||frame>=cache->count)return;
  struct grok_frame_cache_entry *entry=&cache->entries[frame];
  if(entry->attempted)return;
  /* Allocation/budget failures fall back to SD without recompressing on every
   * loop. A bank reopen or player recreation permits another cache attempt. */
  entry->attempted=true;
  size_t size=grok_rle_size(pixels,count);
  if(size>=count*3||!grok_cache_reserve(size))return;
  uint8_t *data=malloc(size);
  if(!data){grok_cache_release(size);return;}
  grok_rle_encode(data,pixels,count);
  entry->data=data;entry->size=size;cache->bytes+=size;
}
