/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "glass_music_cache.h"
#include <errno.h>
#include <sys/statvfs.h>

/* Eight replaceable SD slots, at most 16 MiB each. Never cache audio in Flash.
 * A header, exact URL and checksum distinguish collisions and partial files. */
#define MUSIC_AUDIO_CACHE_MAX (16u * 1024u * 1024u)
#define MUSIC_AUDIO_CACHE_FILE_MAX (64u * 1024u * 1024u)
struct music_audio_cache {
  FILE *file;
  char path[256], temporary[264];
  uint32_t size, hash, limit;
  int error;
  bool admitted;
};

static inline int music_audio_cache_path(const char *key, char *path, size_t capacity)
{
  if (!key || !*key || strlen(key) > 2047) return -EINVAL;
  unsigned slot = music_cache_hash(2166136261u, key, strlen(key)) % 8;
  int n = snprintf(path, capacity, MUSIC_CACHE_ROOT "/a%u.bin", slot);
  return n > 0 && (size_t)n < capacity ? 0 : -ENAMETOOLONG;
}

static inline FILE *music_audio_cache_open_path_limit_cancelled(const char *key,const char *path,size_t limit,
                                                 bool (*cancelled)(void *),void *context)
{
  char identity[2048];
  if (!key || !path || !limit||limit>MUSIC_AUDIO_CACHE_FILE_MAX||strlen(key) > 2047) return NULL;
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  uint32_t h[4]; struct stat st;
  if (fstat(fileno(f), &st) || fread(h, sizeof(h), 1, f) != 1 ||
      h[0] != 0x3141434d || h[1] != strlen(key) || !h[2] ||
      h[2] > limit ||
      (uint64_t)st.st_size != sizeof(h) + (uint64_t)h[1] + h[2] ||
      fread(identity, 1, h[1], f) != h[1] || memcmp(identity, key, h[1])) goto invalid;
  unsigned char bytes[4096]; size_t got;
  uint32_t hash = music_cache_hash(2166136261u, key, h[1]);
  while ((got = fread(bytes, 1, sizeof(bytes), f))) {
    if(cancelled&&cancelled(context))goto invalid;
    hash = music_cache_hash(hash, bytes, got);
  }
  if (ferror(f) || hash != h[3] || fseek(f, sizeof(h) + h[1], SEEK_SET)) goto invalid;
  return f;
invalid:
  fclose(f); return NULL;
}
static inline FILE *music_audio_cache_open_path_cancelled(const char *key,const char *path,
                                                 bool (*cancelled)(void *),void *context)
{return music_audio_cache_open_path_limit_cancelled(key,path,MUSIC_AUDIO_CACHE_MAX,cancelled,context);}
static inline FILE *music_audio_cache_open_path(const char *key,const char *path)
{return music_audio_cache_open_path_cancelled(key,path,NULL,NULL);}

static inline FILE *music_audio_cache_open(const char *key)
{
  char path[256];
  return music_audio_cache_path(key, path, sizeof(path)) ? NULL :
    music_audio_cache_open_path(key, path);
}

static inline void music_audio_cache_release_writer(struct music_audio_cache *c)
{
#if defined(__NuttX__) || defined(MUSIC_CACHE_COORDINATION_TEST)
  if(c->admitted){c->admitted=false;glass_music_cache_write_leave();}
#else
  (void)c;
#endif
}
static inline void music_audio_cache_begin_path_limit(struct music_audio_cache *c,
                                         const char *key,const char *path,size_t limit)
{
  memset(c, 0, sizeof(*c));
  if (!key || !path || !limit||limit>MUSIC_AUDIO_CACHE_FILE_MAX||strlen(key)>2047||strlen(path)>=sizeof(c->path)){c->error=-EINVAL;return;}
  c->limit=limit;
#if defined(__NuttX__) || defined(MUSIC_CACHE_COORDINATION_TEST)
  c->error=glass_music_cache_write_enter();if(c->error)return;c->admitted=true;
#endif
  strcpy(c->path, path);
#ifdef __NuttX__
  mkdir("/sdcard/music", 0777);
#endif
  mkdir(MUSIC_CACHE_ROOT, 0777);
  struct statvfs space;
  if(statvfs(MUSIC_CACHE_ROOT,&space)){c->error=errno?-errno:-EIO;goto denied;}
  if((uint64_t)space.f_bavail*space.f_frsize<limit+1024u*1024u){c->error=-ENOSPC;goto denied;}
  snprintf(c->temporary, sizeof(c->temporary), "%s.tmp", c->path);
  c->file = fopen(c->temporary, "wb");
  if(!c->file){c->error=errno?-errno:-EIO;goto denied;}
  uint32_t h[4] = {0x3141434d, (uint32_t)strlen(key), 0, 0};
  c->hash = music_cache_hash(2166136261u, key, h[1]);
  if (fwrite(h, sizeof(h), 1, c->file) != 1 ||
      fwrite(key, 1, h[1], c->file) != h[1]) {
    c->error=errno?-errno:-EIO;fclose(c->file);c->file=NULL;unlink(c->temporary);goto denied;
  }
  return;
denied:music_audio_cache_release_writer(c);
}
static inline void music_audio_cache_begin_path(struct music_audio_cache *c,const char *key,const char *path)
{music_audio_cache_begin_path_limit(c,key,path,MUSIC_AUDIO_CACHE_MAX);}

static inline void music_audio_cache_begin(struct music_audio_cache *c, const char *key)
{
  char path[256];
  if (music_audio_cache_path(key, path, sizeof(path))) {
    memset(c, 0, sizeof(*c)); return;
  }
  music_audio_cache_begin_path(c, key, path);
}

static inline void music_audio_cache_append(struct music_audio_cache *c,
                                    const unsigned char *bytes, size_t size)
{
  if (!c->file) return;
  uint32_t limit=c->limit?c->limit:MUSIC_AUDIO_CACHE_MAX;
  bool oversize=size>limit-c->size;
  if(oversize||fwrite(bytes,1,size,c->file)!=size) {
    c->error=oversize?-EFBIG:errno?-errno:-EIO;
    fclose(c->file);c->file=NULL;unlink(c->temporary);music_audio_cache_release_writer(c);return;
  }
  c->hash = music_cache_hash(c->hash, bytes, size); c->size += size;
}

static inline void music_audio_cache_finish(struct music_audio_cache *c, bool complete)
{
  if(!c->file){music_audio_cache_release_writer(c);return;}
  uint32_t tail[] = {c->size, c->hash};
  bool valid = complete && c->size && !fseek(c->file, 8, SEEK_SET) &&
               fwrite(tail, sizeof(tail), 1, c->file) == 1;
  if(!valid&&complete&&!c->error)c->error=errno?-errno:-EIO;
  if(fclose(c->file)){valid=false;if(!c->error)c->error=errno?-errno:-EIO;}
  c->file = NULL;
  if(!valid)unlink(c->temporary);
  else if(rename(c->temporary,c->path)){c->error=errno?-errno:-EIO;unlink(c->temporary);}
  music_audio_cache_release_writer(c);
}
