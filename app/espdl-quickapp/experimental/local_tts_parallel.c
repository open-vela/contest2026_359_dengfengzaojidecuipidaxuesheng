/* SPDX-License-Identifier: Apache-2.0 */
#include "local_tts_parallel.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#ifdef __NuttX__
#include <nuttx/config.h>
#include <malloc.h>
#include <sched.h>
#endif
#if (defined(__NuttX__) && defined(CONFIG_SMP)) || defined(GLV_TTS_TEST_AFFINITY)
#define GLV_TTS_CPU_AFFINITY
#include <sched.h>
#endif
#define STACK_BYTES (64u*1024u)
/* Conservative charge includes stack, TLS and helper overhead. Also check
 * actual user-heap reserve/contiguous space before pthread allocates. */
#define STACK_CHARGE (128u*1024u)
_Static_assert(GLV_PAR_REASON_COUNT==7,"profile fallback reason count");
void glv_tts_parallel_fallback(struct glv_memory *m,enum glv_tts_parallel_reason reason,int error)
{
  m->profile.tts_parallel_fallbacks++;
  m->profile.tts_parallel_reasons[reason]++;
  m->profile.tts_parallel_last_reason=(unsigned)reason+1;
  m->profile.tts_parallel_last_error=(unsigned)error;
}
struct pair {
  pthread_mutex_t cancel_lock;
  atomic_bool stop,finished;
  glv_tts_parallel_body body;void *argument;
  struct glv_tts_lane lane;
  unsigned begin,end;int result,cpu,target_cpu;
  uint64_t began,ended;
};
int glv_tts_parallel_cancel(struct glv_tts_lane *lane)
{
  struct pair *p=lane->sync;
  if(!p)return glv_cancel(lane->memory);
  int ret=pthread_mutex_lock(&p->cancel_lock);if(ret)return -ret;
  ret=atomic_load(&p->stop)?-ECANCELED:glv_cancel(lane->memory);
  if(ret)atomic_store(&p->stop,true);
  int unlocked=pthread_mutex_unlock(&p->cancel_lock);return ret?ret:unlocked?-unlocked:0;
}
static void *run_worker(void *argument)
{
  struct pair *p=argument;
  p->began=glv_now_us();
#ifdef GLV_TTS_CPU_AFFINITY
  p->cpu=sched_getcpu();
  p->result=p->cpu==p->target_cpu?p->body(p->argument,p->begin,p->end,&p->lane):-EXDEV;
#else
  p->cpu=-1;p->result=p->body(p->argument,p->begin,p->end,&p->lane);
#endif
  p->ended=glv_now_us();
  if(p->result)atomic_store(&p->stop,true);
  /* Last access to p: the error path can safely retire caller buffers once
   * this release is observed, even if a POSIX join unexpectedly fails. */
  atomic_store_explicit(&p->finished,true,memory_order_release);return NULL;
}
int glv_tts_parallel_run(struct glv_memory *memory,glv_tts_parallel_body body,void *argument,unsigned count,uint64_t work)
{
  struct glv_tts_lane serial={.memory=memory};
  if(memory->tts_serial_fp||count<2||work<262144)return body(argument,0,count,&serial);
  uint64_t setup_started=glv_now_us();
  enum glv_tts_parallel_reason reason=GLV_PAR_BUDGET;int error=ENOMEM;
  if(memory->used>memory->limit||STACK_CHARGE>memory->limit-memory->used)goto fallback;
#ifdef __NuttX__
  struct mallinfo mi=mallinfo();
  if((size_t)mi.fordblks<4u*1024u*1024u+2u*STACK_CHARGE){reason=GLV_PAR_RESERVE;goto fallback;}
  if((size_t)mi.mxordblk<STACK_CHARGE){reason=GLV_PAR_FRAGMENT;goto fallback;}
#endif
  struct pair p={.body=body,.argument=argument,.begin=count/2,.end=count,.cpu=-1};
  atomic_init(&p.stop,false);atomic_init(&p.finished,false);
  int ret=pthread_mutex_init(&p.cancel_lock,NULL);if(ret){reason=GLV_PAR_MUTEX;error=ret;goto fallback;}
  p.lane=(struct glv_tts_lane){.memory=memory,.sync=&p};
  struct glv_tts_lane left=p.lane;
  pthread_attr_t attr;pthread_t worker;bool attributes=false;
#ifdef GLV_TTS_CPU_AFFINITY
  cpu_set_t saved,main_set,worker_set;bool pinned=false;
  int owner_cpu=-1;
  ret=pthread_getaffinity_np(pthread_self(),sizeof(saved),&saved);
  /* Pin where the caller already runs. A helper on the other core needs no
   * forced caller migration. Bound retries for a migration between sampling
   * the current CPU and setting its mask; never weaken the actual CPU check. */
  for(unsigned attempt=0;!ret&&attempt<3;attempt++){
    owner_cpu=sched_getcpu();if(owner_cpu<0||owner_cpu>1){ret=EXDEV;break;}
    CPU_ZERO(&main_set);CPU_SET(owner_cpu,&main_set);
    ret=pthread_setaffinity_np(pthread_self(),sizeof(main_set),&main_set);
    if(ret)break;pinned=true;
    if(sched_getcpu()==owner_cpu)break;
    memory->profile.tts_parallel_pin_retries++;
    if(attempt==2)ret=EXDEV;
  }
  p.target_cpu=1-owner_cpu;
#endif
  if(!ret){ret=pthread_attr_init(&attr);attributes=!ret;}
  if(!ret)ret=pthread_attr_setstacksize(&attr,STACK_BYTES);
#ifdef GLV_TTS_CPU_AFFINITY
  if(!ret){CPU_ZERO(&worker_set);CPU_SET(p.target_cpu,&worker_set);ret=pthread_attr_setaffinity_np(&attr,sizeof(worker_set),&worker_set);}
  /* Desktop is priority 100 on this baseline. Keep its input/paint thread
   * preemptible while the second core performs sustained FP32 work. */
  struct sched_param priority={.sched_priority=CONFIG_SYSTEM_DESKTOP_PRIORITY-1};
  if(!ret)ret=pthread_attr_setinheritsched(&attr,PTHREAD_EXPLICIT_SCHED);
  if(!ret)ret=pthread_attr_setschedpolicy(&attr,SCHED_RR);
  if(!ret)ret=pthread_attr_setschedparam(&attr,&priority);
#endif
  uint64_t began=glv_now_us();
  reason=GLV_PAR_SETUP;
  if(!ret){
    memory->used+=STACK_CHARGE;if(memory->used>memory->peak)memory->peak=memory->used;
    if(memory->profile.tts_parallel_stack_peak<STACK_CHARGE)memory->profile.tts_parallel_stack_peak=STACK_CHARGE;
    reason=GLV_PAR_CREATE;ret=pthread_create(&worker,&attr,run_worker,&p);
    if(ret)memory->used-=STACK_CHARGE;
  }
  if(attributes)pthread_attr_destroy(&attr);
  if(ret){
    error=ret;
#ifdef GLV_TTS_CPU_AFFINITY
    if(pinned){int restored=pthread_setaffinity_np(pthread_self(),sizeof(saved),&saved);if(restored){pthread_mutex_destroy(&p.cancel_lock);return -restored;}}
#endif
    pthread_mutex_destroy(&p.cancel_lock);goto fallback;
  }
  memory->profile.tts_parallel_setup_us+=glv_now_us()-setup_started;
  memory->profile.tts_parallel_jobs++;
#ifdef GLV_TTS_CPU_AFFINITY
  int actual_owner=sched_getcpu();
  if(actual_owner==0){memory->profile.tts_parallel_cpu0++;memory->profile.tts_parallel_owner0++;}
  if(actual_owner==1){memory->profile.tts_parallel_cpu1++;memory->profile.tts_parallel_owner1++;}
#endif
  uint64_t left_began=glv_now_us();
#ifdef GLV_TTS_CPU_AFFINITY
  int main_result=actual_owner==owner_cpu?body(argument,0,count/2,&left):-EXDEV;
#else
  int main_result=body(argument,0,count/2,&left);
#endif
  uint64_t left_ended=glv_now_us();
  if(main_result)atomic_store(&p.stop,true);
  uint64_t join_started=glv_now_us();
  int joined;do{joined=pthread_join(worker,NULL);}while(joined==EINTR);
  if(joined){
    /* No freeing caller data while a helper might still reference it. */
    while(!atomic_load_explicit(&p.finished,memory_order_acquire))usleep(1000);
    pthread_detach(worker);
  }
  memory->profile.tts_parallel_join_us+=glv_now_us()-join_started;
  memory->profile.tts_parallel_us+=glv_now_us()-began;
  uint64_t overlap_start=left_began>p.began?left_began:p.began,overlap_end=left_ended<p.ended?left_ended:p.ended;
  if(overlap_end>overlap_start)memory->profile.tts_parallel_overlap_us+=overlap_end-overlap_start;
#ifdef GLV_TTS_CPU_AFFINITY
  if(p.cpu==0)memory->profile.tts_parallel_cpu0++;
  if(p.cpu==1)memory->profile.tts_parallel_cpu1++;
  int restored=pthread_setaffinity_np(pthread_self(),sizeof(saved),&saved);
  if(restored&&!joined)joined=restored;
#endif
  memory->used-=STACK_CHARGE;pthread_mutex_destroy(&p.cancel_lock);
  if(joined)return -joined;
  if(main_result&&main_result!=-ECANCELED)return main_result;
  if(p.result)return p.result;
  return main_result;
fallback:
  memory->profile.tts_parallel_setup_us+=glv_now_us()-setup_started;
  glv_tts_parallel_fallback(memory,reason,error);return body(argument,0,count,&serial);
}
