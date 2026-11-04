/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include "glass_music_artwork_cache.h"
int main(void)
{
  struct music_song song={0};strcpy(song.name,"Format round trip");strcpy(song.artist,"Fixture");
  char url[1024],path[256];assert(!music_favorite_cover_url(&song,url));
  struct glass_lock_photo image={.width=64,.height=64},loaded={0};
  image.pixels=malloc(64*64*2);assert(image.pixels);
  uint32_t random=1234567;
  for(unsigned i=0;i<4096;i++){random=random*1664525u+1013904223u;image.pixels[i]=random>>16;}
  assert(music_artwork_write(url,true,&image));assert(music_artwork_read(url,true,&loaded));
  assert(loaded.width==64&&loaded.height==64&&!memcmp(image.pixels,loaded.pixels,8192));free(loaded.pixels);
  assert(!music_artwork_path(url,true,path));size_t size=0;
  unsigned char *encoded=music_cache_read_path(path,url,MUSIC_ARTWORK_LIMIT,0,&size);assert(encoded);
  assert(size==8208&&music_artwork_u32(encoded+12)==0);free(encoded);free(image.pixels);
  image=(struct glass_lock_photo){.width=384,.height=384};image.pixels=malloc(384*384*2);assert(image.pixels);
  for(unsigned i=0;i<384*384;i++){random=random*1664525u+1013904223u;image.pixels[i]=random>>16;}
  assert(music_artwork_write(url,false,&image));assert(music_artwork_read(url,false,&loaded));
  assert(!memcmp(image.pixels,loaded.pixels,384*384*2));free(loaded.pixels);
  assert(!music_artwork_path(url,false,path));encoded=music_cache_read_path(path,url,MUSIC_ARTWORK_LIMIT,0,&size);assert(encoded);
  assert(size==MUSIC_ARTWORK_LIMIT&&music_artwork_u32(encoded+12)==0);free(encoded);
  for(unsigned i=0;i<384*384;i++)image.pixels[i]=0x9876;
  assert(music_artwork_write(url,false,&image));assert(music_artwork_read(url,false,&loaded));
  assert(loaded.width==384&&loaded.height==384&&!memcmp(image.pixels,loaded.pixels,384*384*2));free(loaded.pixels);free(image.pixels);
  assert(!music_artwork_path(url,false,path));encoded=music_cache_read_path(path,url,MUSIC_ARTWORK_LIMIT,0,&size);assert(encoded);
  assert(size==28&&music_artwork_u32(encoded+12)==1);free(encoded);
  /* Checksums are valid: semantic bounds must still reject malicious lengths,
   * dimensions, codecs and runs before exposing or overrunning pixels. */
  unsigned char invalid[20];music_artwork_put(invalid,0x3141354du);music_artwork_put(invalid+4,4);
  music_artwork_put(invalid+8,4);music_artwork_put(invalid+12,1);music_artwork_put(invalid+16,65535u<<16|123u);
  music_cache_write_path(path,url,invalid,sizeof(invalid),MUSIC_ARTWORK_LIMIT);assert(!music_artwork_read(url,false,&loaded));
  music_artwork_put(invalid+16,0);music_cache_write_path(path,url,invalid,sizeof(invalid),MUSIC_ARTWORK_LIMIT);assert(!music_artwork_read(url,false,&loaded));
  music_artwork_put(invalid+4,UINT32_MAX);music_cache_write_path(path,url,invalid,sizeof(invalid),MUSIC_ARTWORK_LIMIT);assert(!music_artwork_read(url,false,&loaded));
  music_artwork_put(invalid+4,4);music_artwork_put(invalid+12,999);music_cache_write_path(path,url,invalid,sizeof(invalid),MUSIC_ARTWORK_LIMIT);assert(!music_artwork_read(url,false,&loaded));
  music_cache_write_path(path,"another song",invalid,sizeof(invalid),MUSIC_ARTWORK_LIMIT);assert(!music_artwork_read(url,false,&loaded));
  music_favorite_cache_forget(&song);assert(access(path,F_OK));
  puts("ARTWORK_FORMAT_RAW_RLE_OK: lossless incompressible/raw and bounded long RLE runs, full identity, malformed sizes/runs/dimensions/codecs, cleanup");
  return 0;
}
