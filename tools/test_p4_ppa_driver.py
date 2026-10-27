#!/usr/bin/env python3
"""Run actual PPA validation/ioctl/register sequence with fake DMA completion."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
from render_all_ui import function

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--nuttx',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();repo=Path(__file__).resolve().parents[1];out=a.output.resolve()
out.mkdir(parents=True,exist_ok=True)
source=repo/'app/espdl-quickapp/overlay/nuttx/arch/risc-v/src/esp32p4/esp32p4_ppa.c'
ll=a.nuttx/'arch/risc-v/src/esp32p4/esp-hal-3rdparty/components/esp_hal_ppa/esp32p4/include/hal/ppa_ll.h'
text=source.read_text();names=['ppa_reset','ppa_interrupt','ppa_ram','ppa_run','ppa_ioctl']
body='\n\n'.join(function(text,n) for n in names)
private=text[text.index('struct p4_ppa_dev_s'):text.index('/****************************************************************************\n * Private Functions')]
block=function(ll.read_text(),'ppa_ll_srm_get_dma_dscr_port_mode_block_size')
custom={'ppa_ll_srm_get_dma_dscr_port_mode_block_size','ppa_ll_srm_set_rx_color_mode',
 'dma2d_ll_tx_set_dscr_port_block_size','dma2d_ll_tx_start','dma2d_ll_rx_start',
 'dma2d_ll_tx_stop','dma2d_ll_rx_stop','dma2d_ll_reset_register','ppa_ll_reset_register'}
llcalls=set(re.findall(r'\b((?:ppa|dma2d)_ll_\w+)\(',body))
macros='\n'.join('#define '+n+'(...) ((void)0)' for n in sorted(llcalls-custom))
for name,value in {'nuttx/compiler.h':'#pragma once\n#define FAR\n','nuttx/fs/ioctl.h':'#pragma once\n#define _IOC(a,b) ((a)|(b))\n'}.items():
 path=out/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(value)
prelude=r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <pthread.h>
#include <nuttx/video/p4_ppa.h>
#define OK 0
#define FAR
#define MSEC2TICK(n) (n)
#define HAL_CONFIG(...) 300
typedef unsigned irqstate_t;
typedef unsigned spinlock_t;
typedef int sem_t;
typedef pthread_mutex_t mutex_t;
typedef enum {PPA_SRM_COLOR_MODE_YUV422_UYVY,PPA_SRM_COLOR_MODE_RGB565,
 PPA_SRM_COLOR_MODE_ARGB8888,PPA_SRM_COLOR_MODE_RGB888,PPA_SRM_COLOR_MODE_GRAY8,
 PPA_SRM_COLOR_MODE_YUV420,PPA_SRM_COLOR_MODE_YUV422_VYUY,
 PPA_SRM_COLOR_MODE_YUV422_YUYV,PPA_SRM_COLOR_MODE_YUV422_YVYU} ppa_srm_color_mode_t;
typedef enum {PPA_LL_SRM_MB_SIZE_16_16,PPA_LL_SRM_MB_SIZE_32_32} ppa_ll_srm_mb_size_t;
typedef struct {struct {uint32_t val;} int_ena,int_clr,int_raw,sr_param_err_st;} ppa_dev_t;
typedef struct {struct {struct {uint32_t val;} in_int_raw;} in_channel[1];
 struct {struct {uint32_t val;} out_int_raw;} out_channel[1];} dma2d_dev_t;
typedef struct {unsigned vb_size,hb_length,va_size,ha_length,dma2d_en,suc_eof,owner,pbyte,x,y;void *buffer;} dma2d_descriptor_t;
struct inode {void *i_private;};struct file {struct inode *f_inode;};
static dma2d_dev_t dma_hw;
static ppa_dev_t ppa_hw;
#define DMA2D_LL_GET_HW(n) (&dma_hw)
#define PPA_LL_GET_HW (&ppa_hw)
#define DMA2D_LL_EVENT_RX_SUC_EOF 1
#define DMA2D_LL_EVENT_RX_DESC_ERROR 2
#define DMA2D_DESCRIPTOR_PBYTE_2B0_PER_PIXEL 2
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M 1
#define ESP_CACHE_MSYNC_FLAG_DIR_M2C 2
static unsigned starts,resets,acquires,releases,cache_calls,fail_cache,test_block_h,test_block_v,posts;
static unsigned interrupt_status=1,rx_mode;
static int wait_result;
static bool ram_valid=true,leased,tx_running,rx_running;
static void (*completion)(void *,uint32_t);
static void *completion_arg;
static struct p4_ppa_job_s *mutate;
static unsigned enter_critical_section(void){return 0;}
static void leave_critical_section(unsigned f){(void)f;}
static unsigned spin_lock_irqsave(spinlock_t *s){(void)s;return 0;}
static void spin_unlock_irqrestore(spinlock_t *s,unsigned f){(void)s;(void)f;}
static int nxmutex_lock(mutex_t *m){return -pthread_mutex_lock(m);}
static void nxmutex_unlock(mutex_t *m){pthread_mutex_unlock(m);}
static bool esp_ptr_internal(const void *p){(void)p;return false;}
static bool esp_ptr_external_ram(const void *p){(void)p;return ram_valid;}
static int esp_cache_msync(void *p,size_t n,int flags){assert(p&&n&&flags);return ++cache_calls==fail_cache?-1:0;}
static int nxsem_post(sem_t *s){(*s)++;posts++;return 0;}
static int nxsem_trywait(sem_t *s){if(*s){(*s)--;return 0;}return -EAGAIN;}
static int nxsem_tickwait_uninterruptible(sem_t *s,unsigned ticks){assert(ticks==250);
 if(wait_result)return wait_result;completion(completion_arg,interrupt_status);assert(*s);(*s)--;return 0;}
static int esp32p4_dma2d_acquire(void (*cb)(void *,uint32_t),void *arg){assert(!leased);
 leased=true;acquires++;completion=cb;completion_arg=arg;if(mutate)mutate->width=3;return 0;}
static void esp32p4_dma2d_release(void){assert(leased&&!tx_running&&!rx_running);leased=false;releases++;}
#define dma2d_ll_reset_register(...) do {tx_running=rx_running=false;} while(0)
#define ppa_ll_reset_register(...) do {resets++;} while(0)
#define dma2d_ll_tx_start(...) do {tx_running=true;starts++;} while(0)
#define dma2d_ll_rx_start(...) do {rx_running=true;starts++;} while(0)
#define dma2d_ll_tx_stop(...) do {tx_running=false;} while(0)
#define dma2d_ll_rx_stop(...) do {rx_running=false;} while(0)
#define ppa_ll_srm_set_rx_color_mode(p,m) do {(void)(p);rx_mode=(m);} while(0)
#define dma2d_ll_tx_set_dscr_port_block_size(d,c,h,v) do {(void)(d);(void)(c);test_block_h=(h);test_block_v=(v);} while(0)
#define syslog(...) ((void)0)
'''
main=r'''
int main(void)
{
 struct p4_ppa_dev_s dev={.lock=PTHREAD_MUTEX_INITIALIZER};
 dev.tx=memalign(128,128);dev.rx=memalign(128,128);
 void *input=memalign(128,4096),*output=memalign(128,4096);
 assert(dev.tx&&dev.rx&&input&&output);
 struct inode inode={.i_private=&dev};struct file file={.f_inode=&inode};
 struct p4_ppa_job_s good={.input=input,.output=output,.input_size=4096,.output_size=4096,
  .width=64,.height=32,.out_width=64,.out_height=32,.scale256=256},job=good;
 assert(ppa_ioctl(&file,999,0)==-ENOTTY&&!acquires);
 assert(ppa_ioctl(&file,P4_PPA_SRM_RGB565,0)==-EINVAL&&!acquires);
 assert(!ppa_ioctl(&file,P4_PPA_SRM,(unsigned long)&job));
 assert(rx_mode==PPA_SRM_COLOR_MODE_YUV422_UYVY&&test_block_h==20&&test_block_v==18);
 assert(!ppa_ioctl(&file,P4_PPA_SRM_RGB565,(unsigned long)&job));
 assert(rx_mode==PPA_SRM_COLOR_MODE_RGB565&&test_block_h==18&&test_block_v==18);
 assert(dev.tx->pbyte==2&&dev.rx->pbyte==2&&dev.tx->hb_length==64);
 job.width=3;job.height=4;job.out_width=3;job.out_height=4;
 unsigned before=starts;
 assert(ppa_ioctl(&file,P4_PPA_SRM,(unsigned long)&job)==-EINVAL&&starts==before);
 assert(!ppa_ioctl(&file,P4_PPA_SRM_RGB565,(unsigned long)&job));
 for(unsigned which=0;which<5;which++){
  job=good;
  if(which==0)job.input=(uint8_t *)input+1;
  if(which==1)job.input_size=128;
  if(which==2)job.output=input;
  if(which==3)job.scale256=17;
  if(which==4)job.out_x=1;
  before=starts;assert(ppa_ioctl(&file,P4_PPA_SRM_RGB565,(unsigned long)&job)==-EINVAL&&starts==before);
 }
 job=good;ram_valid=false;before=starts;
 assert(ppa_ioctl(&file,P4_PPA_SRM_RGB565,(unsigned long)&job)==-EINVAL&&starts==before);ram_valid=true;
 for(fail_cache=1;fail_cache<=4;fail_cache++){
  cache_calls=0;before=starts;
  assert(ppa_ioctl(&file,P4_PPA_SRM_RGB565,(unsigned long)&job)==-EIO&&starts==before);
 }
 fail_cache=0;cache_calls=0;mutate=&job;
 assert(!ppa_ioctl(&file,P4_PPA_SRM,(unsigned long)&job));
 assert(job.width==3&&dev.tx->hb_length==64);mutate=NULL;job=good;
 wait_result=-ETIMEDOUT;unsigned reset_before=resets;
 assert(ppa_ioctl(&file,P4_PPA_SRM_RGB565,(unsigned long)&job)==-ETIMEDOUT&&resets>reset_before);
 wait_result=0;interrupt_status=3;
 assert(ppa_ioctl(&file,P4_PPA_SRM_RGB565,(unsigned long)&job)==-EIO);
 interrupt_status=1;assert(!ppa_ioctl(&file,P4_PPA_SRM_RGB565,(unsigned long)&job));
 assert(!leased&&!tx_running&&!rx_running&&acquires==releases&&posts);
 free(input);free(output);free(dev.tx);free(dev.rx);pthread_mutex_destroy(&dev.lock);
 puts("PPA_RGB565_DRIVER_OK: real SDK RGB/YUV descriptor blocks, old ABI, odd RGB width, invalid/overlap/RAM guards, all four C2M failures stop before DMA, job snapshot, timeout/descriptor-error recovery");
 return 0;
}
'''
test=out/'driver.c';test.write_text(prelude+'\n'+macros+'\n'+block+'\n'+private+'\n'+body+'\n'+main)
cmd=['gcc','-std=gnu11','-O1','-g','-pthread','-Wall','-Wextra','-Werror','-Wno-unused-variable','-Wno-unused-parameter','-Wno-misleading-indentation',
 '-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie','-I'+str(out),
 '-I'+str(repo/'app/espdl-quickapp/overlay/nuttx/include'),str(test),'-o',str(out/'driver-host')]
for name,args in [('compile',cmd),('run',[str(out/'driver-host')])]:
 result=subprocess.run(args,capture_output=True,text=True,timeout=60,
  env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
 (out/(name+'.log')).write_text(result.stdout+result.stderr)
 if result.returncode:raise RuntimeError(name+' failed:\n'+result.stdout+result.stderr)
 print(result.stdout,end='')
(out/'RESULT.json').write_text(json.dumps({'passed':True,'hardware_tested':False,
 'scope':'actual PPA/ioctl bodies and actual SDK block-size function; fake registers/completion',
 'source_sha256':{str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in [source,ll,Path(__file__).resolve()]}},indent=2)+'\n')
