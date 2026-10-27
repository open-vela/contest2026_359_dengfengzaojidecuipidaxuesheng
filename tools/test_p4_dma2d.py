#!/usr/bin/env python3
"""Exercise the real shared DMA2D manager, then compile both P4 driver units."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--nuttx',type=Path,required=True)
p.add_argument('--toolchain',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();repo=Path(__file__).resolve().parents[1]
chip=repo/'app/espdl-quickapp/overlay/nuttx/arch/risc-v/src/esp32p4'
overlay=repo/'app/espdl-quickapp/overlay/nuttx/include'
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
stubs={
 'nuttx/config.h':'#pragma once\n#include <stdbool.h>\n#include <stdint.h>\n#include <stddef.h>\n#define CONFIG_ESP32P4_PPA 1\n#define OK 0\n',
 'nuttx/compiler.h':'#pragma once\n#define FAR\n',
 'nuttx/arch.h':'#pragma once\nvoid up_enable_irq(int);\n',
 'nuttx/irq.h':'#pragma once\ntypedef unsigned irqstate_t;\nunsigned enter_critical_section(void);\nvoid leave_critical_section(unsigned);\n',
 'nuttx/kmalloc.h':'#pragma once\n#include <stddef.h>\nvoid *kmm_zalloc(size_t);\nvoid kmm_free(void *);\n',
 'nuttx/mutex.h':'''#pragma once
#include <pthread.h>
typedef pthread_mutex_t mutex_t;
#define NXMUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
static inline int nxmutex_lock(mutex_t *m){return -pthread_mutex_lock(m);}
static inline int nxmutex_unlock(mutex_t *m){return -pthread_mutex_unlock(m);}
static inline void nxmutex_init(mutex_t *m){pthread_mutex_init(m,0);}
static inline void nxmutex_destroy(mutex_t *m){pthread_mutex_destroy(m);}
''',
 'nuttx/spinlock.h':'''#pragma once
#include <pthread.h>
typedef pthread_mutex_t spinlock_t;
#define SP_UNLOCKED PTHREAD_MUTEX_INITIALIZER
static inline unsigned spin_lock_irqsave(spinlock_t *m){pthread_mutex_lock(m);return 0;}
static inline void spin_unlock_irqrestore(spinlock_t *m,unsigned f){(void)f;pthread_mutex_unlock(m);}
''',
 'soc/interrupts.h':'#pragma once\n#define ETS_DMA2D_IN_CH0_INTR_SOURCE 1\n',
 'esp_irq.h':'''#pragma once
#define ESP_IRQ_PRIORITY_DEFAULT 1
#define ESP_IRQ_TRIGGER_LEVEL 1
#define ESP_SOURCE2IRQ(n) (n)
int esp_setup_irq(int,int,int,int (*)(int,void *,void *),void *);
''',
 'hal/dma2d_ll.h':'''#pragma once
#include <stdint.h>
#include <stdbool.h>
typedef struct {struct {struct {uint32_t val;} in_int_raw;} in_channel[1];} dma2d_dev_t;
extern dma2d_dev_t test_dma;
extern unsigned test_stops,test_mask,test_scramble;
#define DMA2D_LL_GET_HW(n) (&test_dma)
#define dma2d_ll_enable_bus_clock(n,v) ((void)0)
#define dma2d_ll_reset_register(n) do {test_scramble=0;} while(0)
static inline void dma2d_ll_hw_enable(dma2d_dev_t *d,bool v){(void)d;(void)v;}
static inline void dma2d_ll_rx_clear_interrupt_status(dma2d_dev_t *d,int c,uint32_t s){(void)c;d->in_channel[0].in_int_raw.val&=~s;}
static inline void dma2d_ll_tx_clear_interrupt_status(dma2d_dev_t *d,int c,uint32_t s){(void)d;(void)c;(void)s;}
static inline void dma2d_ll_rx_enable_interrupt(dma2d_dev_t *d,int c,uint32_t s,bool on){(void)d;(void)c;test_mask=on?test_mask|s:test_mask&~s;}
static inline void dma2d_ll_tx_enable_interrupt(dma2d_dev_t *d,int c,uint32_t s,bool on){dma2d_ll_rx_enable_interrupt(d,c,s,on);}
static inline void dma2d_ll_tx_stop(dma2d_dev_t *d,int c){(void)d;(void)c;test_stops++;}
static inline void dma2d_ll_rx_stop(dma2d_dev_t *d,int c){(void)d;(void)c;test_stops++;}
'''}
for name,text in stubs.items():
 f=out/'include'/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_text(text)
fixture=repo/'app/espdl-quickapp/tests/p4_dma2d_host.c'
source=chip/'esp32p4_dma2d.c'
cmd=['gcc','-std=gnu11','-D_GNU_SOURCE','-O1','-g','-pthread','-Wall','-Wextra',
 '-Wno-unused-parameter','-Wno-unused-variable','-Werror','-fsanitize=address,undefined',
 '-fno-omit-frame-pointer','-fno-pie','-no-pie','-I'+str(out/'include'),'-I'+str(chip),
 '-DTEST_SOURCE="'+str(source)+'"',str(fixture),'-o',str(out/'owner-host')]
def run(cmd,name,timeout=60):
 result=subprocess.run(cmd,capture_output=True,text=True,timeout=timeout,
  env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
 (out/(name+'.log')).write_text(result.stdout+result.stderr)
 if result.returncode:raise RuntimeError(name+' failed:\n'+result.stdout+result.stderr)
 return result.stdout
run(cmd,'host-compile');print(run([str(out/'owner-host')],'host-run'),end='')
nuttx=a.nuttx.resolve();hal=nuttx/'arch/risc-v/src/esp32p4/esp-hal-3rdparty'
includes=[chip,overlay,nuttx/'include',nuttx/'arch/risc-v/src/common/espressif',
 hal/'nuttx/esp32p4/include',hal/'nuttx/include',
 hal/'components/soc/esp32p4/register/hw_ver3',hal/'components/soc/esp32p4/include']
for folder in sorted((hal/'components').rglob('include')):
 if any(x.startswith('esp32') and x!='esp32p4' for x in folder.parts):continue
 if any(x.startswith('hw_ver') and x!='hw_ver3' for x in folder.parts):continue
 includes.append(folder)
objects={}
for name in ['esp32p4_dma2d.c','esp32p4_ppa.c','esp32p4_jpeg.c','esp32p4_jpeg_emit.c']:
 obj=out/(name+'.o')
 cmd=[str(a.toolchain/'riscv32-esp-elf-gcc'),'-c','-Os','-g','-std=gnu11',
 '-march=rv32imafc_zicsr_zifencei','-mabi=ilp32f','-Werror=implicit-function-declaration',
 '-fstack-usage','-DCONFIG_ESP32P4_JPEG=1',
 *['-I'+str(i) for i in dict.fromkeys(includes)],str(chip/name),'-o',str(obj)]
 run(cmd,name+'-target');objects[name]=hashlib.sha256(obj.read_bytes()).hexdigest()
 print(name+': TARGET_COMPILE_OK')
paths=[source,chip/'esp32p4_dma2d.h',chip/'esp32p4_ppa.c',chip/'esp32p4_jpeg.c',
 overlay/'nuttx/video/p4_jpeg.h',fixture,Path(__file__).resolve(),
 chip/'Make.defs',chip/'CMakeLists.txt',nuttx/'.config']
(out/'RESULT.json').write_text(json.dumps({'passed':True,'hardware_tested':False,
 'scope':'Real shared manager with POSIX concurrency/fake registers; actual P4 compile of both manager and PPA',
 'objects_sha256':objects,'source_sha256':{str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in paths}},indent=2)+'\n')
