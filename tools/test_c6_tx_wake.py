#!/usr/bin/env python3
"""Exercise production coalesced TX wake with real pthreads and semaphores."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess
from render_all_ui import function

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
source = root / 'app/espdl-quickapp/overlay/apps/system/c6probe/c6net.c'
text = source.read_text()
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
base = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#define C6NET_POLL_US 10000
static sem_t g_c6net_tx_event;
static bool g_c6net_tx_event_ready;
static bool g_c6net_tx_signalled;
static bool g_c6net_tx_wake_enabled=true;
static bool fail_post, timeout_race, fail_clock;
static int test_post(sem_t *s) {
 if(fail_post){errno=EOVERFLOW;return -1;}
 return sem_post(s);
}
static int test_clock(clockid_t c,struct timespec *t) {
 if(fail_clock){errno=EINVAL;return -1;}
 return clock_gettime(c,t);
}
static void c6net_tx_notify(void);
static int test_wait(sem_t *s,clockid_t c,const struct timespec *t) {
 if(timeout_race){timeout_race=false;c6net_tx_notify();errno=ETIMEDOUT;return -1;}
 return sem_clockwait(s,c,t);
}
#define sem_post test_post
#define sem_clockwait test_wait
#define clock_gettime test_clock
'''
body = '\n'.join(function(text, name) for name in
                 ['c6net_tx_wake', 'c6net_tx_notify', 'c6net_tx_wait'])
main = r'''
static unsigned produced, consumed, done;
static void *producer(void *unused) {
 (void)unused;
 for(unsigned i=0;i<100000;i++) {
  __atomic_fetch_add(&produced,1,__ATOMIC_RELEASE);
  c6net_tx_notify();
 }
 __atomic_fetch_add(&done,1,__ATOMIC_RELEASE);return NULL;
}
int main(void) {
 assert(!sem_init(&g_c6net_tx_event,0,0));int n;
 c6net_tx_notify();assert(!sem_getvalue(&g_c6net_tx_event,&n)&&n==0);
 g_c6net_tx_event_ready=true;
 for(int i=0;i<100000;i++)c6net_tx_notify();
 assert(!sem_getvalue(&g_c6net_tx_event,&n)&&n==1);
 c6net_tx_wait();assert(!g_c6net_tx_signalled);
 timeout_race=true;c6net_tx_wait();assert(g_c6net_tx_signalled);
 c6net_tx_notify();assert(!sem_getvalue(&g_c6net_tx_event,&n)&&n==1);
 c6net_tx_wait();assert(!g_c6net_tx_signalled);
 c6net_tx_wake(false);c6net_tx_notify();
 assert(!sem_getvalue(&g_c6net_tx_event,&n)&&n==0);
 c6net_tx_wait();assert(!g_c6net_tx_signalled);
 c6net_tx_wake(true);fail_post=true;c6net_tx_notify();
 assert(!g_c6net_tx_signalled);fail_post=false;
 c6net_tx_notify();c6net_tx_wait();assert(!g_c6net_tx_signalled);
 fail_clock=true;c6net_tx_wait();fail_clock=false;
 pthread_t threads[4];
 for(int i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,producer,NULL));
 while(__atomic_load_n(&done,__ATOMIC_ACQUIRE)<4 || consumed<__atomic_load_n(&produced,__ATOMIC_ACQUIRE)) {
  consumed=__atomic_load_n(&produced,__ATOMIC_ACQUIRE);
  c6net_tx_wait();
 }
 for(int i=0;i<4;i++)assert(!pthread_join(threads[i],NULL));
 consumed=__atomic_load_n(&produced,__ATOMIC_ACQUIRE);
 assert(consumed==400000);
 assert(!sem_getvalue(&g_c6net_tx_event,&n)&&n<=1);
 c6net_tx_wait();assert(!g_c6net_tx_signalled);
 assert(!sem_destroy(&g_c6net_tx_event));
 puts("C6_TX_WAKE_OK: bounded coalescing, timeout/post race, disabled mode, post/clock failures, 400000 concurrent notifications");
 return 0;
}
'''
file = out / 'wake.c'
file.write_text(base + body + main)
exe = out / 'wake'
subprocess.run(['gcc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                '-Werror', '-pthread', '-fsanitize=address,undefined',
                str(file), '-o', str(exe)], check=True)
result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
(out / 'wake.log').write_text(result.stdout + result.stderr)
assert result.returncode == 0, result.stderr
print(result.stdout, end='')
(out / 'RESULT.json').write_text(json.dumps({
    'passed': True, 'production_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
    'scope': 'real coalescing/wait bodies with POSIX semaphore; injected timeout/post and error paths; four producer threads',
}, indent=2) + '\n')
