/* SPDX-License-Identifier: Apache-2.0 */
#include "local_video_media.h"
#include "bili_decoder.h"
#include "bili_cpu.h"
#include "bili_late_pacer.h"
#include "aacdec.h"
#include "qpk_mjpeg.h"
#include <strings.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

/* MP4 index allocations are capped across the one active local session. */
#define INDEX_BUDGET (4u * 1024u * 1024u)
static atomic_size_t index_used;
union index_header { max_align_t align; size_t bytes; };
static void *index_alloc(size_t n)
{
  if (n > INDEX_BUDGET-sizeof(union index_header)) return NULL;
  size_t total=n+sizeof(union index_header);
  size_t old=atomic_load(&index_used);
  do { if (old>INDEX_BUDGET-total) return NULL; }
  while (!atomic_compare_exchange_weak(&index_used,&old,old+total));
  union index_header *p=malloc(total);
  if (!p) { atomic_fetch_sub(&index_used,total);return NULL; }
  p->bytes=total;return p+1;
}
static void index_free(void *ptr)
{
  if (!ptr)return;
  union index_header *p=(union index_header *)ptr-1;
  atomic_fetch_sub(&index_used,p->bytes);free(p);
}
static void *index_realloc(void *p,size_t n)
{
  if (!p)return index_alloc(n);
  if (!n){index_free(p);return NULL;}
  size_t previous=((union index_header *)p-1)->bytes-sizeof(union index_header);
  void *next=index_alloc(n);
  if(next){memcpy(next,p,n<previous?n:previous);index_free(p);}return next;
}
#define malloc index_alloc
#define free index_free
#define realloc index_realloc
#define MINIMP4_IMPLEMENTATION
#include "minimp4.h"
#undef malloc
#undef free
#undef realloc

#define PACKET_LIMIT (1024u*1024u)
#define READ_CACHE_SIZE (64u*1024u)
struct file_reader {FILE *file;uint8_t *cache;uint64_t offset;size_t count;unsigned fills;};
struct local_video {
  bool benchmark;
  uint32_t benchmark_hash;
  char path[512];
  struct local_video_ops ops;
  void *opaque;
  atomic_bool cancel, pause, audio_ready, audio_done;
  atomic_uint volume, audio_ms;
  atomic_int audio_error;
  pthread_mutex_t lock;
  struct local_video_snapshot state;
  MP4D_demux_t mp4;
  FILE *file;
  struct file_reader reader;
  int video_track, audio_track;
  uint64_t size, start_ms, pause_started, pause_total;
  struct bili_late_pacer late_pacer;
  void *decoder;
  uint64_t callback_us, render_us, read_us, decode_us;
};
static uint64_t now_ms(void)
{
  struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
  return (uint64_t)ts.tv_sec*1000+ts.tv_nsec/1000000;
}
static uint64_t now_us(void)
{
  struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
  return (uint64_t)ts.tv_sec*1000000+ts.tv_nsec/1000;
}
static int cached_read(struct local_video *s,struct file_reader *r,uint64_t offset,void *buf,size_t n)
{
  if(atomic_load(&s->cancel)||offset>s->size||n>s->size-offset)return -EIO;
  uint8_t *dst=buf;
  while(n){
    if(atomic_load(&s->cancel))return -ECANCELED;
    if(!r->count||offset<r->offset||offset-r->offset>=r->count){
      size_t bytes=s->size-offset<READ_CACHE_SIZE?(size_t)(s->size-offset):READ_CACHE_SIZE;
      if(fseeko(r->file,(off_t)offset,SEEK_SET))return -EIO;
      r->count=fread(r->cache,1,bytes,r->file);r->offset=offset;r->fills++;
      if(r->count!=bytes)return -EIO;
    }
    size_t inside=(size_t)(offset-r->offset),take=r->count-inside;
    if(take>n)take=n;
    memcpy(dst,r->cache+inside,take);dst+=take;offset+=take;n-=take;
  }
  return 0;
}
static int read_at(int64_t offset,void *buf,size_t n,void *opaque)
{
  struct local_video *s=opaque;
  return offset<0||cached_read(s,&s->reader,(uint64_t)offset,buf,n)!=0;
}
static int packet(struct local_video *s,struct file_reader *reader,int track,unsigned sample,uint8_t *p,unsigned *n)
{
  unsigned ts,duration;
  MP4D_file_offset_t offset=MP4D_frame_offset(&s->mp4,track,sample,n,&ts,&duration);
  if(!*n||*n>PACKET_LIMIT||(uint64_t)offset>s->size||*n>s->size-(uint64_t)offset)return -EPROTO;
  return cached_read(s,reader,offset,p,*n);
}
static int64_t sample_ms(MP4D_track_t *t,unsigned i)
{
  int64_t pts=t->timestamp[i];
  if(t->composition)pts+=t->composition[i];
  return pts*1000/t->timescale;
}
static int paused_wait(struct local_video *s)
{
  while(atomic_load(&s->pause)&&!atomic_load(&s->cancel))usleep(10000);
  return atomic_load(&s->cancel)?-ECANCELED:0;
}
static int timed_frame(void *opaque,const uint8_t *const planes[3],const int strides[2],int w,int h,uint64_t pts)
{
  struct local_video *s=opaque;
  uint64_t callback_started=now_us();
  pthread_mutex_lock(&s->lock);s->state.decoded++;pthread_mutex_unlock(&s->lock);
  if(s->benchmark){
    for(int plane=0;plane<3;plane++){
      int pw=plane?(w+1)/2:w,ph=plane?(h+1)/2:h;
      int stride=plane?strides[1]:strides[0];
      for(int y=0;y<ph;y++)for(int x=0;x<pw;x++)
        s->benchmark_hash=(s->benchmark_hash^planes[plane][y*stride+x])*UINT32_C(16777619);
    }
    s->state.frames++;
    s->callback_us+=now_us()-callback_started;
    return 0;
  }
  if(paused_wait(s))return -ECANCELED;
  unsigned clock;
  for(;;){
    if(atomic_load(&s->cancel))return -ECANCELED;
    if(paused_wait(s))return -ECANCELED;
    if(s->audio_track>=0){
      if(atomic_load(&s->audio_error))return atomic_load(&s->audio_error);
      if(!atomic_load(&s->audio_ready)){usleep(5000);continue;}
      clock=atomic_load(&s->audio_ms);
      if(atomic_load(&s->audio_done)&&pts>clock+1000)return -ERANGE;
    }else{
      pthread_mutex_lock(&s->lock);
      clock=now_ms()-s->start_ms-s->pause_total;
      pthread_mutex_unlock(&s->lock);
    }
    if(pts<=clock+20 || (s->audio_track>=0&&atomic_load(&s->audio_done)))break;
    usleep(5000);
  }
  /* Keep rendering when software decode cannot catch the audio clock.
   * Dropping every late picture starves the viewport indefinitely. */
  uint64_t display_now=now_ms();
  bool late=(uint64_t)clock>pts+150;
  bool drop=bili_late_drop(&s->late_pacer,display_now,late,bili_cpu_frame_period_us()/1000);
  uint64_t render_started=now_us();
  int ret=drop?0:s->ops.frame(s->opaque,planes,strides,w,h);
  s->render_us+=now_us()-render_started;
  pthread_mutex_lock(&s->lock);
  if(drop)s->state.dropped++;else s->state.frames++;
  s->state.video_ms=pts;
  s->state.late_ms=clock>pts?clock-pts:0;
  s->state.position_ms=clock;
  pthread_mutex_unlock(&s->lock);
  s->callback_us+=now_us()-callback_started;
  return ret;
}
static const unsigned rates[]={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
static int aac_config(MP4D_track_t *t,AACFrameInfo *info)
{
  if(t->dsi_bytes<2)return -ENOTSUP;
  unsigned aot=t->dsi[0]>>3,index=((t->dsi[0]&7)<<1)|(t->dsi[1]>>7),channels=(t->dsi[1]>>3)&15;
  /* AAC-LC GASpecificConfig: 1024 samples, no core dependency/extension. */
  if(aot!=2||index>=13||rates[index]<8000||rates[index]>48000||channels<1||channels>2||(t->dsi[1]&7))return -ENOTSUP;
  memset(info,0,sizeof(*info));info->nChans=channels;info->sampRateCore=rates[index];info->sampRateOut=rates[index];info->profile=1;
  return 0;
}
static void *audio_worker(void *opaque)
{
  bili_cpu_audio_begin();
  struct local_video *s=opaque;MP4D_track_t *t=&s->mp4.track[s->audio_track];
  FILE *fp=fopen(s->path,"rb");HAACDecoder dec=AACInitDecoder();
  struct file_reader reader={.file=fp,.cache=malloc(READ_CACHE_SIZE)};
  uint8_t *input=malloc(PACKET_LIMIT+64);int16_t *pcm=malloc(4096*sizeof(int16_t));
  AACFrameInfo info;int ret=aac_config(t,&info);bool opened=false,paused=false;
  unsigned volume=atomic_load(&s->volume);
  if(!fp||!dec||!input||!pcm||!reader.cache){ret=-ENOMEM;goto done;}
  if(ret)goto done;
  if(AACSetRawBlockParams(dec,0,&info)){ret=-ENOTSUP;goto done;}
  ret=s->ops.audio_open(s->opaque,info.sampRateCore,volume);
  if(ret)goto done;opened=true;
  for(unsigned i=0;i<t->sample_count&&!atomic_load(&s->cancel);i++){
    bool want_pause=atomic_load(&s->pause);
    if(want_pause!=paused){ret=s->ops.audio_pause(s->opaque,want_pause);if(ret)break;paused=want_pause;}
    if(paused){if(paused_wait(s)){ret=-ECANCELED;break;}i--;continue;}
    unsigned want_volume=atomic_load(&s->volume);
    if(volume!=want_volume){ret=s->ops.audio_volume(s->opaque,want_volume);if(ret)break;volume=want_volume;}
    unsigned n;ret=packet(s,&reader,s->audio_track,i,input,&n);if(ret)break;
    memset(input+n,0,64);unsigned char *ptr=input;int left=n;
    if(AACDecode(dec,&ptr,&left,pcm)){ret=-EPROTO;break;}
    AACGetLastFrameInfo(dec,&info);
    if(info.nChans<1||info.nChans>2||info.outputSamps<1||info.outputSamps>4096){ret=-EPROTO;break;}
    ret=s->ops.audio_write(s->opaque,pcm,info.outputSamps/info.nChans,info.nChans);if(ret)break;
    atomic_store(&s->audio_ms,s->ops.audio_time(s->opaque));atomic_store(&s->audio_ready,true);
  }
  if(!ret&&!atomic_load(&s->cancel)){
    ret=s->ops.audio_drain(s->opaque);atomic_store(&s->audio_ms,s->ops.audio_time(s->opaque));
  }
done:
  if(opened)s->ops.audio_close(s->opaque);
  if(dec)AACFreeDecoder(dec);if(fp)fclose(fp);free(reader.cache);free(input);free(pcm);
  if(ret&&ret!=-ECANCELED)atomic_store(&s->audio_error,ret);
  atomic_store(&s->audio_done,true);return NULL;
}
struct local_video *local_video_create(const char *path,const struct local_video_ops *ops,void *opaque)
{
  if(!path||strlen(path)>=512||!ops||!ops->frame)return NULL;
  struct local_video *s=calloc(1,sizeof(*s));if(!s)return NULL;
  strcpy(s->path,path);s->ops=*ops;s->opaque=opaque;s->video_track=s->audio_track=-1;
  pthread_mutex_init(&s->lock,NULL);atomic_init(&s->volume,40);
  atomic_init(&s->cancel,false);atomic_init(&s->pause,false);atomic_init(&s->audio_ready,false);
  atomic_init(&s->audio_done,false);atomic_init(&s->audio_ms,0);atomic_init(&s->audio_error,0);
  return s;
}

#define JPEG_PACKET_LIMIT (512u*1024u)
/* Decode native MP4 JPEG frames against the shared audio clock. */
static int mp4_mjpeg_play(struct local_video *s,MP4D_track_t *v)
{
  unsigned width=v->SampleDescription.video.width,height=v->SampleDescription.video.height;
  if(width<2||height<2||width>640||height>360||!s->ops.native_buffer||!s->ops.native_frame)return -ENOTSUP;
  for(unsigned i=0;i<v->sample_count;i++){
    if(!v->entry_size[i]||v->entry_size[i]>JPEG_PACKET_LIMIT)return -EPROTO;
    if(i&&sample_ms(v,i)<sample_ms(v,i-1))return -ENOTSUP;
  }
  uint8_t *jpeg=malloc(PACKET_LIMIT+512);void *decoder=qpk_mjpeg_create();
  if(!jpeg||!decoder){free(jpeg);qpk_mjpeg_destroy(decoder);return -ENOMEM;}
  int ret=0;uint64_t decode_us=0,io_us=0;
  for(unsigned i=0;i<v->sample_count;i++){
    int64_t stamp=sample_ms(v,i);if(stamp<0)stamp=0;uint64_t pts=stamp;
    if((ret=paused_wait(s)))break;
    unsigned clock=0;
    for(;;){
      if((ret=paused_wait(s)))break;
      if((s->audio_track>=0)){
        if(atomic_load(&s->audio_error)){ret=atomic_load(&s->audio_error);break;}
        if(!atomic_load(&s->audio_ready)){if(atomic_load(&s->audio_done)){ret=-EPROTO;break;}usleep(1000);continue;}
        clock=atomic_load(&s->audio_ms);
      }else {pthread_mutex_lock(&s->lock);clock=now_ms()-s->start_ms-s->pause_total;pthread_mutex_unlock(&s->lock);}
      if(pts<=clock+2||atomic_load(&s->audio_done))break;
      usleep(1000);
    }
    if(ret)break;
    /* JPEG pictures are independent: skip overdue frames before decoding. */
    if(i+1<v->sample_count&&sample_ms(v,i+1)<=clock){
      pthread_mutex_lock(&s->lock);s->state.dropped++;pthread_mutex_unlock(&s->lock);continue;
    }
    uint16_t *dst=s->ops.native_buffer(s->opaque,width,height);if(!dst){ret=-ENOMEM;break;}
    uint64_t before=now_us();unsigned bytes=0;ret=packet(s,&s->reader,s->video_track,i,jpeg,&bytes);io_us+=now_us()-before;if(ret)break;
    before=now_us();ret=qpk_mjpeg_decode(decoder,jpeg,bytes,JPEG_PACKET_LIMIT+512,dst,width,height,width,height);
    decode_us+=now_us()-before;if(ret)break;
    before=now_us();ret=s->ops.native_frame(s->opaque,dst,width,height);s->render_us+=now_us()-before;if(ret)break;
    if((s->audio_track>=0))clock=atomic_load(&s->audio_ms);
    pthread_mutex_lock(&s->lock);s->state.frames++;s->state.decoded++;s->state.decoder_us=decode_us;s->state.render_us=s->render_us;s->state.position_ms=clock;s->state.video_ms=pts;
    s->state.late_ms=clock>pts?clock-pts:0;unsigned frames=s->state.frames,dropped=s->state.dropped;
    pthread_mutex_unlock(&s->lock);
    if(frames%120==0)printf("[mp4-mjpeg] frames=%u dropped=%u elapsed=%lu ms read=%lu JPEG=%lu ms lag=%u\n",frames,dropped,(unsigned long)(now_ms()-s->start_ms),(unsigned long)(io_us/1000),(unsigned long)(decode_us/1000),s->state.late_ms);
  }
  qpk_mjpeg_destroy(decoder);free(jpeg);return ret;
}

int local_video_run(struct local_video *s)
{
  bili_cpu_video_begin();
  struct stat st;int ret=0;uint8_t *input=NULL,*annex=NULL;pthread_t audio;bool audio_started=false;
  pthread_mutex_lock(&s->lock);s->state.running=true;pthread_mutex_unlock(&s->lock);
  if(stat(s->path,&st)||st.st_size<16){ret=-ENOENT;goto done;}
  s->size=st.st_size;s->file=fopen(s->path,"rb");if(!s->file){ret=-errno;goto done;}
  s->reader.file=s->file;s->reader.cache=malloc(READ_CACHE_SIZE);
  if(!s->reader.cache){ret=-ENOMEM;goto done;}
  uint64_t index_started=now_ms();
  if(!MP4D_open(&s->mp4,read_at,s,s->size)){ret=atomic_load(&s->cancel)?-ECANCELED:-EPROTO;goto done;}
  printf("[local-video] index ready in %lu ms, SD reads=%u tracks=%u\n",(unsigned long)(now_ms()-index_started),s->reader.fills,s->mp4.track_count);
  if(s->mp4.track_count>8){ret=-EFBIG;goto done;}
  for(unsigned i=0;i<s->mp4.track_count;i++){
    MP4D_track_t *t=&s->mp4.track[i];
    if(!t->timescale||!t->sample_count||t->sample_count>t->timestamp_count){ret=-EPROTO;goto done;}
    if(t->handler_type==MP4D_HANDLER_TYPE_VIDE&&(t->object_type_indication==MP4_OBJECT_TYPE_AVC||t->object_type_indication==0x6c)&&s->video_track<0)s->video_track=i;
    else if(t->handler_type==FOUR_CHAR_INT('s','o','u','n')&&s->audio_track<0)s->audio_track=i;
  }
  if(s->video_track<0){ret=-ENOTSUP;goto done;}
  if(s->benchmark)s->audio_track=-1;
  MP4D_track_t *v=&s->mp4.track[s->video_track];
  bool mjpeg=v->object_type_indication==0x6c;
  if(s->benchmark&&mjpeg){ret=-ENOTSUP;goto done;}
  printf("[local-video] %s %ux%u samples=%u duration=%lu ms\n",mjpeg?"MJPEG":"H264",v->SampleDescription.video.width,v->SampleDescription.video.height,v->sample_count,(unsigned long)((uint64_t)v->duration_lo*1000/v->timescale));
  if(v->SampleDescription.video.width>1280||v->SampleDescription.video.height>720){ret=-EFBIG;goto done;}
  if(s->audio_track>=0){
    AACFrameInfo info;MP4D_track_t *a=&s->mp4.track[s->audio_track];
    if(a->object_type_indication!=MP4_OBJECT_TYPE_AUDIO_ISO_IEC_14496_3||aac_config(a,&info)||
       !s->ops.audio_open||!s->ops.audio_write||!s->ops.audio_pause||!s->ops.audio_volume||
       !s->ops.audio_time||!s->ops.audio_drain||!s->ops.audio_close){ret=-ENOTSUP;goto done;}
  }
  pthread_mutex_lock(&s->lock);s->state.has_audio=s->audio_track>=0;s->state.mjpeg=mjpeg;
  s->state.duration_ms=(uint64_t)v->duration_lo*1000/v->timescale;pthread_mutex_unlock(&s->lock);
  if(mjpeg)goto start_audio;
  s->decoder=bili_decoder_create_timed(timed_frame,s);if(!s->decoder){ret=-ENOMEM;goto done;}
  input=malloc(PACKET_LIMIT+64);annex=malloc(PACKET_LIMIT+4096);if(!input||!annex){ret=-ENOMEM;goto done;}
  size_t parameter_bytes=0;
  for(int pps=0;pps<2;pps++)for(int i=0;i<256;i++){
    int n=0;const void *p=pps?MP4D_read_pps(&s->mp4,s->video_track,i,&n):MP4D_read_sps(&s->mp4,s->video_track,i,&n);
    if(!p)break;if(n<1||n>65536){ret=-EPROTO;goto done;}
    if(parameter_bytes+n+4>4096){ret=-EPROTO;goto done;}
    memcpy(annex+parameter_bytes,"\0\0\0\1",4);memcpy(annex+parameter_bytes+4,p,n);parameter_bytes+=n+4;
  }
start_audio:
  s->start_ms=now_ms();
  if(s->audio_track>=0){
    pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,65536);
    int r=pthread_create(&audio,&attr,audio_worker,s);pthread_attr_destroy(&attr);
    if(r){ret=-r;goto done;}audio_started=true;
  }
  if(mjpeg){ret=mp4_mjpeg_play(s,v);goto done;}
  for(unsigned i=0;i<v->sample_count&&!atomic_load(&s->cancel);i++){
    if((ret=paused_wait(s)))break;
    uint64_t read_started=now_us();
    unsigned n;if((ret=packet(s,&s->reader,s->video_track,i,input,&n)))break;
    s->read_us+=now_us()-read_started;
    size_t pos=0,len=i==0?parameter_bytes:0;unsigned prefix=v->nal_length;
    if(prefix<1||prefix>4){ret=-EPROTO;break;}
    while(pos<n){
      if(n-pos<prefix){ret=-EPROTO;break;}
      unsigned size=0;for(unsigned j=0;j<prefix;j++)size=(size<<8)|input[pos++];
      if(!size||size>n-pos||size+4>PACKET_LIMIT+4096-len){ret=-EPROTO;break;}
      memcpy(annex+len,"\0\0\0\1",4);memcpy(annex+len+4,input+pos,size);len+=size+4;pos+=size;
    }
    if(ret)break;
    int64_t pts=sample_ms(v,i);if(pts<0)pts=0;
    uint64_t decode_started=now_us(),callback_before=s->callback_us;
    ret=bili_decoder_feed_timestamp(s->decoder,annex,len,pts);
    s->decode_us+=now_us()-decode_started-(s->callback_us-callback_before);
    pthread_mutex_lock(&s->lock);s->state.decoder_us=s->decode_us;s->state.render_us=s->render_us;pthread_mutex_unlock(&s->lock);
    if(ret)break;
    if((i+1)%60==0)printf("[local-video] profile samples=%u read=%lu decode=%lu render=%lu wait=%lu ms frames=%u drop=%u lag=%u ms\n",i+1,(unsigned long)(s->read_us/1000),(unsigned long)(s->decode_us/1000),(unsigned long)(s->render_us/1000),(unsigned long)((s->callback_us-s->render_us)/1000),s->state.frames,s->state.dropped,s->state.late_ms);
    if(atomic_load(&s->audio_error)){ret=atomic_load(&s->audio_error);break;}
  }
  if(!ret&&!atomic_load(&s->cancel)){
    uint64_t started=now_us(),callback_before=s->callback_us;
    ret=bili_decoder_flush(s->decoder);
    s->decode_us+=now_us()-started-(s->callback_us-callback_before);
    pthread_mutex_lock(&s->lock);s->state.decoder_us=s->decode_us;s->state.render_us=s->render_us;pthread_mutex_unlock(&s->lock);
  }
done:
  if(ret)atomic_store(&s->cancel,true);
  if(audio_started)pthread_join(audio,NULL);
  if(!ret&&atomic_load(&s->audio_error))ret=atomic_load(&s->audio_error);
  if(atomic_load(&s->cancel)&&!ret)ret=-ECANCELED;
  bili_decoder_destroy(s->decoder);s->decoder=NULL;free(input);free(annex);
  MP4D_close(&s->mp4);if(s->file)fclose(s->file);s->file=NULL;free(s->reader.cache);memset(&s->reader,0,sizeof(s->reader));
  pthread_mutex_lock(&s->lock);s->state.running=false;s->state.error=ret;pthread_mutex_unlock(&s->lock);
  bili_cpu_video_end();
  printf("[local-video] finished error=%d index_remaining=%lu\n",ret,(unsigned long)atomic_load(&index_used));return ret;
}
void local_video_cancel(struct local_video *s){if(s)atomic_store(&s->cancel,true);}
bool local_video_cancelled(struct local_video *s){return !s || atomic_load(&s->cancel);}
void local_video_pause(struct local_video *s,bool pause)
{
  if(!s)return;pthread_mutex_lock(&s->lock);
  if(pause&&!atomic_load(&s->pause))s->pause_started=now_ms();
  if(!pause&&atomic_load(&s->pause))s->pause_total+=now_ms()-s->pause_started;
  atomic_store(&s->pause,pause);pthread_mutex_unlock(&s->lock);
}
void local_video_volume(struct local_video *s,unsigned v){if(s)atomic_store(&s->volume,v>100?100:v);}
void local_video_snapshot(struct local_video *s,struct local_video_snapshot *out)
{
  pthread_mutex_lock(&s->lock);*out=s->state;pthread_mutex_unlock(&s->lock);
  out->paused=atomic_load(&s->pause);out->volume=atomic_load(&s->volume);
  if(out->has_audio)out->position_ms=atomic_load(&s->audio_ms);
}
void local_video_destroy(struct local_video *s){if(s){pthread_mutex_destroy(&s->lock);free(s);}}
static int benchmark_frame(void *opaque,const uint8_t *const p[3],const int strides[2],int w,int h)
{(void)opaque;(void)p;(void)strides;(void)w;(void)h;return 0;}
struct benchmark_job {struct local_video *session;int result;};
static void *benchmark_worker(void *opaque)
{
  struct benchmark_job *job=opaque;job->result=local_video_run(job->session);return NULL;
}
int local_video_decode_benchmark(const char *path,unsigned features)
{
  static pthread_mutex_t benchmark_lock=PTHREAD_MUTEX_INITIALIZER;
  if(!path||strncmp(path,"/sdcard/",8)||strstr(path,"..")||features>3)return -EINVAL;
  struct local_video_ops ops={0};ops.frame=benchmark_frame;
  struct local_video *s=local_video_create(path,&ops,NULL);if(!s)return -ENOMEM;
  pthread_mutex_lock(&benchmark_lock);
  int previous=bili_decoder_set_mc_features(features);
  if(previous<0){pthread_mutex_unlock(&benchmark_lock);local_video_destroy(s);return previous;}
  s->benchmark=true;s->benchmark_hash=UINT32_C(2166136261);
  struct benchmark_job job={s,0};pthread_t thread;pthread_attr_t attr;
  pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,65536);
  int ret=pthread_create(&thread,&attr,benchmark_worker,&job);
  pthread_attr_destroy(&attr);
  if(ret)ret=-ret;else {pthread_join(thread,NULL);ret=job.result;}
  bili_decoder_set_mc_features((unsigned)previous);
  printf("[decode-bench] mode=%u error=%d frames=%u decode_us=%lu read_us=%lu hash=%08lx\n",
         features,ret,s->state.frames,(unsigned long)s->decode_us,
         (unsigned long)s->read_us,(unsigned long)s->benchmark_hash);
  local_video_destroy(s);pthread_mutex_unlock(&benchmark_lock);return ret;
}
void *helix_malloc(int n){return n>0&&n<512*1024?malloc(n):NULL;}
void helix_free(void *p){free(p);}
