/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include "hal/dma2d_ll.h"

dma2d_dev_t test_dma;
unsigned test_stops, test_mask, test_scramble;
static int fail_alloc, fail_irq, irq_setups, irq_enables, allocations;
static int (*test_isr)(int,void *,void *);
static void *test_isr_arg;
static atomic_int calls_a, calls_b, entered_b, callback_entered, unblock;

void *kmm_zalloc(size_t size)
{
  if(fail_alloc)return NULL;
  allocations++;return calloc(1,size);
}
void kmm_free(void *p){if(p)allocations--;free(p);}
unsigned enter_critical_section(void){return 0;}
void leave_critical_section(unsigned flags){(void)flags;}
void up_enable_irq(int irq){(void)irq;irq_enables++;}
int esp_setup_irq(int source,int priority,int trigger,
                  int (*handler)(int,void *,void *),void *arg)
{
  (void)source;(void)priority;(void)trigger;irq_setups++;
  if(fail_irq)return -EIO;
  test_isr=handler;test_isr_arg=arg;return 3;
}

#include TEST_SOURCE

static void callback_a(void *arg,uint32_t status)
{
  assert(arg==&calls_a&&status==1);
  atomic_store(&callback_entered,1);
  while(!atomic_load(&unblock))sched_yield();
  atomic_fetch_add(&calls_a,1);
}
static void callback_b(void *arg,uint32_t status)
{
  assert(arg==&calls_b&&status==2);atomic_fetch_add(&calls_b,1);
}
static void raise_irq(uint32_t status)
{
  test_dma.in_channel[0].in_int_raw.val=status;
  assert(test_isr(0,NULL,test_isr_arg)==0);
  assert(!test_dma.in_channel[0].in_int_raw.val);
}
static void *second_owner(void *unused)
{
  (void)unused;
  assert(!esp32p4_dma2d_acquire(callback_b,&calls_b));
  assert(atomic_load(&calls_a)==1);
  assert(test_mask==0&&test_stops>=2&&test_scramble==0);
  atomic_store(&entered_b,1);raise_irq(2);
  esp32p4_dma2d_release();return NULL;
}
static void *inflight_irq(void *unused)
{(void)unused;raise_irq(1);return NULL;}
static void *finish_irq(void *unused)
{
  (void)unused;usleep(20000);
  assert(!atomic_load(&entered_b));
  atomic_store(&unblock,1);return NULL;
}
int main(void)
{
  pthread_t waiter,irq,finish;
  assert(esp32p4_dma2d_acquire(NULL,&calls_a)==-EINVAL);
  assert(esp32p4_dma2d_acquire(callback_a,NULL)==-EINVAL);
  assert(!irq_setups&&!allocations);
  fail_alloc=1;
  assert(esp32p4_dma2d_acquire(callback_a,&calls_a)==-ENOMEM);
  fail_alloc=0;fail_irq=1;
  assert(esp32p4_dma2d_acquire(callback_a,&calls_a)==-EIO);
  assert(!allocations&&!irq_enables);
  fail_irq=0;
  assert(!esp32p4_dma2d_acquire(callback_a,&calls_a));
  assert(irq_setups==2&&irq_enables==1&&allocations==1);
  test_scramble=7;
  assert(!pthread_create(&waiter,NULL,second_owner,NULL));
  usleep(20000);assert(!atomic_load(&entered_b));
  assert(!pthread_create(&irq,NULL,inflight_irq,NULL));
  while(!atomic_load(&callback_entered))sched_yield();
  assert(!pthread_create(&finish,NULL,finish_irq,NULL));
  esp32p4_dma2d_release();
  assert(!pthread_join(irq,NULL));assert(!pthread_join(finish,NULL));
  assert(!pthread_join(waiter,NULL));
  assert(atomic_load(&calls_a)==1&&atomic_load(&calls_b)==1);
  raise_irq(1);raise_irq(2);
  assert(atomic_load(&calls_a)==1&&atomic_load(&calls_b)==1);
  assert(irq_setups==2&&irq_enables==1&&!test_mask);
  puts("P4_DMA2D_OWNER_OK: allocation/IRQ rollback, serialized owners, in-flight ISR handover, stray IRQ drain");
  return 0;
}
