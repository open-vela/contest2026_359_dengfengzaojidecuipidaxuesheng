/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include "glass_lock_photo.c"
static atomic_bool hold_cover,cover_entered;
static atomic_uint downloads;
int music_http_get(const char *url,music_data_cb sink,void *ctx,music_cancel_cb cancel,unsigned *status)
{
 assert(!strncmp(url,"https://fixture/",16));if(status)*status=200;
 atomic_fetch_add(&downloads,1);
 if(strstr(url,"hold")){atomic_store(&cover_entered,true);while(atomic_load(&hold_cover))usleep(1000);}
 FILE *f=fopen(GLASS_LOCK_PHOTO_PATH,"rb");assert(f);unsigned char data[137];size_t n;int ret=0;
 while((n=fread(data,1,sizeof(data),f))){if(cancel(ctx)){ret=-ECANCELED;break;}ret=sink(ctx,data,n);if(ret)break;}
 fclose(f);return ret;
}
static void quiet(void)
{
  for(unsigned i=0;i<4000;i++) {
    pthread_mutex_lock(&photo_lock);bool active=busy;pthread_mutex_unlock(&photo_lock);
    if(!active)return;usleep(1000);
  }
  assert(false);
}
static struct glass_lock_photo take(void)
{
  struct glass_lock_photo out;quiet();assert(glass_lock_photo_take(&out));return out;
}
int main(int argc,char **argv)
{
  if(argc==4 && !strcmp(argv[1],"--export")) {
    FILE *f=fopen(argv[2],"rb");assert(f);fseek(f,0,SEEK_END);size_t size=ftell(f);rewind(f);assert(size<=COVER_JPEG_MAX);
    uint8_t *data=malloc(size+512);assert(data);assert(fread(data,1,size,f)==size);fclose(f);
    unsigned w,h;assert(!qpk_mjpeg_probe(data,size,&w,&h));assert(w<=4096&&h<=4096&&(uint64_t)w*h<=12000000);
    unsigned scale=0;while(scale<3&&((w>>scale)>368||(h>>scale)>368))scale++;
    unsigned dw=w>>scale,dh=h>>scale;assert(dw&&dh&&dw<=512&&dh<=512);
    uint16_t *pixels=malloc(dw*dh*2);void *d=qpk_mjpeg_create();assert(pixels&&d);assert(!qpk_mjpeg_decode(d,data,size,size+512,pixels,dw,dh,w,h));
    f=fopen(argv[3],"wb");assert(f);uint32_t dimensions[]={dw,dh};assert(fwrite(dimensions,4,2,f)==2);assert(fwrite(pixels,2,dw*dh,f)==dw*dh);fclose(f);
    qpk_mjpeg_destroy(d);free(pixels);free(data);printf("LIVE_COVER_JPEG_OK %ux%u -> %ux%u\n",w,h,dw,dh);return 0;
  }
  assert(argc==2);assert(!strcmp(argv[1],GLASS_LOCK_PHOTO_PATH));
  assert(!glass_lock_photo_request());struct glass_lock_photo out=take();
  assert(!out.error && out.pixels && out.width==1024 && out.height==600);
  unsigned long sum=0;for(unsigned i=0;i<1024*600;i++)sum+=out.pixels[i];assert(sum);free(out.pixels);
  assert(glass_lock_cover_request(GLASS_COVER_MUSIC,"http://fixture/x")==-EINVAL);
  assert(!glass_lock_cover_request(GLASS_COVER_MUSIC,"https://fixture/cover"));quiet();assert(!glass_lock_cover_take(GLASS_COVER_LOCK,&out));
  assert(glass_lock_cover_take(GLASS_COVER_MUSIC,&out) && !out.error && out.pixels && out.width<=368 && out.height<=368);free(out.pixels);
  atomic_store(&hold_cover,true);assert(!glass_lock_cover_request(GLASS_COVER_MUSIC,"https://fixture/hold"));
  while(!atomic_load(&cover_entered))usleep(1000);glass_lock_cover_cancel(GLASS_COVER_MUSIC);atomic_store(&hold_cover,false);quiet();
  assert(!glass_lock_cover_take(GLASS_COVER_MUSIC,&out));
  unsigned fetched=atomic_load(&downloads);
  assert(!glass_lock_cover_request(GLASS_COVER_LOCK,"https://fixture/cover"));quiet();assert(glass_lock_cover_take(GLASS_COVER_LOCK,&out) && !out.error);free(out.pixels);
  assert(atomic_load(&downloads)==fetched);
  const char invalid[]="not a JPEG";
  music_cache_write('c',"https://fixture/cover",invalid,sizeof(invalid)-1,COVER_JPEG_MAX);
  assert(!glass_lock_cover_request(GLASS_COVER_LOCK,"https://fixture/cover"));quiet();
  assert(glass_lock_cover_take(GLASS_COVER_LOCK,&out)&&!out.error&&out.pixels);free(out.pixels);
  assert(atomic_load(&downloads)==fetched+1);
  /* The desktop owns a separate result/generation, with one bounded decoder
   * worker shared by all views. Cancelling another view cannot steal its art. */
  assert(glass_lock_cover_request(GLASS_COVER_HOME+1,"https://fixture/cover")==-EINVAL);
  assert(glass_lock_cover_request(0,"https://fixture/cover")==-EINVAL);
  assert(!glass_lock_cover_request(GLASS_COVER_HOME,"https://fixture/cover"));quiet();
  glass_lock_cover_cancel(GLASS_COVER_MUSIC);
  assert(!glass_lock_cover_take(GLASS_COVER_MUSIC,&out));
  assert(glass_lock_cover_take(GLASS_COVER_HOME,&out)&&!out.error&&out.pixels);free(out.pixels);
  struct music_song favorite={0};strcpy(favorite.name,"Offline");strcpy(favorite.artist,"Demo");
  char favorite_url[1024];assert(!music_favorite_cover_url(&favorite,favorite_url));
  FILE *saved=fopen(GLASS_LOCK_PHOTO_PATH,"rb");assert(saved);
  assert(!fseek(saved,0,SEEK_END));long saved_size=ftell(saved);assert(saved_size>4);
  rewind(saved);uint8_t *saved_bytes=malloc(saved_size);assert(saved_bytes);
  assert(fread(saved_bytes,1,saved_size,saved)==(size_t)saved_size);fclose(saved);
  assert(music_favorite_cover_write(&favorite,saved_bytes,saved_size));free(saved_bytes);
  fetched=atomic_load(&downloads);
  assert(!glass_lock_cover_request(GLASS_COVER_MUSIC,favorite_url));quiet();
  assert(glass_lock_cover_take(GLASS_COVER_MUSIC,&out)&&!out.error&&out.pixels);
  free(out.pixels);assert(atomic_load(&downloads)==fetched);
  music_favorite_cache_forget(&favorite);
  atomic_store(&cover_entered,false);atomic_store(&hold_cover,true);
  assert(!glass_lock_cover_request(GLASS_COVER_HOME,"https://fixture/hold-home"));
  while(!atomic_load(&cover_entered))usleep(1000);
  assert(glass_lock_cover_request(GLASS_COVER_MUSIC,"https://fixture/cover")==-EBUSY);
  glass_lock_cover_cancel(GLASS_COVER_HOME);atomic_store(&hold_cover,false);quiet();
  assert(!glass_lock_cover_take(GLASS_COVER_HOME,&out));
  assert(!glass_lock_cover_request(GLASS_COVER_MUSIC,"https://fixture/cover"));quiet();
  assert(glass_lock_cover_take(GLASS_COVER_MUSIC,&out)&&!out.error&&out.pixels);free(out.pixels);
  for(unsigned i=0;i<20;i++){assert(!glass_lock_photo_request());glass_lock_photo_cancel();quiet();assert(!glass_lock_photo_take(&out));}
  assert(!unlink(GLASS_LOCK_PHOTO_PATH));assert(!glass_lock_photo_request());out=take();assert(out.error==-ENOENT && !out.pixels);
  FILE *f=fopen(GLASS_LOCK_PHOTO_PATH,"wb");assert(f);fputs("not a JPEG",f);fclose(f);
  assert(!glass_lock_photo_request());out=take();assert(out.error==-EBADMSG && !out.pixels);
  f=fopen(GLASS_LOCK_PHOTO_PATH,"wb");assert(f);assert(!fseek(f,LOCK_JPEG_MAX,SEEK_SET));fputc(0,f);fclose(f);
  assert(!glass_lock_photo_request());out=take();assert(out.error==-E2BIG && !out.pixels);
  glass_lock_photo_cancel();usleep(10000);
  puts("LOCK_PHOTO_SERVICE_OK: real baseline JPEG decoder, offline favorite cover, RGB565 frame, independent home/music/lock owners, single-worker busy/cancel/retry, 20 cancellation cycles, missing/corrupt/oversized SD file and ownership cleanup");return 0;
}
