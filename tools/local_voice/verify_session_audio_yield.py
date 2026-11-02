#!/usr/bin/env python3
"""Actual foreground PCM admission functions with inert owner/open doubles."""
import argparse,hashlib,json,subprocess,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from render_all_ui import function
def main():
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    repo=Path(__file__).resolve().parents[2];source=repo/'app/espdl-quickapp/overlay/apps/system/desktop/glass_music_pcm.c'
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    code='''#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
struct music_pcm {int unused;};
static pthread_mutex_t g_pcm_session_lock=PTHREAD_MUTEX_INITIALIZER;
static atomic_bool g_pcm_session_active;
static atomic_uint g_pcm_foreground_waiters;
static int yield_result;static unsigned yields,opens;
int glass_local_voice_yield_model(unsigned timeout){assert(timeout<=5000);yields++;return yield_result;}
int music_pcm_try_open(struct music_pcm *p,unsigned rate,unsigned volume){(void)p;(void)rate;(void)volume;assert(yields);opens++;return 0;}
static bool stopped(void *unused){(void)unused;return true;}
'''
    code+='\n'.join(function(source.read_text(),n) for n in ('music_pcm_session_lock','music_pcm_foreground_open'))
    code+='''
int main(void){struct music_pcm pcm={0};yield_result=-EBUSY;
 assert(music_pcm_session_lock()==-EBUSY&&!atomic_load(&g_pcm_session_active));
 assert(!pthread_mutex_trylock(&g_pcm_session_lock));pthread_mutex_unlock(&g_pcm_session_lock);
 assert(music_pcm_foreground_open(&pcm,24000,40,3000,NULL,NULL)==-EBUSY);
 assert(!opens&&!atomic_load(&g_pcm_foreground_waiters));
 yield_result=0;assert(!music_pcm_session_lock()&&atomic_load(&g_pcm_session_active));
 atomic_store(&g_pcm_session_active,false);pthread_mutex_unlock(&g_pcm_session_lock);
 assert(!music_pcm_foreground_open(&pcm,24000,40,3000,NULL,NULL)&&opens==1);
 assert(!atomic_load(&g_pcm_foreground_waiters));unsigned before=yields;
 assert(music_pcm_foreground_open(&pcm,24000,40,3000,stopped,NULL)==-ECANCELED&&yields==before);
 return 0;}
'''
    (out/'audio-yield.c').write_text(code);exe=out/'audio-yield'
    subprocess.run(['gcc','-O2','-g','-fsanitize=address,undefined','-fno-pie','-no-pie',str(out/'audio-yield.c'),'-pthread','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=10)
    (out/'VERIFICATION.json').write_text(json.dumps(dict(passed=True,scope='Actual PCM admission; model yield occurs before codec/lock acquisition; busy/cancel leaves no waiter',source_sha256={str(source.relative_to(repo)):hashlib.sha256(source.read_bytes()).hexdigest()}),indent=2)+'\n')
    print('PCM_MODEL_YIELD_ADMISSION_OK')
if __name__=='__main__':main()
