/* SPDX-License-Identifier: Apache-2.0 */
/* One capture owner + one ASR consumer. RAM only; bounded, no dropped audio. */
#include "glass_local_capture.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __NuttX__
#include <nuttx/config.h>
#include <malloc.h>
#include <sched.h>
#endif
#define STREAM_SAMPLES (12u*GLV_CAPTURE_RATE)
#define STREAM_PAGE_SAMPLES 4096u
#define STREAM_PAGE_LIMIT ((STREAM_SAMPLES+STREAM_PAGE_SAMPLES-1u)/STREAM_PAGE_SAMPLES+2u)
struct pcm_page {struct pcm_page *next;size_t count,read;int16_t pcm[STREAM_PAGE_SAMPLES];};
#define STREAM_STACK (64u*1024u)
#define STREAM_STACK_CHARGE (128u*1024u)
struct stream_capture {
 struct glv_memory *memory;const struct glv_capture_source *source;
 const struct glv_capture_signals *signals;struct glv_capture_options options;
 glv_capture_stream_progress progress;void *opaque;
 bool (*original_cancel)(void *);void *original_opaque;
 pthread_mutex_t queue_lock,cancel_lock;pthread_cond_t ready;
 struct glv_memory queue_memory; /* accessed only under queue_lock */
 struct pcm_page *first,*last;size_t used,captured,completed;
 bool ended;int error;atomic_bool abort,finished;
};
static bool stream_cancel(void *opaque)
{
 struct stream_capture *s=opaque;pthread_mutex_lock(&s->cancel_lock);
 bool stop=atomic_load(&s->abort)||(s->original_cancel&&s->original_cancel(s->original_opaque));
 pthread_mutex_unlock(&s->cancel_lock);return stop;
}
static void progress(struct stream_capture *s,bool active)
{
 if(!s->progress)return;pthread_mutex_lock(&s->queue_lock);
 unsigned captured=(unsigned)(s->captured*1000u/GLV_CAPTURE_RATE),pending=(unsigned)((s->captured-s->completed)*1000u/GLV_CAPTURE_RATE);
 pthread_mutex_unlock(&s->queue_lock);s->progress(s->opaque,active,captured,pending);
}
static void *produce(void *opaque)
{
 struct stream_capture *s=opaque;const struct glv_capture_source *source=s->source;
 int ret=source->open(source->opaque);size_t voiced=0,quiet=0;bool finished=false;
 uint64_t began=source->milliseconds(source->opaque);
 if(!ret)progress(s,true);
 while(!ret){
  if(stream_cancel(s)){ret=-ECANCELED;break;}
  if(s->signals&&s->signals->finish&&s->signals->finish(s->signals->opaque)){finished=true;break;}
  uint64_t now=source->milliseconds(source->opaque);if(now<began){ret=-EIO;break;}
  unsigned elapsed=(unsigned)(now-began);unsigned audio_ms=(unsigned)(s->captured*1000u/GLV_CAPTURE_RATE);if(elapsed<audio_ms)elapsed=audio_ms;
  if(elapsed>=s->options.max_ms){ret=-E2BIG;break;}
  if(s->options.automatic_endpoint&&voiced<800&&elapsed>=s->options.no_speech_ms){ret=-ENODATA;break;}
  int16_t block[GLV_CAPTURE_BLOCK];int count=source->read(source->opaque,block);
  if(count<0){ret=count;break;}if(!count)continue;if((unsigned)count>GLV_CAPTURE_BLOCK){ret=-EIO;break;}
  unsigned peak=0,sum=0;for(unsigned i=0;i<(unsigned)count;i++){int v=block[i];unsigned n=v<0?(unsigned)-v:(unsigned)v;sum+=n;if(n>peak)peak=n;}
  pthread_mutex_lock(&s->queue_lock);
  if((unsigned)count>STREAM_SAMPLES-s->used)ret=-ENOBUFS;
  else {
   size_t at=0;
   while(at<(unsigned)count){
    if(!s->last||s->last->count==STREAM_PAGE_SAMPLES){
     struct pcm_page *page=glv_alloc(&s->queue_memory,sizeof(*page));
     if(!page){ret=-ENOMEM;break;}
     if(s->last)s->last->next=page;else s->first=page;s->last=page;
    }
    size_t n=(unsigned)count-at,room=STREAM_PAGE_SAMPLES-s->last->count;if(n>room)n=room;
    memcpy(s->last->pcm+s->last->count,block+at,n*sizeof(*block));
    s->last->count+=n;s->used+=n;s->captured+=n;at+=n;
   }
   pthread_cond_signal(&s->ready);
  }
  pthread_mutex_unlock(&s->queue_lock);if(ret){atomic_store(&s->abort,true);break;}
  if(peak>=640&&sum/(unsigned)count>=160){voiced+=(unsigned)count;quiet=0;}else quiet+=(unsigned)count;
  if(s->signals&&s->signals->level)s->signals->level(s->signals->opaque,peak,(unsigned)(s->captured*1000u/GLV_CAPTURE_RATE));
  progress(s,true);
  if(s->options.automatic_endpoint&&voiced>=800&&quiet>=(size_t)s->options.end_silence_ms*(GLV_CAPTURE_RATE/1000)){finished=true;break;}
 }
 /* close synchronously revokes DMA and retires callbacks before final receipt. */
 int closed=source->close(source->opaque);if(!ret)ret=closed;
 if(!ret&&(!finished||s->captured<GLV_CAPTURE_RATE/10))ret=-ENODATA;
 pthread_mutex_lock(&s->queue_lock);s->error=ret;s->ended=true;pthread_cond_broadcast(&s->ready);pthread_mutex_unlock(&s->queue_lock);
 if(ret)atomic_store(&s->abort,true);progress(s,false);
 atomic_store_explicit(&s->finished,true,memory_order_release);return NULL;
}
int glv_capture_stream(struct glv_memory *m,const struct glv_capture_source *source,const struct glv_capture_signals *signals,
                       const struct glv_capture_options *options,glv_capture_consume consume,void *opaque,glv_capture_stream_progress update)
{
 if(!m||!source||!source->open||!source->read||!source->close||!source->milliseconds||!consume)return -EINVAL;
 struct glv_capture_options defaults={60000,8000,1500,true};if(!options)options=&defaults;
 if(options->max_ms<500||options->max_ms>60000||!options->no_speech_ms||options->no_speech_ms>options->max_ms||!options->end_silence_ms||options->end_silence_ms>options->max_ms)return -EINVAL;
 int ret=glv_cancel(m);if(ret)return ret;
 size_t queue_charge=STREAM_PAGE_LIMIT*glv_alloc_charge(sizeof(struct pcm_page));
 if(m->used>m->limit||STREAM_STACK_CHARGE+queue_charge>m->limit-m->used)return -ENOMEM;
#ifdef __NuttX__
 struct mallinfo info=mallinfo();if((size_t)info.fordblks<STREAM_STACK_CHARGE+queue_charge+GLV_SYSTEM_RESERVE||(size_t)info.mxordblk<STREAM_STACK_CHARGE)return -ENOMEM;
#endif
 struct stream_capture s={.memory=m,.source=source,.signals=signals,.options=*options,.progress=update,.opaque=opaque,.original_cancel=m->cancelled,.original_opaque=m->opaque};
 /* Reserve a bounded quota, not a permanent PCM buffer. Actual pages
  * return to the heap as soon as their samples leave the queue. */
 s.queue_memory.limit=queue_charge;m->used+=queue_charge;
 if(m->used>m->peak)m->peak=m->used;
 bool queue=false,cancel=false,condition=false,attributes=false;pthread_attr_t attr;pthread_t thread;
 ret=pthread_mutex_init(&s.queue_lock,NULL);queue=!ret;
 if(!ret){ret=pthread_mutex_init(&s.cancel_lock,NULL);cancel=!ret;}
 if(!ret){ret=pthread_cond_init(&s.ready,NULL);condition=!ret;}
 if(!ret){ret=pthread_attr_init(&attr);attributes=!ret;}
 if(!ret)ret=pthread_attr_setstacksize(&attr,STREAM_STACK);
#ifdef __NuttX__
 struct sched_param priority={.sched_priority=CONFIG_SYSTEM_DESKTOP_PRIORITY};
 if(!ret)ret=pthread_attr_setinheritsched(&attr,PTHREAD_EXPLICIT_SCHED);
 if(!ret)ret=pthread_attr_setschedpolicy(&attr,SCHED_RR);
 if(!ret)ret=pthread_attr_setschedparam(&attr,&priority);
#if defined(CONFIG_SMP)
 cpu_set_t cpu;CPU_ZERO(&cpu);CPU_SET(0,&cpu);if(!ret)ret=pthread_attr_setaffinity_np(&attr,sizeof(cpu),&cpu);
#endif
#endif
 atomic_init(&s.abort,false);atomic_init(&s.finished,false);
 bool created=false;
 if(!ret){
  m->cancelled=stream_cancel;m->opaque=&s;m->used+=STREAM_STACK_CHARGE;if(m->used>m->peak)m->peak=m->used;
  ret=pthread_create(&thread,&attr,produce,&s);created=!ret;if(ret)m->used-=STREAM_STACK_CHARGE;
 }
 if(attributes)pthread_attr_destroy(&attr);
 if(created){
  while(!ret){
   if(glv_cancel(m)){ret=-ECANCELED;break;}
   int16_t block[1600];size_t count=0;pthread_mutex_lock(&s.queue_lock);
   while(!s.used&&!s.ended&&!atomic_load(&s.abort)){
    struct timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_nsec+=100000000;if(until.tv_nsec>=1000000000){until.tv_sec++;until.tv_nsec-=1000000000;}
    int wait=pthread_cond_timedwait(&s.ready,&s.queue_lock,&until);if(wait&&wait!=ETIMEDOUT){ret=-wait;break;}if(wait==ETIMEDOUT)break;
   }
   if(s.error)ret=s.error;
   if(!ret&&s.used){
    size_t wanted=s.used;if(wanted>1600)wanted=1600;
    while(count<wanted){
     struct pcm_page *page=s.first;size_t n=page->count-page->read;if(n>wanted-count)n=wanted-count;
     memcpy(block+count,page->pcm+page->read,n*sizeof(*block));page->read+=n;count+=n;s.used-=n;
     if(page->read==page->count){s.first=page->next;if(!s.first)s.last=NULL;glv_free(page);}
    }
   }
   bool ended=s.ended;pthread_mutex_unlock(&s.queue_lock);if(ret)break;
   if(!count){if(ended)break;continue;}
   ret=consume(opaque,block,count);memset(block,0,sizeof(block));if(ret)break;
   pthread_mutex_lock(&s.queue_lock);s.completed+=count;pthread_mutex_unlock(&s.queue_lock);
  }
  if(ret)atomic_store(&s.abort,true);
  int joined;do{joined=pthread_join(thread,NULL);}while(joined==EINTR);
  if(joined){while(!atomic_load_explicit(&s.finished,memory_order_acquire))usleep(1000);pthread_detach(thread);if(!ret)ret=-joined;}
  if(s.error&&(!ret||ret==-ECANCELED))ret=s.error;
  m->used-=STREAM_STACK_CHARGE;
 }
 m->cancelled=s.original_cancel;m->opaque=s.original_opaque;
 /* Producer/DMA have retired; pending pages on error are now safe to free. */
 while(s.first){struct pcm_page *page=s.first;s.first=page->next;glv_free(page);}
 m->used-=queue_charge;
 if(condition)pthread_cond_destroy(&s.ready);if(cancel)pthread_mutex_destroy(&s.cancel_lock);if(queue)pthread_mutex_destroy(&s.queue_lock);
 return ret>0?-ret:ret;
}
