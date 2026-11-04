/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
/* Lossless native512x300 JPEG pixels. The original fit path doubles each
 * pixel and row; reproduce that mapping one requested scanline at a time. */
#define LOCK_ART_RUN_LIMIT (128u*1024u)
struct lock_art_rle {unsigned page,used,capacity;uint32_t offsets[601];unsigned char *runs;uint16_t *native;};
static unsigned lock_art_storage_bytes(const struct lock_art_rle *art)
{return art->capacity+(art->native?512u*300u*2u:0);}
static void lock_art_rle_release(struct lock_art_rle *art)
{free(art->runs);free(art->native);art->runs=NULL;art->native=NULL;art->capacity=art->used=0;}
/* Takes native ownership on success. The bounded native image is the fallback
 * if compression is unavailable; never allocate a1024x600 pixel buffer. */
static bool lock_art_rle_build(struct lock_art_rle *art,unsigned page,uint16_t *native,unsigned limit)
{
 if(!art||page>=4||!native)return false;memset(art,0,sizeof(*art));art->page=page;art->native=native;
 if(limit>LOCK_ART_RUN_LIMIT)limit=LOCK_ART_RUN_LIMIT;
 if(!limit)return true;art->runs=malloc(limit);if(!art->runs)return true;art->capacity=limit;
 for(unsigned y=0;y<300;y++)for(unsigned phase=0;phase<2;phase++){
  art->offsets[y*2+phase]=art->used;
  for(unsigned x=phase;x<512;){
   uint16_t color=native[y*512+x];unsigned length=1;
   while(length<256&&x+length*2<512&&native[y*512+x+length*2]==color)length++;
   if(art->used>limit||limit-art->used<3){free(art->runs);art->runs=NULL;art->capacity=art->used=0;return true;}
   art->runs[art->used++]=(unsigned char)color;art->runs[art->used++]=(unsigned char)(color>>8);
   art->runs[art->used++]=(unsigned char)(length-1);x+=length*2;
  }
 }
 art->offsets[600]=art->used;free(art->native);art->native=NULL;
 unsigned char *small=realloc(art->runs,art->used);if(small){art->runs=small;art->capacity=art->used;}
 return true;
}
static bool lock_art_rle_row(const struct lock_art_rle *art,unsigned y,unsigned first,unsigned count,uint16_t *out)
{
 if(!art||art->page>=4||y>=600||first>1024||count>1024-first||(!out&&count))return false;
 if(!count)return true;
 if(art->native){for(unsigned x=0;x<count;x++)out[x]=art->native[(y/2)*512+(first+x)/2];return true;}
 if(!art->runs||art->used>art->capacity)return false;uint16_t row[512];
 for(unsigned phase=0;phase<2;phase++){
  unsigned at=art->offsets[(y/2)*2+phase],end=art->offsets[(y/2)*2+phase+1],position=0;
  if(at>end||end>art->used)return false;
  while(at<end){
   if(end-at<3)return false;
   uint16_t color=(uint16_t)(art->runs[at]|(uint16_t)art->runs[at+1]<<8);unsigned length=(unsigned)art->runs[at+2]+1;at+=3;
   if(length>256-position)return false;
   for(unsigned i=position;i<position+length;i++)row[phase+2*i]=color;position+=length;
  }
  if(position!=256)return false;
 }
 for(unsigned x=0;x<count;x++)out[x]=row[(first+x)/2];return true;
}
