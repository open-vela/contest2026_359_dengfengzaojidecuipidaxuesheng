/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef MUSIC_CACHE_ROOT
#define MUSIC_CACHE_ROOT "/sdcard/music/cache"
#endif
#if defined(__NuttX__) || defined(MUSIC_CACHE_COORDINATION_TEST)
int glass_music_cache_write_enter(void);
void glass_music_cache_write_leave(void);
#endif
/* 32 replaceable slots per kind. Verify the whole request identity on hits;
 * a slot/hash collision can evict an entry, never return another song. */
static uint32_t music_cache_hash(uint32_t h,const void *data,size_t size)
{const unsigned char *p=data;while(size--){h^=*p++;h*=16777619u;}return h;}
static int music_cache_path(char kind,const char *key,char *path,size_t capacity)
{
  if((kind!='l'&&kind!='c')||!key||!*key||strlen(key)>2047)return -1;
  unsigned slot=music_cache_hash(2166136261u,key,strlen(key))%32;
  int n=snprintf(path,capacity,MUSIC_CACHE_ROOT "/%c%02u.bin",kind,slot);
  return n>0&&(size_t)n<capacity?0:-1;
}
static unsigned char *music_cache_read_path(const char *path,const char *key,
                                             size_t limit,size_t padding,size_t *size)
{
  *size=0;if(!path||!key||strlen(key)>2047)return NULL;
  FILE *f=fopen(path,"rb");if(!f)return NULL;
  uint32_t h[4];unsigned char *body=NULL;char *identity=NULL;struct stat st;
  if(fstat(fileno(f),&st)||fread(h,sizeof(h),1,f)!=1||h[0]!=0x3143434d||
     h[1]!=strlen(key)||h[1]>2047||!h[2]||h[2]>limit||
     (uint64_t)st.st_size!=sizeof(h)+(uint64_t)h[1]+h[2]||padding>SIZE_MAX-h[2])goto done;
  identity=malloc(h[1]);body=malloc((size_t)h[2]+padding);if(!identity||!body)goto invalid;
  if(fread(identity,1,h[1],f)!=h[1]||memcmp(identity,key,h[1])||fread(body,1,h[2],f)!=h[2]||
     music_cache_hash(music_cache_hash(2166136261u,key,h[1]),body,h[2])!=h[3])goto invalid;
  *size=h[2];goto done;
invalid:free(body);body=NULL;
done:free(identity);fclose(f);return body;
}
static unsigned char *music_cache_read(char kind,const char *key,size_t limit,size_t padding,size_t *size)
{
  char path[256];*size=0;
  return music_cache_path(kind,key,path,sizeof(path))?NULL:
    music_cache_read_path(path,key,limit,padding,size);
}
/* Availability/commit verification must not allocate another full JPEG.
 * Check the complete identity, bytes, size and checksum with one 4KiB block. */
static bool music_cache_verify_path(const char *path,const char *key,size_t limit,
                                    const void *expected,size_t expected_size)
{
  if(!path||!key||strlen(key)>2047)return false;
  FILE *f=fopen(path,"rb");if(!f)return false;
  uint32_t h[4];struct stat st;unsigned char block[4096];bool valid=false;
  if(fstat(fileno(f),&st)||fread(h,sizeof(h),1,f)!=1||h[0]!=0x3143434d||
     h[1]!=strlen(key)||h[1]>2047||!h[2]||h[2]>limit||
     (uint64_t)st.st_size!=sizeof(h)+(uint64_t)h[1]+h[2]||
     (expected&&h[2]!=expected_size)||fread(block,1,h[1],f)!=h[1]||memcmp(block,key,h[1]))goto done;
  uint32_t hash=music_cache_hash(2166136261u,key,h[1]);size_t offset=0,got;
  while((got=fread(block,1,sizeof(block),f))){
    if(expected&&(got>expected_size-offset||memcmp(block,(const unsigned char *)expected+offset,got)))goto done;
    hash=music_cache_hash(hash,block,got);offset+=got;
  }
  valid=!ferror(f)&&offset==h[2]&&hash==h[3];
done:if(fclose(f))valid=false;return valid;
}
static void music_cache_write_path_impl(const char *path,const char *key,
                                    const void *data,size_t size,size_t limit)
{
  char temporary[264];
  if(!path||!key||strlen(path)>255||strlen(key)>2047||!data||!size||
     size>limit||size>UINT32_MAX)return;
  /* Missing/read-only SD is a cache miss, never a playback failure. */
#ifdef __NuttX__
  mkdir("/sdcard/music",0777);
#endif
  mkdir(MUSIC_CACHE_ROOT,0777);
  snprintf(temporary,sizeof(temporary),"%s.tmp",path);
  FILE *f=fopen(temporary,"wb");if(!f)return;
  uint32_t h[]={0x3143434d,(uint32_t)strlen(key),(uint32_t)size,
    music_cache_hash(music_cache_hash(2166136261u,key,strlen(key)),data,size)};
  bool valid=fwrite(h,sizeof(h),1,f)==1&&fwrite(key,1,h[1],f)==h[1]&&fwrite(data,1,size,f)==size;
  if(fclose(f))valid=false;
  if(!valid||rename(temporary,path))unlink(temporary);
}
static void music_cache_write_path(const char *path,const char *key,
                                    const void *data,size_t size,size_t limit)
{
#if defined(__NuttX__) || defined(MUSIC_CACHE_COORDINATION_TEST)
  if(glass_music_cache_write_enter())return;
#endif
  music_cache_write_path_impl(path,key,data,size,limit);
#if defined(__NuttX__) || defined(MUSIC_CACHE_COORDINATION_TEST)
  glass_music_cache_write_leave();
#endif
}
static void music_cache_write(char kind,const char *key,const void *data,size_t size,size_t limit)
{
  char path[256];
  if(!music_cache_path(kind,key,path,sizeof(path)))
    music_cache_write_path(path,key,data,size,limit);
}
