/* SPDX-License-Identifier: Apache-2.0 */
#include "local_tts_parallel.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
static int create_fault,join_fault;
static unsigned created;
int __real_pthread_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
int __wrap_pthread_create(pthread_t *t,const pthread_attr_t *a,void *(*f)(void *),void *p)
{created++;return create_fault?EAGAIN:__real_pthread_create(t,a,f,p);}
int __real_pthread_join(pthread_t,void **);
int __wrap_pthread_join(pthread_t t,void **p)
{if(join_fault){int ret=join_fault;join_fault=0;return ret;}return __real_pthread_join(t,p);}
struct task {unsigned values[64];atomic_int active;atomic_uint checks;bool cancel,fail;};
static bool cancelled(void *argument)
{struct task *t=argument;return t->cancel&&atomic_fetch_add(&t->checks,1)>=3;}
static int body(void *argument,unsigned first,unsigned last,struct glv_tts_lane *lane)
{
  struct task *t=argument;atomic_fetch_add(&t->active,1);int ret=0;
  for(unsigned i=first;i<last;i++){
    if((ret=glv_tts_parallel_cancel(lane)))break;
    if(t->fail&&first){ret=-ERANGE;break;}
    t->values[i]=i*17+3;usleep(100);
  }
  atomic_fetch_sub(&t->active,1);return ret;
}
static void one(unsigned mode)
{
  struct task t={.cancel=mode==2,.fail=mode==5};atomic_init(&t.active,0);atomic_init(&t.checks,0);
  struct glv_memory m={.limit=GLV_MEMORY_LIMIT,.cancelled=cancelled,.opaque=&t};
  m.tts_serial_fp=mode==6;if(mode==7)m.limit=32768;
  create_fault=mode==1;join_fault=mode==3?EINVAL:mode==4?EINTR:0;created=0;
  int ret=glv_tts_parallel_run(&m,body,&t,64,1000000);
  assert(ret==(mode==2?-ECANCELED:mode==3?-EINVAL:mode==5?-ERANGE:0));
  assert(!m.used&&m.peak<=m.limit&&!atomic_load(&t.active));
  unsigned before[64];memcpy(before,t.values,sizeof(before));usleep(10000);assert(!memcmp(before,t.values,sizeof(before)));
  if(!ret)for(unsigned i=0;i<64;i++)assert(t.values[i]==i*17+3);
  if(mode==1||mode==7)assert(m.profile.tts_parallel_fallbacks==1&&!m.profile.tts_parallel_jobs);
  else if(mode==6)assert(!created&&!m.profile.tts_parallel_jobs&&!m.profile.tts_parallel_fallbacks);
  else assert(m.profile.tts_parallel_jobs==1&&created==1);
  unsigned classified=0;for(unsigned i=0;i<GLV_PAR_REASON_COUNT;i++)classified+=m.profile.tts_parallel_reasons[i];
  assert(classified==m.profile.tts_parallel_fallbacks);
  if(mode==1)assert(m.profile.tts_parallel_reasons[GLV_PAR_CREATE]==1&&m.profile.tts_parallel_last_error==EAGAIN);
  if(mode==7)assert(m.profile.tts_parallel_reasons[GLV_PAR_BUDGET]==1&&m.profile.tts_parallel_last_error==ENOMEM);
  create_fault=join_fault=0;
}
int main(void)
{
  for(unsigned mode=0;mode<8;mode++)one(mode);
  for(unsigned i=0;i<20;i++)one(0);
  puts("TTS_PARALLEL_JOIN_RETIRE_CANCEL_ERROR_BUDGET_FALLBACK_OK");return 0;
}
