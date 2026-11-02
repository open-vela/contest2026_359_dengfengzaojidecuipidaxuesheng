/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_lock_photo.h"
#include "glass_music.h"
#include "qpk_mjpeg.h"
#include "glass_music_cache.h"
#include "glass_music_favorite_cache.h"
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef GLASS_LOCK_PHOTO_PATH
#define GLASS_LOCK_PHOTO_PATH "/sdcard/lockscreen/photo.jpg"
#endif
#define LOCK_JPEG_MAX (512u*1024u)
#define COVER_JPEG_MAX (2u*1024u*1024u)
static pthread_mutex_t photo_lock=PTHREAD_MUTEX_INITIALIZER;
static bool busy;
static struct {
  unsigned generation;bool ready;struct glass_lock_photo result;
} slots[GLASS_COVER_HOME+1];
struct image_job {unsigned owner,generation;char url[1024];uint8_t *input;size_t used,capacity;bool cached,skip_cache;};
static bool image_cancelled(void *ctx)
{
  struct image_job *job=ctx;pthread_mutex_lock(&photo_lock);
  bool cancelled=job->generation!=slots[job->owner].generation;
  pthread_mutex_unlock(&photo_lock);return cancelled;
}
static int image_receive(void *ctx,const unsigned char *data,size_t size)
{
  struct image_job *job=ctx;if(image_cancelled(job))return -ECANCELED;
  size_t limit=job->owner?COVER_JPEG_MAX:LOCK_JPEG_MAX;
  if(size>limit-job->used)return -E2BIG;
  if(job->used+size>job->capacity) {
    size_t capacity=job->capacity;
    while(capacity<job->used+size)capacity*=2;
    if(capacity>limit)capacity=limit;
    uint8_t *next=realloc(job->input,capacity+512);if(!next)return -ENOMEM;
    job->input=next;job->capacity=capacity;
  }
  memcpy(job->input+job->used,data,size);job->used+=size;return 0;
}
static int image_read(struct image_job *job)
{
  if(job->owner) {
    if(!strncmp(job->url,MUSIC_FAVORITE_COVER_SCHEME,
                sizeof(MUSIC_FAVORITE_COVER_SCHEME)-1)) {
      job->input=music_favorite_cover_read(job->url,512,&job->used);
      if(!job->input)return -ENOENT;
      job->cached=true;job->capacity=job->used;return 0;
    }
    if(!job->skip_cache){
      job->input=music_cache_read('c',job->url,COVER_JPEG_MAX,512,&job->used);
      if(job->input){job->cached=true;job->capacity=job->used;return 0;}
    }
    job->capacity=65536;job->input=malloc(job->capacity+512);if(!job->input)return -ENOMEM;
    return music_http_get(job->url,image_receive,job,image_cancelled,NULL);
  }
  FILE *f=fopen(GLASS_LOCK_PHOTO_PATH,"rb");if(!f)return -errno;
  int ret=-EIO;
  if(fseek(f,0,SEEK_END))goto done;
  long size=ftell(f);if(size<=0 || (unsigned long)size>LOCK_JPEG_MAX){ret=-E2BIG;goto done;}
  if(fseek(f,0,SEEK_SET))goto done;
  job->input=malloc((size_t)size+512);if(!job->input){ret=-ENOMEM;goto done;}
  if(fread(job->input,1,size,f)!=(size_t)size)goto done;
  job->used=size;ret=0;
done:
  if(fclose(f) && !ret)ret=-EIO;return ret;
}
static void *photo_worker(void *arg)
{
  struct image_job *job=arg;struct glass_lock_photo out={0};void *decoder=NULL;
retry:
  out.error=image_read(job);if(out.error)goto done;
  unsigned width=0,height=0;out.error=qpk_mjpeg_probe(job->input,job->used,&width,&height);
  if(out.error)goto done;
  if(!width || !height || width>4096 || height>4096 || (uint64_t)width*height>12000000){out.error=-E2BIG;goto done;}
  unsigned maxw=job->owner?512:1024,maxh=job->owner?512:600,scale=0;
  unsigned targetw=job->owner?368:maxw,targeth=job->owner?368:maxh;
  while(scale<3 && ((width>>scale)>targetw || (height>>scale)>targeth))scale++;
  out.width=width>>scale;out.height=height>>scale;
  if(!out.width || !out.height || out.width>maxw || out.height>maxh){out.error=-E2BIG;goto done;}
  if(image_cancelled(job)){out.error=-ECANCELED;goto done;}
  out.pixels=malloc((size_t)out.width*out.height*2);decoder=qpk_mjpeg_create();
  if(!out.pixels || !decoder){out.error=-ENOMEM;goto done;}
  out.error=qpk_mjpeg_decode(decoder,job->input,job->used,job->used+512,out.pixels,out.width,out.height,width,height);
done:
  if(out.error&&job->cached&&!image_cancelled(job)&&
     strncmp(job->url,MUSIC_FAVORITE_COVER_SCHEME,
             sizeof(MUSIC_FAVORITE_COVER_SCHEME)-1)){
    qpk_mjpeg_destroy(decoder);decoder=NULL;free(job->input);job->input=NULL;free(out.pixels);
    out=(struct glass_lock_photo){0};job->used=job->capacity=0;job->cached=false;job->skip_cache=true;goto retry;
  }
  if(!out.error&&job->owner&&!job->cached&&!image_cancelled(job)&&qpk_mjpeg_frame_size(decoder)==job->used)
    music_cache_write('c',job->url,job->input,job->used,COVER_JPEG_MAX);
  qpk_mjpeg_destroy(decoder);free(job->input);
  if(out.error){free(out.pixels);out.pixels=NULL;}
  pthread_mutex_lock(&photo_lock);busy=false;
  if(job->generation==slots[job->owner].generation){slots[job->owner].result=out;out.pixels=NULL;slots[job->owner].ready=true;}
  pthread_mutex_unlock(&photo_lock);free(out.pixels);free(job);return NULL;
}
static int image_request(unsigned owner,const char *url)
{
  char favorite_path[256];
  if(owner>GLASS_COVER_HOME || (owner && (!url || strlen(url)>=1024 ||
      (strncmp(url,"https://",8)&&music_favorite_cover_path(url,favorite_path)))))return -EINVAL;
  pthread_mutex_lock(&photo_lock);
  if(busy){pthread_mutex_unlock(&photo_lock);return -EBUSY;}
  struct image_job *job=calloc(1,sizeof(*job));if(!job){pthread_mutex_unlock(&photo_lock);return -ENOMEM;}
  free(slots[owner].result.pixels);slots[owner].result=(struct glass_lock_photo){0};slots[owner].ready=false;
  job->owner=owner;job->generation=++slots[owner].generation;if(owner)strcpy(job->url,url);
  busy=true;pthread_attr_t attr;pthread_attr_init(&attr);pthread_t thread;
  int ret=pthread_attr_setstacksize(&attr,32768);
  if(!ret)ret=pthread_create(&thread,&attr,photo_worker,job);
  pthread_attr_destroy(&attr);
  if(ret){busy=false;free(job);}else pthread_detach(thread);
  pthread_mutex_unlock(&photo_lock);return ret?-ret:0;
}
static void image_cancel(unsigned owner)
{
  if(owner>GLASS_COVER_HOME)return;
  pthread_mutex_lock(&photo_lock);slots[owner].generation++;slots[owner].ready=false;
  free(slots[owner].result.pixels);slots[owner].result=(struct glass_lock_photo){0};pthread_mutex_unlock(&photo_lock);
}
static bool image_take(unsigned owner,struct glass_lock_photo *out)
{
  if(owner>GLASS_COVER_HOME || !out)return false;
  pthread_mutex_lock(&photo_lock);bool available=slots[owner].ready;
  if(available){*out=slots[owner].result;slots[owner].result=(struct glass_lock_photo){0};slots[owner].ready=false;}
  pthread_mutex_unlock(&photo_lock);return available;
}
int glass_lock_photo_request(void){return image_request(0,NULL);}
void glass_lock_photo_cancel(void){image_cancel(0);}
bool glass_lock_photo_take(struct glass_lock_photo *out){return image_take(0,out);}
int glass_lock_cover_request(unsigned owner,const char *url){return owner>=GLASS_COVER_MUSIC&&owner<=GLASS_COVER_HOME?image_request(owner,url):-EINVAL;}
void glass_lock_cover_cancel(unsigned owner){if(owner>=GLASS_COVER_MUSIC&&owner<=GLASS_COVER_HOME)image_cancel(owner);}
bool glass_lock_cover_take(unsigned owner,struct glass_lock_photo *out){return owner>=GLASS_COVER_MUSIC&&owner<=GLASS_COVER_HOME&&image_take(owner,out);}
