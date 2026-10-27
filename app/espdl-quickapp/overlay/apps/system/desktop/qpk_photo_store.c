/* SPDX-License-Identifier: Apache-2.0 */
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "qpk_photo_store.h"
#include "qpk_photo_storage.h"
#include "camera_color.h"
#include "camera_frame.h"
#include "qpk_jpeg_hw.h"
#if defined(__NuttX__) && defined(CONFIG_ESPRESSIF_MIPI_DSI)
#include <nuttx/video/camera_timing.h>
#endif

enum state { EMPTY, COPYING, QUEUED, WRITING, COMPLETE };
struct job {
  enum state state;
  bool cancel;
  struct qpk_photo_input input;
  struct qpk_photo_result result;
  void *owned;
  char root[128];
};
static struct job jobs[QPK_PHOTO_OWNERS];
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake=PTHREAD_COND_INITIALIZER;
static pthread_t thread;
static bool ready,stopping;
static uint32_t sequence;

static uint64_t now_us(void)
{
#if defined(__NuttX__) && defined(CONFIG_ESPRESSIF_MIPI_DSI)
  return esp_camera_timing_now();
#else
  struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
  return (uint64_t)ts.tv_sec*1000000+ts.tv_nsec/1000;
#endif
}
static bool cancelled(unsigned owner,uint32_t ticket)
{
  pthread_mutex_lock(&lock);
  bool value=stopping||jobs[owner].result.ticket!=ticket||jobs[owner].cancel;
  pthread_mutex_unlock(&lock);return value;
}
static void little32(uint8_t *out,uint32_t value)
{for(unsigned i=0;i<4;i++)out[i]=(uint8_t)(value>>(i*8));}

static int encode_photo(unsigned owner,const struct job *job,unsigned width,
                         unsigned height,uint8_t **jpeg,size_t *size)
{
#ifdef CONFIG_ESP32P4_JPEG
  const struct qpk_photo_input *in=&job->input;
  uint16_t *frame=malloc((size_t)width*height*2);
  if(!frame)return -ENOMEM;
  struct camera_reference_rect crop=camera_frame_crop(in->width,in->height,
    in->image_width,in->image_height,in->zoom,in->aspect);
  const uint16_t *source=job->owned;
  int ret=0;
  for(unsigned y=0;y<height;y++){
    if(cancelled(owner,job->result.ticket)){ret=-ECANCELED;break;}
    for(unsigned x=0;x<width;x++)
      frame[(size_t)y*width+x]=camera_color_pixel(source[
        (size_t)(crop.y+y*crop.h/height)*in->stride+crop.x+x*crop.w/width],in->style);
  }
  if(!ret)ret=qpk_jpeg_hw_encode565(frame,width,height,width,90,jpeg,size);
  free(frame);
  if(!ret&&cancelled(owner,job->result.ticket)){free(*jpeg);*jpeg=NULL;*size=0;ret=-ECANCELED;}
  return ret;
#else
  (void)owner;(void)job;(void)width;(void)height;(void)jpeg;(void)size;
  return -ENOSYS;
#endif
}

static int write_photo(unsigned owner,const struct job *job,char *path)
{
  const struct qpk_photo_input *in=&job->input;
  if(cancelled(owner,job->result.ticket))return -ECANCELED;
  bool encoding=in->format==QPK_PHOTO_JPEG565;
  bool jpeg_input=in->format==QPK_PHOTO_JPEG;
  unsigned width=in->format==QPK_PHOTO_BMP565||encoding?512:in->width;
  unsigned height=in->format==QPK_PHOTO_BMP565||encoding?in->height*512/in->width:in->height;
  if(in->aspect)height=camera_aspect_height(width,in->aspect);
  size_t row_bytes=(width*(in->format==QPK_PHOTO_BMP24?3:2)+3)&~3u;
  size_t expected=in->format==QPK_PHOTO_JPEG?in->bytes:
    (in->format==QPK_PHOTO_BMP24?54:66)+row_bytes*height;
  if(encoding)expected=(size_t)((width+15)&~15u)*((height+15)&~15u)*6+4096;
  char directory[128];
  int ret=qpk_photo_directory(directory,sizeof(directory),expected,
                               job->root[0]?job->root:NULL);
  if(ret)return ret;
  if(cancelled(owner,job->result.ticket))return -ECANCELED;
  uint8_t *encoded=NULL;size_t encoded_size=0;
  if(encoding){ret=encode_photo(owner,job,width,height,&encoded,&encoded_size);if(ret)return ret;expected=encoded_size;}
  /* Encoding delays file open: keep the selected SD medium and reject an
   * unplug instead of accidentally opening a stale mount-point directory.
   */
  if(encoding&&!job->root[0]&&!strcmp(directory,QPK_PHOTO_SD_ROOT)){
    ret=qpk_volume_mounted(QPK_VOLUME_SD);if(ret){free(encoded);return ret;}
  }
  const char *extension=jpeg_input||encoding?"jpg":"bmp";
  FILE *file=NULL;
  for(unsigned i=0;i<1000;i++){
    snprintf(path,128,"%.95s/photo-%08lu-%u.%s",directory,
             (unsigned long)job->result.ticket,i,extension);
    file=fopen(path,"wbx");if(file||errno!=EEXIST)break;
  }
  if(!file){free(encoded);return -(errno?errno:EIO);}
  printf("[photo-io] begin owner=%u ticket=%lu bytes=%lu directory=%s\n",
         owner,(unsigned long)job->result.ticket,(unsigned long)expected,directory);
  if(jpeg_input||encoding){
    size_t bytes=encoding?encoded_size:in->bytes;
    const uint8_t *data=encoding?encoded:job->owned;
    for(size_t offset=0;offset<bytes;){
      if(cancelled(owner,job->result.ticket)){ret=-ECANCELED;break;}
      size_t count=bytes-offset;if(count>4096)count=4096;
      if(fwrite(data+offset,1,count,file)!=count){ret=-EIO;break;}
      offset+=count;
    }
  }else{
    uint8_t header[66]={0},row[1536];
    unsigned header_bytes=in->format==QPK_PHOTO_BMP24?54:66;
    header[0]='B';header[1]='M';header[26]=1;
    header[28]=in->format==QPK_PHOTO_BMP24?24:16;
    little32(header+2,expected);little32(header+10,header_bytes);little32(header+14,40);
    little32(header+18,width);little32(header+22,(uint32_t)-(int)height);
    little32(header+34,row_bytes*height);
    if(header_bytes==66){little32(header+30,3);little32(header+54,0xf800);little32(header+58,0x7e0);little32(header+62,0x1f);}
    if(fwrite(header,1,header_bytes,file)!=header_bytes)ret=-EIO;
    struct camera_reference_rect crop=camera_frame_crop(in->width,in->height,
      in->image_width,in->image_height,in->zoom,in->aspect);
    unsigned cropw=crop.w,croph=crop.h,cropx=crop.x,cropy=crop.y;
    const uint16_t *pixels=job->owned;
    for(unsigned y=0;!ret&&y<height;y++){
      if(cancelled(owner,job->result.ticket)){ret=-ECANCELED;break;}
      memset(row,0,row_bytes);
      for(unsigned x=0;x<width;x++){
        uint16_t pixel=camera_color_pixel(pixels[(cropy+y*croph/height)*in->stride+cropx+x*cropw/width],in->style);
        if(header_bytes==66){row[x*2]=pixel;row[x*2+1]=pixel>>8;}
        else{row[x*3]=(pixel&31)*255/31;row[x*3+1]=((pixel>>5)&63)*255/63;row[x*3+2]=(pixel>>11)*255/31;}
      }
      if(fwrite(row,1,row_bytes,file)!=row_bytes)ret=-EIO;
    }
  }
  if(!ret&&cancelled(owner,job->result.ticket))ret=-ECANCELED;
  if(!ret&&(fflush(file)||fsync(fileno(file))))ret=-EIO;
  if(fclose(file)&&!ret)ret=-EIO;
  /* A cancellation during fsync must never publish a successful receipt. */
  if(!ret&&cancelled(owner,job->result.ticket))ret=-ECANCELED;
  if(ret){unlink(path);path[0]=0;}
  free(encoded);
  return ret;
}
static void *save_worker(void *unused)
{
  (void)unused;
  pthread_mutex_lock(&lock);
  unsigned next_owner=0;
  for(;;){
    unsigned owner=QPK_PHOTO_OWNERS;
    for(unsigned n=0;n<QPK_PHOTO_OWNERS;n++){
      unsigned candidate=(next_owner+n)%QPK_PHOTO_OWNERS;
      if(jobs[candidate].state==QUEUED){owner=candidate;break;}
    }
    if(owner==QPK_PHOTO_OWNERS){
      bool copying=false;for(unsigned i=0;i<QPK_PHOTO_OWNERS;i++)copying|=jobs[i].state==COPYING;
      if(stopping&&!copying)break;
      pthread_cond_wait(&wake,&lock);continue;
    }
    jobs[owner].state=WRITING;
    next_owner=(owner+1)%QPK_PHOTO_OWNERS;
    struct job work=jobs[owner];
    pthread_mutex_unlock(&lock);
    char path[128]={0};uint64_t started=now_us();
    int error=write_photo(owner,&work,path);
    uint32_t elapsed=(uint32_t)(now_us()-started);
    free(work.owned);
    pthread_mutex_lock(&lock);
    jobs[owner].owned=NULL;
    /* Close the cancellation race between file close and publication. */
    if(!error&&(jobs[owner].cancel||stopping)){
      pthread_mutex_unlock(&lock);unlink(path);path[0]=0;
      pthread_mutex_lock(&lock);error=-ECANCELED;
    }
    jobs[owner].result.error=error;jobs[owner].result.write_us=elapsed;
    snprintf(jobs[owner].result.path,128,"%s",error?"":path);
    jobs[owner].result.done=true;jobs[owner].state=COMPLETE;
    pthread_mutex_unlock(&lock);
    printf("[photo-io] done owner=%u ticket=%lu error=%d copy_us=%lu write_us=%lu\n",
           owner,(unsigned long)work.result.ticket,error,
           (unsigned long)work.result.copy_us,(unsigned long)elapsed);
    pthread_mutex_lock(&lock);
  }
  pthread_mutex_unlock(&lock);return NULL;
}
int qpk_photo_store_initialize(void)
{
  pthread_mutex_lock(&lock);
  int ret=0;
  if(!ready){
    stopping=false;pthread_attr_t attr;ret=pthread_attr_init(&attr);
    if(!ret){
#ifdef __NuttX__
      ret=pthread_attr_setstacksize(&attr,16384);
#else
      ret=pthread_attr_setstacksize(&attr,65536);
#endif
      if(!ret)ret=pthread_create(&thread,&attr,save_worker,NULL);
      pthread_attr_destroy(&attr);
    }
    if(!ret)ready=true;
  }
  pthread_mutex_unlock(&lock);return -ret;
}
int qpk_photo_store_submit(enum qpk_photo_owner owner,const struct qpk_photo_input *in,uint32_t *ticket)
{
  if((unsigned)owner>=QPK_PHOTO_OWNERS||!in||!ticket||!in->data||!in->bytes||
     in->bytes>QPK_PHOTO_COPY_MAX||(unsigned)in->format>QPK_PHOTO_JPEG565||
     in->aspect>CAMERA_ASPECT_COUNT||(in->format==QPK_PHOTO_JPEG&&in->aspect)||
     in->image_width>4096||in->image_height>4096||(!in->image_width!=!in->image_height))return -EINVAL;
  if(in->format!=QPK_PHOTO_JPEG &&
     (!in->width||in->width>512*8||!in->height||in->height>2048||in->stride<in->width||
      in->stride>4096||in->bytes!=(size_t)in->stride*in->height*2||
      in->zoom<100||in->zoom>300||in->style>=CAMERA_COLOR_STYLE_COUNT||
      (in->format==QPK_PHOTO_BMP24&&in->width>512)||
      ((in->format==QPK_PHOTO_BMP565||in->format==QPK_PHOTO_JPEG565)&&
       (!(in->height*512/in->width)||in->height*512/in->width>2048))))return -EINVAL;
  if(in->root_override&&strlen(in->root_override)>95)return -ENAMETOOLONG;
  pthread_mutex_lock(&lock);
  if(!ready||stopping){pthread_mutex_unlock(&lock);return -EAGAIN;}
  struct job *job=&jobs[owner];
  if(job->state!=EMPTY&&job->state!=COMPLETE){pthread_mutex_unlock(&lock);return -EBUSY;}
  memset(job,0,sizeof(*job));job->state=COPYING;job->input=*in;
  if(!++sequence)++sequence;
  job->result.ticket=sequence;*ticket=sequence;
  if(in->root_override)snprintf(job->root,sizeof(job->root),"%s",in->root_override);
  job->input.data=NULL;job->input.root_override=NULL;
  pthread_mutex_unlock(&lock);
  uint64_t started=now_us();void *copy=malloc(in->bytes);
  if(copy)memcpy(copy,in->data,in->bytes);
  uint32_t copy_us=(uint32_t)(now_us()-started);
  pthread_mutex_lock(&lock);
  job->result.copy_us=copy_us;
  if(!copy){job->result.error=-ENOMEM;job->result.done=true;job->state=COMPLETE;}
  else{job->owned=copy;job->state=QUEUED;}
  pthread_cond_signal(&wake);pthread_mutex_unlock(&lock);
  return 0;
}
int qpk_photo_store_status(enum qpk_photo_owner owner,uint32_t ticket,struct qpk_photo_result *result)
{
  if((unsigned)owner>=QPK_PHOTO_OWNERS||!ticket||!result)return -EINVAL;
  pthread_mutex_lock(&lock);
  int ret=jobs[owner].result.ticket==ticket?0:-ESTALE;
  if(!ret)*result=jobs[owner].result;
  pthread_mutex_unlock(&lock);return ret;
}
void qpk_photo_store_cancel(enum qpk_photo_owner owner,uint32_t ticket)
{
  if((unsigned)owner>=QPK_PHOTO_OWNERS)return;
  pthread_mutex_lock(&lock);
  if(jobs[owner].result.ticket==ticket&&jobs[owner].state!=COMPLETE)jobs[owner].cancel=true;
  pthread_mutex_unlock(&lock);
}
void qpk_photo_store_shutdown(void)
{
  pthread_mutex_lock(&lock);bool join=ready;stopping=true;pthread_cond_broadcast(&wake);pthread_mutex_unlock(&lock);
  if(join)pthread_join(thread,NULL);
  pthread_mutex_lock(&lock);ready=false;pthread_mutex_unlock(&lock);
}
