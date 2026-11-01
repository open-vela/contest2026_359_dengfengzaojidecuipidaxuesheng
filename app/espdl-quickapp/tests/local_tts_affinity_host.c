/* SPDX-License-Identifier: Apache-2.0 */
/* Simulate both P4 CPU identities and affinity races, not Linux CPU IDs. */
#include "local_tts_parallel.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
static _Thread_local int cpu_id;
static unsigned mask,helper_cpu,pins,creates,restores;
static int mode,join_error;
struct start {void *(*entry)(void *);void *argument;unsigned cpu;};
static struct start pending;
int __wrap_sched_getcpu(void){return cpu_id;}
int __wrap_pthread_getaffinity_np(pthread_t t,size_t n,cpu_set_t *set)
{
  (void)t;assert(n==sizeof(*set));if(mode==4)return ESRCH;
  CPU_ZERO(set);for(unsigned i=0;i<2;i++)if(mask&(1u<<i))CPU_SET(i,set);
  return 0;
}
int __wrap_pthread_setaffinity_np(pthread_t t,size_t n,const cpu_set_t *set)
{
  (void)t;assert(n==sizeof(*set));unsigned next=CPU_ISSET(0,set)|(CPU_ISSET(1,set)<<1);
  assert(next==1||next==2||next==3);
  if(next==3){mask=next;restores++;return 0;}
  pins++;if(mode==5)return EACCES;
  mask=next;
  if(mode==3||(mode==2&&pins==1))cpu_id=1-cpu_id;
  return 0;
}
int __wrap_pthread_attr_setaffinity_np(pthread_attr_t *a,size_t n,const cpu_set_t *set)
{
  (void)a;assert(n==sizeof(*set));assert(CPU_COUNT(set)==1);
  helper_cpu=CPU_ISSET(1,set)?1:0;return mode==6?EINVAL:0;
}
static void *trampoline(void *arg)
{
  struct start *s=arg;cpu_id=(int)s->cpu;return s->entry(s->argument);
}
int __real_pthread_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
int __wrap_pthread_create(pthread_t *t,const pthread_attr_t *a,void *(*entry)(void *),void *arg)
{
  (void)a;creates++;if(mode==11)return EAGAIN;
  pending=(struct start){entry,arg,mode==7?1-helper_cpu:helper_cpu};
  /* OS scheduling is mocked; use ordinary Linux attributes, no RT privilege. */
  return __real_pthread_create(t,NULL,trampoline,&pending);
}
int __real_pthread_join(pthread_t,void **);
int __wrap_pthread_join(pthread_t t,void **value)
{if(join_error){int e=join_error;join_error=0;return e;}return __real_pthread_join(t,value);}
struct task {unsigned values[64];atomic_uint checks;atomic_int active;bool cancel;};
static bool cancelled(void *arg)
{struct task *t=arg;return t->cancel&&atomic_fetch_add(&t->checks,1)>=3;}
static int body(void *arg,unsigned first,unsigned last,struct glv_tts_lane *lane)
{
  struct task *t=arg;atomic_fetch_add(&t->active,1);int ret=0;
  for(unsigned i=first;i<last;i++){
    if((ret=glv_tts_parallel_cancel(lane)))break;
    t->values[i]=17*i+3;usleep(100);
  }
  atomic_fetch_sub(&t->active,1);return ret;
}
static void one(unsigned test)
{
  mode=(int)test;mask=3;pins=creates=restores=0;cpu_id=test==1||test==9?1:0;
  join_error=test==10?EINVAL:0;
  struct task t={.cancel=test==8||test==9};atomic_init(&t.active,0);atomic_init(&t.checks,0);
  struct glv_memory m={.limit=GLV_MEMORY_LIMIT,.cancelled=cancelled,.opaque=&t};
  int ret=glv_tts_parallel_run(&m,body,&t,64,1000000);
  int expected=test==7?-EXDEV:test==8||test==9?-ECANCELED:test==10?-EINVAL:0;
  assert(ret==expected&&mask==3&&!m.used&&m.peak<=m.limit&&!atomic_load(&t.active));
  unsigned before[64];memcpy(before,t.values,sizeof(before));usleep(10000);
  assert(!memcmp(before,t.values,sizeof(before)));
  if(!ret)for(unsigned i=0;i<64;i++)assert(t.values[i]==17*i+3);
  if(test>=3&&test<=6||test==11){
    assert(m.profile.tts_parallel_fallbacks==1&&!m.profile.tts_parallel_jobs);
    if(test!=4&&test!=5)assert(restores==1);
    if(test==3)assert(pins==3&&m.profile.tts_parallel_pin_retries==3&&m.profile.tts_parallel_last_error==EXDEV);
    if(test==11)assert(m.profile.tts_parallel_reasons[GLV_PAR_CREATE]==1&&m.profile.tts_parallel_last_error==EAGAIN);
    else assert(!creates&&m.profile.tts_parallel_reasons[GLV_PAR_SETUP]==1);
  }else{
    assert(creates==1&&restores==1&&m.profile.tts_parallel_jobs==1);
    assert(m.profile.tts_parallel_owner0+m.profile.tts_parallel_owner1==1);
    if(test!=7)assert(m.profile.tts_parallel_cpu0==1&&m.profile.tts_parallel_cpu1==1);
    if(test==1||test==2||test==9)assert(m.profile.tts_parallel_owner1==1&&helper_cpu==0);
    if(test==2)assert(pins==2&&m.profile.tts_parallel_pin_retries==1);
  }
}
int main(void)
{
  for(unsigned i=0;i<12;i++)one(i);
  for(unsigned i=0;i<20;i++){one(0);one(1);}
  puts("TTS_AFFINITY_BOTH_OWNERS_RACE_BOUNDED_FAILURE_CANCEL_RETIRE_RESTORE_OK");return 0;
}
