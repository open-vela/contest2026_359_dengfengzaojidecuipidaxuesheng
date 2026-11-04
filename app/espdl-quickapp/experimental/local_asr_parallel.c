/* SPDX-License-Identifier: Apache-2.0 */
/* One optional helper per ASR session; no allocation or I/O in compute lanes. */
#include "local_asr_parallel.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#ifdef __NuttX__
#include <nuttx/config.h>
#include <malloc.h>
#include <sched.h>
#if defined(CONFIG_SMP)
#define ASR_AFFINITY
#endif
#endif
#define STACK_BYTES (64u*1024u)
#define STACK_CHARGE (128u*1024u)
struct pool {
 pthread_mutex_t job_lock,cancel_lock;
 pthread_cond_t ready,done;
 pthread_t worker;
 struct glv_memory *memory;
 glv_asr_parallel_body body;void *argument;
 unsigned first,last;int result,owner_cpu,worker_cpu,pie_state[2];
 bool pending,complete,quit,busy;
 atomic_bool stop,finished;
 uint64_t began,ended;
};
int glv_asr_lane_cancel(struct glv_asr_lane *lane)
{
 struct pool *p=lane->pool;if(!p)return glv_cancel(lane->memory);
 int ret=pthread_mutex_lock(&p->cancel_lock);if(ret)return -ret;
 ret=atomic_load(&p->stop)?-ECANCELED:glv_cancel(lane->memory);
 if(ret)atomic_store(&p->stop,true);
 int end=pthread_mutex_unlock(&p->cancel_lock);return ret?ret:end?-end:0;
}
static void *worker(void *opaque)
{
 struct pool *p=opaque;struct glv_asr_lane lane={p->memory,p,1,&p->pie_state[1]};
 pthread_mutex_lock(&p->job_lock);
 for(;;){
  while(!p->pending&&!p->quit)pthread_cond_wait(&p->ready,&p->job_lock);
  if(p->quit)break;
  glv_asr_parallel_body body=p->body;void *arg=p->argument;unsigned first=p->first,last=p->last;p->pending=false;
  pthread_mutex_unlock(&p->job_lock);p->began=glv_now_us();
#ifdef ASR_AFFINITY
  int ret=sched_getcpu()==p->worker_cpu?body(arg,first,last,&lane):-EXDEV;
#else
  int ret=body(arg,first,last,&lane);
#endif
  p->ended=glv_now_us();if(ret)atomic_store(&p->stop,true);
  pthread_mutex_lock(&p->job_lock);p->result=ret;p->complete=true;pthread_cond_signal(&p->done);
 }
 pthread_mutex_unlock(&p->job_lock);
 /* Last access: owner can retire buffers after this even on join failure. */
 atomic_store_explicit(&p->finished,true,memory_order_release);return NULL;
}
static struct pool *open_pool(struct glv_memory *m)
{
 if(m->asr_pool)return m->asr_pool;
 if(m->asr_pool_unavailable)return NULL;
 m->asr_pool_unavailable=true;
 if(m->used>m->limit||STACK_CHARGE+sizeof(struct pool)+256>m->limit-m->used)goto fail;
#ifdef __NuttX__
 struct mallinfo info=mallinfo();
 if((size_t)info.fordblks<4u*1024u*1024u+2u*STACK_CHARGE||(size_t)info.mxordblk<STACK_CHARGE)goto fail;
#endif
 struct pool *p=glv_alloc(m,sizeof(*p));if(!p)goto fail;p->memory=m;
 bool job=false,cancel=false,ready=false,done=false,attr_ok=false;
 pthread_attr_t attr;int ret=pthread_mutex_init(&p->job_lock,NULL);job=!ret;
 if(!ret){ret=pthread_mutex_init(&p->cancel_lock,NULL);cancel=!ret;}
 if(!ret){ret=pthread_cond_init(&p->ready,NULL);ready=!ret;}
 if(!ret){ret=pthread_cond_init(&p->done,NULL);done=!ret;}
 if(!ret){ret=pthread_attr_init(&attr);attr_ok=!ret;}
 if(!ret)ret=pthread_attr_setstacksize(&attr,STACK_BYTES);
#ifdef ASR_AFFINITY
 p->owner_cpu=sched_getcpu();p->worker_cpu=1-p->owner_cpu;
 if(!ret&&(p->owner_cpu<0||p->owner_cpu>1))ret=EXDEV;
 cpu_set_t cpu;CPU_ZERO(&cpu);if(!ret)CPU_SET(p->worker_cpu,&cpu);
 if(!ret)ret=pthread_attr_setaffinity_np(&attr,sizeof(cpu),&cpu);
 struct sched_param priority={.sched_priority=CONFIG_SYSTEM_DESKTOP_PRIORITY-1};
 if(!ret)ret=pthread_attr_setinheritsched(&attr,PTHREAD_EXPLICIT_SCHED);
 if(!ret)ret=pthread_attr_setschedpolicy(&attr,SCHED_RR);
 if(!ret)ret=pthread_attr_setschedparam(&attr,&priority);
#endif
 atomic_init(&p->stop,false);atomic_init(&p->finished,false);
 if(!ret){m->used+=STACK_CHARGE;if(m->used>m->peak)m->peak=m->used;ret=pthread_create(&p->worker,&attr,worker,p);if(ret)m->used-=STACK_CHARGE;}
 if(attr_ok)pthread_attr_destroy(&attr);
 if(!ret){m->asr_pool=p;m->asr_pool_unavailable=false;m->profile.asr_parallel_workers++;return p;}
 if(done)pthread_cond_destroy(&p->done);if(ready)pthread_cond_destroy(&p->ready);
 if(cancel)pthread_mutex_destroy(&p->cancel_lock);if(job)pthread_mutex_destroy(&p->job_lock);glv_free(p);
fail:m->profile.asr_parallel_fallbacks++;return NULL;
}
int glv_asr_parallel_run(struct glv_memory *m,glv_asr_parallel_body body,void *arg,unsigned count,uint64_t work)
{
 struct glv_asr_lane serial={m,NULL,0,&m->asr_pie_state};
 if(m->asr_reference||m->asr_serial||count<2||work<262144)return body(arg,0,count,&serial);
 struct pool *p=open_pool(m);if(!p)return body(arg,0,count,&serial);
#ifdef ASR_AFFINITY
 cpu_set_t saved,target;int ret=pthread_getaffinity_np(pthread_self(),sizeof(saved),&saved);if(ret)return -ret;
 CPU_ZERO(&target);CPU_SET(p->owner_cpu,&target);
 ret=pthread_setaffinity_np(pthread_self(),sizeof(target),&target);if(ret){m->profile.asr_parallel_fallbacks++;return body(arg,0,count,&serial);}
 if(sched_getcpu()!=p->owner_cpu){pthread_setaffinity_np(pthread_self(),sizeof(saved),&saved);return -EXDEV;}
#endif
 pthread_mutex_lock(&p->job_lock);
 if(p->busy){pthread_mutex_unlock(&p->job_lock);
#ifdef ASR_AFFINITY
  pthread_setaffinity_np(pthread_self(),sizeof(saved),&saved);
#endif
  return -EBUSY;}
 p->busy=true;p->body=body;p->argument=arg;p->first=count/2;p->last=count;p->complete=false;p->pending=true;atomic_store(&p->stop,false);
 pthread_cond_signal(&p->ready);pthread_mutex_unlock(&p->job_lock);
 struct glv_asr_lane left={m,p,0,&p->pie_state[0]};uint64_t began=glv_now_us();
 int own=body(arg,0,count/2,&left);uint64_t ended=glv_now_us();if(own)atomic_store(&p->stop,true);
 pthread_mutex_lock(&p->job_lock);while(!p->complete)pthread_cond_wait(&p->done,&p->job_lock);
 int other=p->result;p->busy=false;pthread_mutex_unlock(&p->job_lock);
 m->profile.asr_wait_us+=glv_now_us()-ended;
 m->profile.asr_parallel_jobs++;
 uint64_t first=began>p->began?began:p->began,last=ended<p->ended?ended:p->ended;
 if(last>first)m->profile.asr_parallel_overlap_us+=last-first;
#ifdef ASR_AFFINITY
 m->profile.asr_parallel_cpu0++;m->profile.asr_parallel_cpu1++;
 int restored=pthread_setaffinity_np(pthread_self(),sizeof(saved),&saved);if(restored)return -restored;
#endif
 if(own&&own!=-ECANCELED)return own;if(other)return other;return own;
}
void glv_asr_parallel_close(struct glv_memory *m)
{
 if(!m)return;struct pool *p=m->asr_pool;if(!p){m->asr_pool_unavailable=false;return;}
 pthread_mutex_lock(&p->job_lock);p->quit=true;pthread_cond_signal(&p->ready);pthread_mutex_unlock(&p->job_lock);
 int ret;do{ret=pthread_join(p->worker,NULL);}while(ret==EINTR);
 if(ret){while(!atomic_load_explicit(&p->finished,memory_order_acquire))usleep(1000);pthread_detach(p->worker);}
 m->used-=STACK_CHARGE;
 pthread_cond_destroy(&p->done);pthread_cond_destroy(&p->ready);pthread_mutex_destroy(&p->cancel_lock);pthread_mutex_destroy(&p->job_lock);
 m->asr_pool=NULL;m->asr_pool_unavailable=false;m->asr_pie_state=0;glv_free(p);
}
