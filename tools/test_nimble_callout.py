#!/usr/bin/env python3
"""Exercise the full production NuttX callout dispatcher under coincident expiry."""
import argparse,os,subprocess
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--source',type=Path,help='Optional prior production os_callout.c for a negative control')
a=p.parse_args();repo=Path(__file__).resolve().parents[1];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
port=repo/'app/espdl-quickapp/overlay/apps/wireless/bluetooth/nimble/mynewt-nimble/porting/npl/nuttx'
source=(a.source or port/'src/os_callout.c').resolve()
headers=out/'include';(headers/'nuttx').mkdir(parents=True,exist_ok=True);(headers/'nimble').mkdir(exist_ok=True)
(headers/'nuttx/config.h').write_text('#define CONFIG_NIMBLE_CALLOUT_THREAD_STACKSIZE 8192\n')
(headers/'nimble/nimble_npl.h').write_text('''#pragma once
#include <stdint.h>
#include <stdbool.h>
struct ble_npl_event;
typedef void ble_npl_event_fn(struct ble_npl_event *);
typedef void *pthread_addr_t;
#include "nimble/os_types.h"
typedef int ble_npl_error_t;
#define BLE_NPL_OK 0
#define BLE_NPL_EINVAL 1
void ble_npl_eventq_put(struct ble_npl_eventq *,struct ble_npl_event *);
ble_npl_time_t ble_npl_time_get(void);
''')
fixture=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "nimble/nimble_npl.h"
#include "nimble/nuttx_callout.h"
enum {COUNT=32, ROUNDS=100};
static void *(*worker_fn)(void *);
static void *worker_arg;
static pthread_t worker_id;
static unsigned created,delivered,expected;
static unsigned seen[COUNT];
static pthread_mutex_t observed=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t signal_done=PTHREAD_COND_INITIALIZER;
static struct itimerspec timers[COUNT+1];
static unsigned timer_count;
static struct ble_npl_callout callouts[COUNT];
static struct ble_npl_eventq queues[2];
static int fake_create(pthread_t *id,const pthread_attr_t *attr,void *(*fn)(void *),void *arg)
{(void)id;(void)attr;created++;worker_fn=fn;worker_arg=arg;return 0;}
static int fake_name(pthread_t id,const char *name){(void)id;(void)name;return 0;}
static int fake_timer_create(clockid_t clock,struct sigevent *sev,timer_t *out)
{assert(clock==CLOCK_REALTIME&&sev->sigev_notify==SIGEV_THREAD);*out=(timer_t)(uintptr_t)++timer_count;return 0;}
static int fake_timer_settime(timer_t id,int flags,const struct itimerspec *value,struct itimerspec *old)
{(void)old;assert(!flags);timers[(uintptr_t)id]=*value;return 0;}
static int fake_timer_gettime(timer_t id,struct itimerspec *value)
{*value=timers[(uintptr_t)id];return 0;}
ble_npl_time_t ble_npl_time_get(void){return 1000;}
#define pthread_create fake_create
#define pthread_setname_np fake_name
#define timer_create fake_timer_create
#define timer_settime fake_timer_settime
#define timer_gettime fake_timer_gettime
@SOURCE@
#undef pthread_create
#undef pthread_setname_np
#undef timer_create
#undef timer_settime
#undef timer_gettime
static void fired(struct ble_npl_event *event)
{
 unsigned index=(uintptr_t)event->ev_arg;
 assert(index<COUNT&&pthread_equal(pthread_self(),worker_id));
 pthread_mutex_lock(&observed);seen[index]++;delivered++;
 bool last=delivered==expected;
 pthread_cond_signal(&signal_done);pthread_mutex_unlock(&observed);
 if(last)pthread_exit(NULL);
}
void ble_npl_eventq_put(struct ble_npl_eventq *queue,struct ble_npl_event *event)
{
 unsigned index=(uintptr_t)event->ev_arg;
 assert(queue==&queues[index%2]);
 event->ev_cb(event);
}
static void *launch(void *arg)
{(void)arg;worker_id=pthread_self();return worker_fn(worker_arg);}
static void expire(unsigned index)
{union sigval value={.sival_ptr=&callouts[index]};ble_npl_callout_timer_cb(value);}
int main(void)
{
 /* Timer callbacks run while the persistent dispatcher cannot yet run,
  * matching simultaneous SIGEV_THREAD callbacks or a full NPL event queue.
  * Every distinct timer must survive. No real radio is substituted here. */
 for(unsigned i=0;i<COUNT;i++)ble_npl_callout_init(&callouts[i],i%3?&queues[i%2]:NULL,fired,(void *)(uintptr_t)i);
 assert(created==1&&worker_fn);
 for(unsigned round=0;round<ROUNDS;round++){
  memset(seen,0,sizeof(seen));delivered=0;expected=COUNT;
  for(unsigned i=0;i<COUNT;i++){
   assert(!ble_npl_callout_reset(&callouts[i],10));
   assert(ble_npl_callout_is_active(&callouts[i]));
   timers[(uintptr_t)callouts[i].c_timer]=(struct itimerspec){0};
   expire(i);expire(i);expire(i);
  }
  pthread_t worker;assert(!pthread_create(&worker,NULL,launch,NULL));
  struct timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_sec++;
  pthread_mutex_lock(&observed);
  while(delivered<expected){
   int rc=pthread_cond_timedwait(&signal_done,&observed,&until);
   if(rc==ETIMEDOUT){printf("NPL_TIMER_LOSS round=%u distinct=%u delivered=%u hci_poll_delivered=%u\n",round,COUNT,delivered,seen[0]);fflush(stdout);return 42;}
   assert(!rc);
  }
  pthread_mutex_unlock(&observed);assert(!pthread_join(worker,NULL));
  for(unsigned i=0;i<COUNT;i++)assert(seen[i]==1&&!ble_npl_callout_is_active(&callouts[i]));
 }
#ifdef PORT_STATS
 struct ble_npl_nuttx_callout_stats stats;ble_npl_nuttx_callout_read(&stats);
 assert(stats.expirations==COUNT*ROUNDS*3&&stats.dispatched==COUNT*ROUNDS&&stats.coalesced==COUNT*ROUNDS*2);
 assert(!stats.pending&&stats.highwater==COUNT);
#endif
 puts("NPL_CALLOUT_OK: 100 coincident-expiry batches, 32 distinct timers/two queues/direct callbacks, duplicate coalescing, rearm, persistent dispatcher ownership; HCI-poll timer never lost");
 return 0;
}
'''
fixture=fixture.replace('@SOURCE@','#include "'+source.as_posix()+'"')
src=out/'callout.c';src.write_text(fixture)
args=['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-type-limits','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie','-I'+str(headers),'-I'+str(port/'include')]
if 'ble_npl_nuttx_callout_read' in source.read_text():args+=['-DPORT_STATS']
subprocess.run([*args,str(src),'-o',str(out/'test')],check=True)
result=subprocess.run([str(out/'test')],capture_output=True,text=True,timeout=20,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
(out/'test.log').write_text(result.stdout+result.stderr);print(result.stdout+result.stderr,end='')
assert result.returncode==0,f'callout regression exit={result.returncode}'
