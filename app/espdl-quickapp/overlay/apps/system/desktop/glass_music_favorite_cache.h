/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GLASS_MUSIC_FAVORITE_CACHE_H
#define GLASS_MUSIC_FAVORITE_CACHE_H

#include "glass_music.h"
#include "glass_music_audio_cache.h"
#include "glass_music_cache.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MUSIC_FAVORITE_COVER_MAX (2u * 1024u * 1024u)
#define MUSIC_FAVORITE_AUDIO_SCHEME "favorite-audio://"
#define MUSIC_FAVORITE_COVER_SCHEME "favorite-cover://"

/* Two independent 64-bit hashes keep filenames bounded. The cache headers
 * still compare the complete song identity before returning any bytes. */
static inline void music_favorite_id(const struct music_song *song, char id[33])
{
  uint64_t a=14695981039346656037ull,b=7809847782465536322ull;
  const char *parts[]={song->name,song->artist};
  for(unsigned i=0;i<2;i++) {
    const unsigned char *p=(const unsigned char *)parts[i];
    do { a=(a^*p)*1099511628211ull;
         b=(b^*p)*14029467366897019727ull; } while(*p++);
  }
  snprintf(id,33,"%016llx%016llx",(unsigned long long)a,(unsigned long long)b);
}

static inline int music_favorite_key(const struct music_song *song,char key[256])
{
  int n=snprintf(key,256,"%s\n%s",song->name,song->artist);
  return n>0&&n<256?0:-EINVAL;
}

static inline int music_favorite_path(const char *id,char kind,char path[256])
{
  int n=snprintf(path,256,MUSIC_CACHE_ROOT "/f%c%s.bin",kind,id);
  return n>0&&n<256?0:-ENAMETOOLONG;
}

static inline FILE *music_favorite_audio_open(const struct music_song *song)
{
  char id[33],key[256],path[256];
  if(music_favorite_key(song,key))return NULL;
  music_favorite_id(song,id);
  return music_favorite_path(id,'a',path)?NULL:music_audio_cache_open_path(key,path);
}

static inline void music_favorite_audio_begin(struct music_audio_cache *cache,
                                       const struct music_song *song)
{
  char id[33],key[256],path[256];
  if(music_favorite_key(song,key)){memset(cache,0,sizeof(*cache));return;}
  music_favorite_id(song,id);
  if(music_favorite_path(id,'a',path)){memset(cache,0,sizeof(*cache));return;}
  music_audio_cache_begin_path(cache,key,path);
}

static inline int music_favorite_cover_url(const struct music_song *song,char url[1024])
{
  char id[33],key[256];
  if(music_favorite_key(song,key))return -EINVAL;
  music_favorite_id(song,id);
  int n=snprintf(url,1024,MUSIC_FAVORITE_COVER_SCHEME "%s/",id);
  if(n<=0||n>=1024)return -ENAMETOOLONG;
  static const char hex[]="0123456789abcdef";
  for(const unsigned char *p=(const unsigned char *)key;*p;p++) {
    if(n+2>=1024)return -ENAMETOOLONG;
    url[n++]=hex[*p>>4];url[n++]=hex[*p&15];
  }
  url[n]=0;return 0;
}

static inline void music_favorite_audio_url(const struct music_song *song,char url[2048])
{
  char id[33];music_favorite_id(song,id);
  snprintf(url,2048,MUSIC_FAVORITE_AUDIO_SCHEME "%s",id);
}

static inline int music_favorite_cover_path(const char *url,char path[256])
{
  const size_t prefix=sizeof(MUSIC_FAVORITE_COVER_SCHEME)-1;
  if(strncmp(url,MUSIC_FAVORITE_COVER_SCHEME,prefix))
    return -EINVAL;
  size_t length=strlen(url),encoded=length-prefix-33;
  if(length<prefix+35||url[prefix+32]!='/'||!encoded||encoded>510||encoded%2)
    return -EINVAL;
  for(const char *p=url+prefix;p<url+length;p++) {
    if(p==url+prefix+32)continue;
    if(!((*p>='0'&&*p<='9')||(*p>='a'&&*p<='f')))return -EINVAL;
  }
  char id[33];memcpy(id,url+prefix,32);id[32]=0;
  return music_favorite_path(id,'c',path);
}

static inline unsigned char *music_favorite_cover_read(const char *url,size_t padding,size_t *size)
{
  char path[256];*size=0;
  return music_favorite_cover_path(url,path)?NULL:
    music_cache_read_path(path,url,MUSIC_FAVORITE_COVER_MAX,padding,size);
}

static inline bool music_favorite_cover_available(const struct music_song *song)
{
  char url[1024];size_t size=0;
  if(music_favorite_cover_url(song,url))return false;
  unsigned char *bytes=music_favorite_cover_read(url,0,&size);
  bool ready=bytes!=NULL&&size!=0;
  free(bytes);return ready;
}

static inline bool music_favorite_cover_write(const struct music_song *song,
                                        const void *data,size_t size)
{
  char url[1024],path[256];size_t check=0;
  if(music_favorite_cover_url(song,url)||music_favorite_cover_path(url,path))return false;
  music_cache_write_path(path,url,data,size,MUSIC_FAVORITE_COVER_MAX);
  unsigned char *stored=music_favorite_cover_read(url,0,&check);
  bool ready=stored!=NULL&&check==size&&!memcmp(stored,data,size);
  free(stored);return ready;
}

static inline void music_favorite_cache_forget(const struct music_song *song)
{
  char id[33],path[256],temporary[264];music_favorite_id(song,id);
  for(const char *kind="ac";*kind;kind++) {
    if(music_favorite_path(id,*kind,path))continue;
    unlink(path);
    snprintf(temporary,sizeof(temporary),"%s.tmp",path);unlink(temporary);
  }
}
#endif
