#!/usr/bin/env python3
"""Exercise real capture/session handoff with a concurrent background DMA owner."""
from pathlib import Path
import argparse,subprocess,os
from render_all_ui import function
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[1];text=(root/'app/espdl-quickapp/overlay/apps/system/desktop/glass_music_pcm.c').read_text()
pre=r'''#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#define GLASS_LOCAL_SPEECH
static pthread_mutex_t g_pcm_session_lock=PTHREAD_MUTEX_INITIALIZER;
static atomic_bool g_pcm_session_active;
static atomic_uint g_pcm_foreground_waiters;
static atomic_bool background,ready,released,stop;
static unsigned yields;
static int (*glass_local_voice_yield_model)(unsigned);
bool music_pcm_foreground_pending(void);
static bool glass_speech_yield_background(void){yields++;if(!atomic_load(&background))return false;atomic_store(&stop,true);return true;}
static void glass_speech_stop(void){if(atomic_load(&background))atomic_store(&stop,true);}
'''
body='\n'.join(function(text,n) for n in ['music_pcm_session_lock','music_pcm_session_trylock','music_pcm_session_unlock','music_pcm_session_active','music_pcm_foreground_pending','music_pcm_capture_lock'])
post=r'''
static void *owner(void *unused){
 (void)unused;assert(!music_pcm_session_trylock());atomic_store(&background,true);atomic_store(&ready,true);
 while(!atomic_load(&stop))usleep(1000);
 assert(music_pcm_foreground_pending());usleep(30000);
 atomic_store(&released,true);atomic_store(&background,false);music_pcm_session_unlock();return NULL;
}
static bool cancelled(void *unused){(void)unused;return atomic_load(&stop);}
static bool always(void *unused){(void)unused;return true;}
static void reset(void){assert(!music_pcm_session_active()&&!music_pcm_foreground_pending());background=ready=released=stop=false;yields=0;}
static void begin(pthread_t *thread){assert(!pthread_create(thread,NULL,owner,NULL));while(!atomic_load(&ready))usleep(1000);}
int main(void){
 pthread_t thread;reset();assert(!music_pcm_capture_lock(100,NULL,NULL));assert(music_pcm_session_active()&&!music_pcm_foreground_pending());music_pcm_session_unlock();
 reset();begin(&thread);assert(!music_pcm_capture_lock(1000,NULL,NULL));assert(released&&!background&&music_pcm_session_active()&&!music_pcm_foreground_pending());pthread_join(thread,NULL);music_pcm_session_unlock();
 reset();begin(&thread);assert(!music_pcm_session_lock());assert(released&&music_pcm_session_active()&&!music_pcm_foreground_pending());pthread_join(thread,NULL);music_pcm_session_unlock();
 reset();assert(!music_pcm_session_trylock());assert(music_pcm_capture_lock(100,NULL,NULL)==-EBUSY);assert(music_pcm_session_active()&&!music_pcm_foreground_pending());music_pcm_session_unlock();
 reset();assert(music_pcm_capture_lock(100,always,NULL)==-ECANCELED);assert(!yields&&!music_pcm_foreground_pending());
 reset();assert(!music_pcm_session_trylock());background=true;assert(music_pcm_capture_lock(10,NULL,NULL)==-ETIMEDOUT);assert(!music_pcm_foreground_pending());background=false;music_pcm_session_unlock();
 reset();assert(!music_pcm_session_trylock());background=true;assert(music_pcm_capture_lock(100,cancelled,NULL)==-ECANCELED);assert(!music_pcm_foreground_pending());background=false;music_pcm_session_unlock();
 reset();assert(music_pcm_capture_lock(5001,NULL,NULL)==-EINVAL);assert(!yields&&!music_pcm_foreground_pending());
 puts("CAPTURE_HANDOFF_OK: real mutex, background DMA retirement, recorder and ASR takeover, foreground busy, cancellation, timeout, wake resumes");
}
'''
c=a.output/'test.c';c.write_text(pre+body+post);exe=a.output/'test'
subprocess.run(['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-O1','-g','-pthread','-fsanitize=address,undefined','-fno-pie','-no-pie',str(c),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True,timeout=10,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
