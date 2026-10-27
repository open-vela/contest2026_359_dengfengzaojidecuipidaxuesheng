#!/usr/bin/env python3
"""Exercise production I2S drains under immediate resubmission and low heap."""
import argparse
import os
from pathlib import Path
import subprocess
from render_all_ui import function

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--source', type=Path)
a = p.parse_args()
repo = Path(__file__).resolve().parents[1]
source = a.source or repo / 'app/espdl-quickapp/overlay/nuttx/arch/risc-v/src/common/espressif/esp_i2s.c'
text = source.read_text()
names = ['i2s_buf_free', 'i2s_buf_allocate', 'i2s_tx_worker', 'i2s_rx_worker']
bodies = '\n'.join(function(text, name) for name in names)
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define OK 0
#define DEBUGASSERT assert
#define VERIFY(x) assert((x)==0)
#define i2sinfo(...) ((void)0)
#define SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE 1
#define CACHE_LL_LEVEL_INT_MEM 0
#define CACHE_TYPE_DATA 0
#define ESP_CACHE_MSYNC_FLAG_DIR_M2C 1
#define ESP_OK 0
#define AUDIO_APB_FINAL 1
#define I2S_ROLE_MASTER 1
#define I2S_ROLE_SLAVE 2
#define I2S_DMADESC_NUM 2
#define ALIGN_UP(n,a) (((n)+(a)-1)&~((a)-1))
#define STAILQ_NEXT(p,f) ((p)->next)
typedef unsigned irqstate_t;
typedef struct entry {struct entry *next;} sq_entry_t;
typedef struct {sq_entry_t *head,*tail;} queue_t;
struct i2s_dev_s {int unused;};
struct ap_buffer_s {unsigned nbytes,nmaxbytes,flags;int refs;unsigned char samp[960];};
typedef struct desc {unsigned length;unsigned char *buf;struct desc *next;} lldesc_t;
typedef void (*i2s_callback_t)(struct i2s_dev_s *,struct ap_buffer_s *,void *,int);
struct esp_buffer_s {
 struct esp_buffer_s *flink;lldesc_t *dma_link[2];i2s_callback_t callback;
 unsigned timeout;void *arg;struct ap_buffer_s *apb;uint8_t *buf;uint32_t nbytes;int result;
};
struct direction {queue_t done,act,pend;};
struct config {bool tx_en,rx_en;int role;};
struct esp_i2s_s {struct i2s_dev_s dev;struct direction tx,rx;struct config *config;
 bool streaming,stopping;int slock,bufsem;struct esp_buffer_s *bf_freelist;struct esp_buffer_s containers[4];};
static unsigned heap_used,heap_limit,peak;static int depth,cache_error;
static void *kmm_memalign(unsigned alignment,size_t n){
 (void)alignment;assert(!depth);
 if(heap_used+n>heap_limit)return NULL;
 size_t *p=malloc(sizeof(size_t)+n);assert(p);*p=n;heap_used+=n;
 if(heap_used>peak)peak=heap_used;return p+1;
}
static void kmm_free(void *v){assert(!depth);if(v){size_t *p=(size_t *)v-1;assert(heap_used>=*p);heap_used-=*p;free(p);}}
static int nxsem_wait_uninterruptible(int *s){if(!*s)return -EAGAIN;--*s;return 0;}
static int nxsem_post(int *s){++*s;return 0;}
static unsigned cache_hal_get_cache_line_size(int a,int b){(void)a;(void)b;return 64;}
static int esp_cache_msync(void *p,size_t n,int flags){(void)p;(void)n;assert(flags==1);return cache_error;}
static irqstate_t spin_lock_irqsave_nopreempt(int *s){(void)s;assert(!depth);depth=1;return 0;}
static void spin_unlock_irqrestore_nopreempt(int *s,irqstate_t f){(void)s;(void)f;assert(depth);depth=0;}
static void *sq_remfirst(queue_t *q){sq_entry_t *e=q->head;if(e){q->head=e->next;if(!q->head)q->tail=NULL;}return e;}
static void sq_addlast(sq_entry_t *e,queue_t *q){e->next=NULL;if(q->tail)q->tail->next=e;else q->head=e;q->tail=e;}
static bool sq_empty(queue_t *q){return !q->head;}
static void apb_free(struct ap_buffer_s *p){assert(p->refs>0);--p->refs;}
static int i2s_rx_channel_stop(struct esp_i2s_s *p){(void)p;return 0;}
static int i2s_tx_channel_stop(struct esp_i2s_s *p){(void)p;return 0;}
'''
suffix = r'''
static struct esp_buffer_s *replacement;
static struct ap_buffer_s next_apb;
static int calls,expected_result;static bool receive;
static void callback(struct i2s_dev_s *dev,struct ap_buffer_s *apb,void *arg,int result){
 struct esp_i2s_s *p=(struct esp_i2s_s *)dev;
 assert(!depth && arg==&calls && result==expected_result && apb->refs==2);
 if(receive){
  assert(apb->nbytes==(result==0?960u:0u));
  if(!result)for(unsigned i=0;i<960;i++)assert(apb->samp[i]==0x5a);
 }
 ++calls;
 /* The higher priority codec can submit before this callback returns.
  * Allocate using the real container allocator with no spare DMA heap.
  */
 replacement=i2s_buf_allocate(p);
 if(!replacement){fprintf(stderr,"REPRODUCED: immediate resubmit exhausted DMA heap\n");exit(42);}
 replacement->buf=kmm_memalign(64,960);assert(replacement->buf);
 replacement->apb=&next_apb;next_apb.refs=2;
 replacement->arg=NULL;replacement->result=123;
 /* Simulate the codec/app dropping their old ownership during callback. */
 apb_free(apb);assert(apb->refs==1);
}
int main(void){
 for(int rx=0;rx<2;rx++)for(int mode=0;mode<4;mode++){
  struct config c={true,true,I2S_ROLE_MASTER};struct esp_i2s_s p={.config=&c,.streaming=true};
  struct ap_buffer_s apb={.refs=2,.nmaxbytes=960};calls=0;receive=rx;cache_error=mode==3?-1:0;
  heap_limit=960+128;heap_used=0;peak=0;
  for(int i=0;i<4;i++)assert(i2s_buf_free(&p,&p.containers[i])==0);
  struct esp_buffer_s *b=i2s_buf_allocate(&p);assert(b);
  b->buf=kmm_memalign(64,960);assert(b->buf);memset(b->buf,0x5a,960);
  b->apb=&apb;b->callback=callback;b->arg=&calls;b->nbytes=960;b->result=mode==1?-ECANCELED:0;
  b->dma_link[0]->length=mode==2?961:960;b->dma_link[0]->buf=b->buf;b->dma_link[0]->next=NULL;
  expected_result=mode==1?-ECANCELED:rx&&(mode==2||mode==3)?-EIO:0;
  sq_addlast((sq_entry_t *)b,rx?&p.rx.done:&p.tx.done);
  if(rx)i2s_rx_worker(&p);else i2s_tx_worker(&p);
  assert(calls==1 && apb.refs==0 && next_apb.refs==2 && replacement->result==123);
  assert(p.bufsem==3 && !depth && peak<=heap_limit);
  kmm_free(replacement->buf);assert(i2s_buf_free(&p,replacement)==0);
  assert(!heap_used && p.bufsem==4);
 }
 puts("I2S_HANDOFF_PASS: TX/RX immediate resubmit at exact heap budget; original APB reference; reused container isolation; cancel/cache/length errors; no leaks");
}
'''
a.output.mkdir(parents=True, exist_ok=True)
c = a.output / 'i2s-handoff.c'
c.write_text(prefix + bodies + suffix)
exe = a.output / 'i2s-handoff'
subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                '-Wno-misleading-indentation', '-g', '-O1', '-fno-pie', '-no-pie',
                '-fsanitize=address,undefined', str(c), '-o', str(exe)], check=True)
result = subprocess.run([str(exe)], env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0' if a.source else 'detect_leaks=1',
                                       'UBSAN_OPTIONS': 'halt_on_error=1'})
raise SystemExit(result.returncode)
