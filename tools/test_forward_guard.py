#!/usr/bin/env python3
"""Check bounded, locked forwarding re-notification without freeing frames."""
from pathlib import Path
import argparse,subprocess,json,hashlib
from render_all_ui import function
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--notify',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
r=Path(__file__).resolve().parents[1];source=r/'app/espdl-quickapp/overlay/nuttx/net/espclaw_share.c';header=r/'app/espdl-quickapp/overlay/nuttx/include/nuttx/net/espclaw_share.h'
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
base=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <errno.h>
#define FAR
#define IPFWD_POLL 64
#define IFF_IS_UP(f) ((f)&1)
struct devif_callback_s {uint32_t flags;struct devif_callback_s *nxtconn,*nxtdev;};
struct fake_lock {bool busy,held;};
struct net_driver_s {struct fake_lock d_lock;uint32_t d_polltype,d_flags;struct devif_callback_s *d_conncb,*d_devcb;int (*d_txavail)(struct net_driver_s *);unsigned notified;};
static int notify(struct net_driver_s *d){assert(d->d_lock.held);d->notified++;return 0;}
static struct net_driver_s eth={.d_flags=1,.d_txavail=notify},ap={.d_flags=1,.d_txavail=notify};
static struct {bool active;} g_share={true};
static bool nat_held;
static void nat_lock(void){assert(!nat_held&&!eth.d_lock.held&&!ap.d_lock.held);nat_held=true;}
static void nat_unlock(void){assert(nat_held);nat_held=false;}
static bool g_fwd_enabled=true;
static uint32_t g_fwd_wakeups[2],g_fwd_missing[2];
static uint64_t g_fwd_last_ms,now_ms=100;
static struct net_driver_s *netdev_findbyname(const char *n){return !strcmp(n,"eth0")?&eth:!strcmp(n,"ap0")?&ap:NULL;}
static int nxrmutex_trylock(struct fake_lock *l){if(l->busy)return -EBUSY;assert(!l->held);l->held=true;return 0;}
static void netdev_unlock(struct net_driver_s *d){assert(d->d_lock.held);d->d_lock.held=false;}
static int test_clock(clockid_t c,struct timespec *t){assert(c==CLOCK_MONOTONIC);t->tv_sec=now_ms/1000;t->tv_nsec=(now_ms%1000)*1000000;return 0;}
#define clock_gettime test_clock
'''
text=header.read_text();start=text.index('struct espclaw_share_fwd_dev_s');end=text.index('/****************************************************************************',start);types=text[start:end]
body='\n'.join(function(source.read_text(),n) for n in ['share_fwd_read','espclaw_share_fwd_enable','espclaw_share_fwd_status','espclaw_share_fwd_poll'])
main=r'''
int main(void){
 struct espclaw_share_fwd_s s;struct devif_callback_s pending={.flags=IPFWD_POLL},other={.flags=2};
 assert(espclaw_share_fwd_status(NULL)==-EINVAL);
 espclaw_share_fwd_poll();assert(!ap.notified&&!eth.notified);
 ap.d_conncb=&pending;ap.d_devcb=&other;other.nxtdev=&pending;ap.d_polltype=0x200;
 assert(!espclaw_share_fwd_status(&s)&&s.enabled&&s.ap.waiting==1&&s.ap.owned==1);
 now_ms=110;espclaw_share_fwd_poll();
 assert(ap.notified==1&&g_fwd_missing[1]==1&&ap.d_polltype==(0x200|IPFWD_POLL));
 assert(ap.d_conncb==&pending&&pending.flags==IPFWD_POLL);
 espclaw_share_fwd_poll();now_ms=119;espclaw_share_fwd_poll();assert(ap.notified==1);
 now_ms=120;espclaw_share_fwd_poll();assert(ap.notified==2&&g_fwd_missing[1]==1);
 espclaw_share_fwd_enable(false);now_ms=130;espclaw_share_fwd_poll();assert(ap.notified==2);
 assert(!espclaw_share_fwd_status(&s)&&!s.enabled);
 espclaw_share_fwd_enable(true);ap.d_lock.busy=true;
 struct devif_callback_s e={.flags=IPFWD_POLL};eth.d_conncb=eth.d_devcb=&e;
 now_ms=140;espclaw_share_fwd_poll();assert(eth.notified==1&&ap.notified==2);
 assert(!espclaw_share_fwd_status(&s)&&s.eth.available&&!s.ap.available);
 ap.d_lock.busy=false;ap.d_flags=0;now_ms=150;espclaw_share_fwd_poll();assert(ap.notified==2);
 ap.d_flags=1;pending.nxtconn= pending.nxtdev=&pending;
 now_ms=160;espclaw_share_fwd_poll();assert(ap.notified==2);
 assert(!espclaw_share_fwd_status(&s)&&s.ap.truncated&&s.ap.waiting==64);
 pending.nxtconn=pending.nxtdev=NULL;ap.d_conncb=NULL;
 now_ms=170;espclaw_share_fwd_poll();assert(ap.notified==2);
 assert(!espclaw_share_fwd_status(&s)&&s.ap.owned==1&&!s.ap.waiting);
 unsigned before=eth.notified;g_share.active=false;now_ms=180;
 espclaw_share_fwd_poll();assert(eth.notified==before);
 puts("FORWARD_GUARD_OK: locked deferred notify, missing poll flag, 10ms bound, busy/down/disabled, bounded cyclic-list handling, no allocation/free/relink");return 0;
}
'''
file=out/'guard.c';file.write_text(base+types+function(a.notify.read_text(),'netdev_txnotify_dev')+body+main);exe=out/'guard'
subprocess.run(['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie',str(file),'-o',str(exe)],check=True)
ret=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10);(out/'guard.log').write_text(ret.stdout+ret.stderr);assert not ret.returncode,ret.stderr;print(ret.stdout,end='')
(out/'RESULT.json').write_text(json.dumps({'passed':True,'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'scope':'actual guard/snapshot functions and real notify; deterministic clock, pending callback lists and mutex ownership; no hardware claim'},indent=2))
