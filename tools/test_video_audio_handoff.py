#!/usr/bin/env python3
"""Actual foreground PCM handoff with real mutexes and bounded fake codec I/O."""
import argparse,json,subprocess
from pathlib import Path
from render_all_ui import function
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True);a=p.parse_args()
repo=Path(__file__).resolve().parents[1];out=a.output;out.mkdir(parents=True,exist_ok=True)
desktop=repo/'app/espdl-quickapp/overlay/apps/system/desktop'
source=(desktop/'glass_music_pcm.c').read_text()
prefix=r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#define GLASS_LOCAL_SPEECH 1
struct music_pcm {bool opened;};
static pthread_mutex_t g_pcm_session_lock=PTHREAD_MUTEX_INITIALIZER;
static atomic_bool g_pcm_session_active;
static atomic_uint g_pcm_foreground_waiters;
static atomic_bool background,release_requested,holder_ready;
static unsigned opens,yields,owner_stops;
static int forced_error;
bool music_pcm_foreground_pending(void);
int music_pcm_session_trylock(void);
void music_pcm_session_unlock(void);
static bool glass_speech_yield_background(void)
{yields++;if(!atomic_load(&background))return false;atomic_store(&release_requested,true);return true;}
static int music_pcm_try_open(struct music_pcm *pcm,unsigned rate,unsigned volume)
{
  assert(rate==16000&&volume==0);opens++;
  if(forced_error)return forced_error;
  int r=music_pcm_session_trylock();if(!r)pcm->opened=true;return r;
}
'''
functions='\n'.join(function(source,n) for n in ['music_pcm_session_trylock','music_pcm_session_unlock',
 'music_pcm_session_active','music_pcm_foreground_pending','music_pcm_foreground_open'])
suffix=r'''
static void *wake_owner(void *p)
{
  (void)p;assert(!music_pcm_session_trylock());atomic_store(&holder_ready,true);
  while(!atomic_load(&release_requested))usleep(1000);
  assert(music_pcm_foreground_pending());
  usleep(40000); /* DMA callback/MQ teardown must finish before unlock. */
  music_pcm_session_unlock();return NULL;
}
static bool cancel_now(void *p){return *(bool *)p;}
static bool cancel_on_wait(void *p){(void)p;return opens>=2;}
static void reset(void)
{
  assert(!music_pcm_session_active()&&!music_pcm_foreground_pending());
  opens=yields=forced_error=0;atomic_store(&background,false);
  atomic_store(&release_requested,false);atomic_store(&holder_ready,false);
}
int main(void)
{
 struct music_pcm pcm={0};bool cancelled=false;
 reset();assert(!music_pcm_foreground_open(&pcm,16000,0,200,cancel_now,&cancelled));
 assert(pcm.opened&&music_pcm_session_active()&&!music_pcm_foreground_pending());music_pcm_session_unlock();
 reset();atomic_store(&background,true);pthread_t worker;
 assert(!pthread_create(&worker,NULL,wake_owner,NULL));
 while(!atomic_load(&holder_ready))usleep(1000);
 assert(!music_pcm_foreground_open(&pcm,16000,0,1000,NULL,NULL));
 pthread_join(worker,NULL);assert(opens>1&&music_pcm_session_active()&&!music_pcm_foreground_pending());
 music_pcm_session_unlock();
 reset();assert(!music_pcm_session_trylock());
 assert(music_pcm_foreground_open(&pcm,16000,0,200,NULL,NULL)==-EBUSY);
 assert(opens==1&&!atomic_load(&release_requested)&&music_pcm_session_active());music_pcm_session_unlock();
 reset();atomic_store(&background,true);forced_error=-EBUSY;
 assert(music_pcm_foreground_open(&pcm,16000,0,20,NULL,NULL)==-ETIMEDOUT);
 assert(!music_pcm_foreground_pending());
 reset();cancelled=true;
 assert(music_pcm_foreground_open(&pcm,16000,0,200,cancel_now,&cancelled)==-ECANCELED);
 assert(!yields&&!opens&&!music_pcm_foreground_pending());
 reset();atomic_store(&background,true);forced_error=-EBUSY;
 assert(music_pcm_foreground_open(&pcm,16000,0,200,cancel_on_wait,NULL)==-ECANCELED);
 assert(!music_pcm_foreground_pending());
 reset();forced_error=-ENOMEM;
 assert(music_pcm_foreground_open(&pcm,16000,0,200,NULL,NULL)==-ENOMEM);
 assert(!music_pcm_foreground_pending());
 reset();assert(music_pcm_foreground_open(&pcm,0,0,200,NULL,NULL)==-EINVAL&&!yields&&!opens);
 assert(music_pcm_foreground_open(&pcm,16000,0,5001,NULL,NULL)==-EINVAL&&!yields);
 puts("VIDEO_AUDIO_HANDOFF_OK: background DMA teardown before take, foreground busy preserved, timeout/cancel/error cleanup, no stranded wake suppression");
}
'''
(out/'test.c').write_text(prefix+functions+suffix)
subprocess.run(['gcc','-std=gnu11','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread',str(out/'test.c'),'-o',str(out/'test')],check=True)
subprocess.run([str(out/'test')],check=True,timeout=10)
(out/'RESULT.json').write_text(json.dumps(dict(passed=True,actual_handoff_code=True,
 real_mutex_and_background_teardown_thread=True,codec_transport_mocked=True,hardware_tested=False),indent=2)+'\n')
