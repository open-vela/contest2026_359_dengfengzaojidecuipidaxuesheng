/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "glass_lock_photo.h"
#include "glass_music_favorite_cache.h"

/* Display-sized, persistent RGB565 assets. The outer cache verifies the full
 * song/URL identity and checksum. RLE is used only when smaller than raw RGB.
 * Originals remain available to repair an interrupted/corrupt conversion. */
#define MUSIC_ARTWORK_EDGE 384u
#define MUSIC_ARTWORK_THUMB_EDGE 64u
#define MUSIC_ARTWORK_LIMIT (16u+MUSIC_ARTWORK_EDGE*MUSIC_ARTWORK_EDGE*2u)
static inline uint32_t music_artwork_u32(const unsigned char *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static inline void music_artwork_put(unsigned char *p,uint32_t v)
{for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(v>>(i*8));}
static inline int music_artwork_path(const char *url,bool thumbnail,char path[256])
{
  int ret;
  if(!strncmp(url,MUSIC_FAVORITE_COVER_SCHEME,sizeof(MUSIC_FAVORITE_COVER_SCHEME)-1)){
    ret=music_favorite_cover_path(url,path);
    if(!ret)path[strlen(MUSIC_CACHE_ROOT)+2]=thumbnail?'t':'m';
  }else{
    ret=music_cache_path('c',url,path,256);
    if(!ret)path[strlen(MUSIC_CACHE_ROOT)+1]=thumbnail?'t':'m';
  }
  return ret;
}
static inline bool music_artwork_read(const char *url,bool thumbnail,struct glass_lock_photo *out)
{
  char path[256];size_t size=0;unsigned edge=thumbnail?MUSIC_ARTWORK_THUMB_EDGE:MUSIC_ARTWORK_EDGE;
  if(music_artwork_path(url,thumbnail,path))return false;
  unsigned char *data=music_cache_read_path(path,url,16u+edge*edge*2u,0,&size);
  if(!data)return false;
  uint16_t *pixels=NULL;bool valid=false;
  if(size<16||music_artwork_u32(data)!=0x3141354du)goto done;
  unsigned w=music_artwork_u32(data+4),h=music_artwork_u32(data+8),codec=music_artwork_u32(data+12);
  if(!w||!h||w>edge||h>edge)goto done;
  size_t count=(size_t)w*h;
  pixels=malloc(count*2);if(!pixels)goto done;
  if(codec==0){
    if(size!=16+count*2)goto done;
    for(size_t i=0;i<count;i++)pixels[i]=(uint16_t)data[16+i*2]|(uint16_t)data[17+i*2]<<8;
  }else if(codec==1){
    if((size-16)%4)goto done;
    size_t filled=0;
    for(size_t offset=16;offset<size;offset+=4){
      uint32_t pair=music_artwork_u32(data+offset);unsigned run=pair>>16;
      if(!run||run>count-filled)goto done;
      for(unsigned i=0;i<run;i++)pixels[filled++]=(uint16_t)pair;
    }
    if(filled!=count)goto done;
  }else goto done;
  *out=(struct glass_lock_photo){.pixels=pixels,.width=w,.height=h};pixels=NULL;valid=true;
done:free(pixels);free(data);return valid;
}
static inline int music_artwork_scale(struct glass_lock_photo *image,unsigned edge)
{
  unsigned w=image->width,h=image->height;
  if(w<=edge&&h<=edge)return 0;
  unsigned dw=w>=h?edge:(uint64_t)w*edge/h;
  unsigned dh=h>=w?edge:(uint64_t)h*edge/w;
  if(!dw)dw=1;if(!dh)dh=1;
  uint16_t *small=malloc((size_t)dw*dh*2);if(!small)return -ENOMEM;
  for(unsigned y=0;y<dh;y++)for(unsigned x=0;x<dw;x++)
    small[(size_t)y*dw+x]=image->pixels[(size_t)(y*h/dh)*w+x*w/dw];
  free(image->pixels);image->pixels=small;image->width=dw;image->height=dh;return 0;
}
static inline int music_artwork_thumbnail(const struct glass_lock_photo *source,struct glass_lock_photo *out)
{
  unsigned w=source->width,h=source->height,edge=MUSIC_ARTWORK_THUMB_EDGE;
  if(!source->pixels||!w||!h)return -EINVAL;
  unsigned dw=w<=edge&&h<=edge?w:w>=h?edge:(uint64_t)w*edge/h;
  unsigned dh=w<=edge&&h<=edge?h:h>=w?edge:(uint64_t)h*edge/w;
  if(!dw)dw=1;if(!dh)dh=1;
  uint16_t *pixels=malloc((size_t)dw*dh*2);if(!pixels)return -ENOMEM;
  for(unsigned y=0;y<dh;y++)for(unsigned x=0;x<dw;x++)
    pixels[(size_t)y*dw+x]=source->pixels[(size_t)(y*h/dh)*w+x*w/dw];
  *out=(struct glass_lock_photo){.pixels=pixels,.width=dw,.height=dh};return 0;
}
static inline bool music_artwork_write(const char *url,bool thumbnail,const struct glass_lock_photo *image)
{
  char path[256];unsigned edge=thumbnail?MUSIC_ARTWORK_THUMB_EDGE:MUSIC_ARTWORK_EDGE;
  if(!image->pixels||!image->width||!image->height||image->width>edge||image->height>edge||
     music_artwork_path(url,thumbnail,path))return false;
  size_t count=(size_t)image->width*image->height,runs=0;
  for(size_t i=0;i<count;){size_t n=1;while(n<65535&&i+n<count&&image->pixels[i+n]==image->pixels[i])n++;runs++;i+=n;}
  bool rle=runs*4<count*2;size_t size=16+(rle?runs*4:count*2);
  unsigned char *data=malloc(size);if(!data)return false;
  music_artwork_put(data,0x3141354du);music_artwork_put(data+4,image->width);
  music_artwork_put(data+8,image->height);music_artwork_put(data+12,rle?1:0);
  if(rle){
    size_t offset=16;
    for(size_t i=0;i<count;){unsigned n=1;while(n<65535&&i+n<count&&image->pixels[i+n]==image->pixels[i])n++;
      music_artwork_put(data+offset,(uint32_t)n<<16|image->pixels[i]);offset+=4;i+=n;}
  }else for(size_t i=0;i<count;i++){data[16+i*2]=image->pixels[i];data[17+i*2]=image->pixels[i]>>8;}
  music_cache_write_path(path,url,data,size,16u+edge*edge*2u);
  bool valid=music_cache_verify_path(path,url,16u+edge*edge*2u,data,size);free(data);return valid;
}
static inline bool music_artwork_available(const struct music_song *song)
{
  char url[1024];if(music_favorite_cover_url(song,url))return false;
  for(unsigned i=0;i<2;i++){
    struct glass_lock_photo image;
    if(!music_artwork_read(url,i!=0,&image))return false;
    free(image.pixels);
  }
  return true;
}
