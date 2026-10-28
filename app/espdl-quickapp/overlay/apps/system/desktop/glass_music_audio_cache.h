/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "glass_music_cache.h"
#include <errno.h>
#include <sys/statvfs.h>

/* Eight replaceable SD slots, at most 16 MiB each. Never cache audio in Flash.
 * A header, exact URL and checksum distinguish collisions and partial files. */
#define MUSIC_AUDIO_CACHE_MAX (16u * 1024u * 1024u)
struct music_audio_cache {
  FILE *file;
  char path[256], temporary[264];
  uint32_t size, hash;
};

static int music_audio_cache_path(const char *key, char *path, size_t capacity)
{
  if (!key || !*key || strlen(key) > 2047) return -EINVAL;
  unsigned slot = music_cache_hash(2166136261u, key, strlen(key)) % 8;
  int n = snprintf(path, capacity, MUSIC_CACHE_ROOT "/a%u.bin", slot);
  return n > 0 && (size_t)n < capacity ? 0 : -ENAMETOOLONG;
}

static FILE *music_audio_cache_open(const char *key)
{
  char path[256], identity[2048];
  if (music_audio_cache_path(key, path, sizeof(path))) return NULL;
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  uint32_t h[4]; struct stat st;
  if (fstat(fileno(f), &st) || fread(h, sizeof(h), 1, f) != 1 ||
      h[0] != 0x3141434d || h[1] != strlen(key) || !h[2] ||
      h[2] > MUSIC_AUDIO_CACHE_MAX ||
      (uint64_t)st.st_size != sizeof(h) + (uint64_t)h[1] + h[2] ||
      fread(identity, 1, h[1], f) != h[1] || memcmp(identity, key, h[1])) goto invalid;
  unsigned char bytes[4096]; size_t got;
  uint32_t hash = music_cache_hash(2166136261u, key, h[1]);
  while ((got = fread(bytes, 1, sizeof(bytes), f)))
    hash = music_cache_hash(hash, bytes, got);
  if (ferror(f) || hash != h[3] || fseek(f, sizeof(h) + h[1], SEEK_SET)) goto invalid;
  return f;
invalid:
  fclose(f); return NULL;
}

static void music_audio_cache_begin(struct music_audio_cache *c, const char *key)
{
  memset(c, 0, sizeof(*c));
  if (music_audio_cache_path(key, c->path, sizeof(c->path))) return;
#ifdef __NuttX__
  mkdir("/sdcard/music", 0777);
#endif
  mkdir(MUSIC_CACHE_ROOT, 0777);
  struct statvfs space;
  if (statvfs(MUSIC_CACHE_ROOT, &space) ||
      (uint64_t)space.f_bavail * space.f_frsize < MUSIC_AUDIO_CACHE_MAX + 1024u * 1024u) return;
  snprintf(c->temporary, sizeof(c->temporary), "%s.tmp", c->path);
  c->file = fopen(c->temporary, "wb");
  if (!c->file) return;
  uint32_t h[4] = {0x3141434d, (uint32_t)strlen(key), 0, 0};
  c->hash = music_cache_hash(2166136261u, key, h[1]);
  if (fwrite(h, sizeof(h), 1, c->file) != 1 ||
      fwrite(key, 1, h[1], c->file) != h[1]) {
    fclose(c->file); c->file = NULL; unlink(c->temporary);
  }
}

static void music_audio_cache_append(struct music_audio_cache *c,
                                    const unsigned char *bytes, size_t size)
{
  if (!c->file) return;
  if (size > MUSIC_AUDIO_CACHE_MAX - c->size || fwrite(bytes, 1, size, c->file) != size) {
    fclose(c->file); c->file = NULL; unlink(c->temporary); return;
  }
  c->hash = music_cache_hash(c->hash, bytes, size); c->size += size;
}

static void music_audio_cache_finish(struct music_audio_cache *c, bool complete)
{
  if (!c->file) return;
  uint32_t tail[] = {c->size, c->hash};
  bool valid = complete && c->size && !fseek(c->file, 8, SEEK_SET) &&
               fwrite(tail, sizeof(tail), 1, c->file) == 1;
  if (fclose(c->file)) valid = false;
  c->file = NULL;
  if (!valid || rename(c->temporary, c->path)) unlink(c->temporary);
}
