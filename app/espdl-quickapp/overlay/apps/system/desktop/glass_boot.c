/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_boot.h"
#include <stdatomic.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#define BOOT_EVENTS 40
struct boot_event {char stage[40];uint32_t at,delta;int result;};
struct boot_trace {uint64_t start,last;unsigned count;struct boot_event events[BOOT_EVENTS];};
static struct boot_trace *trace;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static atomic_bool active,ready;
static uint64_t milliseconds(void)
{struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
bool glass_boot_enter(void){return !atomic_exchange(&active,true);}
bool glass_boot_active(void){return atomic_load(&active);}
bool glass_boot_ui_ready(void){return atomic_load(&ready);}
void glass_boot_set_ui_ready(void){atomic_store(&ready,true);}
void glass_boot_stage(const char *stage,int result)
{
 pthread_mutex_lock(&lock);uint64_t now=milliseconds();
 if(!trace){trace=calloc(1,sizeof(*trace));if(trace)trace->start=trace->last=now;}
 if(trace){
  struct boot_event e={.at=(uint32_t)(now-trace->start),.delta=(uint32_t)(now-trace->last),.result=result};
  snprintf(e.stage,sizeof(e.stage),"%s",stage);trace->last=now;
  if(trace->count==BOOT_EVENTS){memmove(trace->events,trace->events+1,(BOOT_EVENTS-1)*sizeof(e));trace->count--;}
  trace->events[trace->count++]=e;
  /* Do not depend on stdio buffering or an NSH session already being open.
   * No host/DTR must never block startup. The in-RAM trace is authoritative. */
  char line[128];int n=snprintf(line,sizeof(line),"\nBOOT_STAGE ms=%lu delta=%lu stage=%s result=%d\n",(unsigned long)e.at,(unsigned long)e.delta,e.stage,result);
  int fd=open("/dev/console",O_WRONLY|O_NONBLOCK);if(fd>=0){(void)write(fd,line,(size_t)n);close(fd);}
 }
 pthread_mutex_unlock(&lock);
}
void glass_boot_report(void)
{
 pthread_mutex_lock(&lock);
 printf("BOOT_STATUS active=%u ui_ready=%u events=%u\n",glass_boot_active(),glass_boot_ui_ready(),trace?trace->count:0);
 if(trace)for(unsigned i=0;i<trace->count;i++){
  struct boot_event *e=&trace->events[i];printf("BOOT_STAGE ms=%lu delta=%lu stage=%s result=%d\n",(unsigned long)e->at,(unsigned long)e->delta,e->stage,e->result);
 }
 pthread_mutex_unlock(&lock);
}
