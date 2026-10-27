#!/usr/bin/env python3
"""Compile pinned callback lists and forwarding functions under ASan/UBSan.
Force allocation versus last-callback removal, then check deferred ownership.
"""
from pathlib import Path
import argparse,shutil,subprocess,json,hashlib,tempfile
from prepare_network_share import apply
from render_all_ui import function
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--reference',type=Path,required=True)
p.add_argument('--notify',type=Path,required=True)
p.add_argument('--callbacks',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
prepared=Path(tempfile.mkdtemp(prefix='prepared-',dir=out));shutil.copytree(a.reference,prepared,dirs_exist_ok=True);apply(prepared)
base=r"""
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#define FAR
#define OK 0
#define DEBUGASSERT assert
#define LPWORK 0
#define IPFWD_POLL 64
#define NETDEV_DOWN 128
#define IFF_IS_UP(f) ((f)&1)
#define DEVIF_CB_DONT_FREE 1
#define DEVIF_CB_PEND_FREE 2
struct iob_s {int unused;};
struct work_s {int unused;};
struct net_driver_s;
struct devif_callback_s {struct devif_callback_s *nxtconn,*prevconn,*nxtdev;uint32_t flags;void *priv;uint32_t (*event)(struct net_driver_s *,void *,uint32_t);unsigned free_flags;};
struct net_driver_s {unsigned d_flags;int d_lock;struct work_s d_fwdwork;struct iob_s *d_iob;uint8_t *d_buf;int d_len;uint32_t d_polltype;struct devif_callback_s *d_conncb,*d_conncb_tail,*d_devcb;unsigned notifications;};
struct forward_s {struct work_s f_work;struct net_driver_s *f_dev;struct devif_callback_s *f_cb;struct iob_s *f_iob;};
static struct devif_callback_s pool[8];static bool used[8];static int g_cbprealloc;
static bool inject, busy, fail_alloc, fail_queue;static unsigned unsafe,drops,freed,packets_freed;
static struct net_driver_s *current;
static void (*queued)(void *);static void *queued_arg;
static bool netdev_verify(struct net_driver_s *d){return d!=NULL;}
static void *mempool_zallocate(void *p,int f){(void)p;(void)f;if(fail_alloc)return NULL;for(int i=0;i<8;i++)if(!used[i]){used[i]=true;pool[i]=(struct devif_callback_s){0};return &pool[i];}return NULL;}
static void mempool_release(void *p,void *v){(void)p;struct devif_callback_s *c=v;assert(c>=pool&&c<pool+8&&used[c-pool]);used[c-pool]=false;}
static void netdev_lock(struct net_driver_s *d){assert(!d->d_lock);d->d_lock=1;}
static void netdev_unlock(struct net_driver_s *d){assert(d->d_lock);d->d_lock=0;}
static int nxrmutex_trylock(int *m){if(busy)return -EBUSY;assert(!*m);*m=1;return 0;}
static int nxrmutex_unlock(int *m){assert(*m);*m=0;return 0;}
static bool work_available(struct work_s *w){(void)w;return true;}
static int work_queue(int q,struct work_s *w,void (*f)(void *),void *v,int delay){assert(!q&&w&&!delay);if(fail_queue)return -ENOMEM;assert(!queued);queued=f;queued_arg=v;return 0;}
static void netfwd_dropstats(struct forward_s *f){assert(f->f_iob);drops++;}
static void iob_free_chain(struct iob_s *i){assert(i);packets_freed++;}
static void netfwd_free(struct forward_s *f){(void)f;freed++;}
static uint32_t netfwd_eventhandler(struct net_driver_s *d,void *v,uint32_t f){(void)d;(void)v;return f;}
static void netdev_txnotify_dev(struct net_driver_s *d,uint32_t flags){if(!d->d_lock)unsafe++;d->d_polltype|=flags;d->notifications++;}
static void insert_during_last_removal(void);
"""
# The target TX poller removes its last entry and clears the head. A remote
# input then inserts before the poller finishes reading next/tail. Both list
# insertion/removal bodies come from the pinned SDK; only the interleaving
# hook is inserted.
cbtext=a.callbacks.read_text()
alloc='FAR struct devif_callback_s *\n'+function(cbtext,'devif_callback_alloc')
free=function(cbtext,'devif_callback_free').replace('*list_head = cb->nxtconn;', '*list_head = cb->nxtconn; insert_during_last_removal();')
hooks=r"""
static struct forward_s *pending;
int netfwd_forward(struct net_driver_s *,struct forward_s *);
static void insert_during_last_removal(void){if(!inject)return;inject=false;assert(current&&current->d_lock&&pending);struct net_driver_s input={0};assert(!netfwd_forward(&input,pending));}
#define netfwd_callback_alloc(dev) devif_callback_alloc(dev,&(dev)->d_conncb,&(dev)->d_conncb_tail)
static unsigned owned(struct net_driver_s *d){unsigned n=0;for(struct devif_callback_s *c=d->d_devcb;c;c=c->nxtdev){assert(++n<=8);}return n;}
static unsigned waiting(struct net_driver_s *d){unsigned n=0;struct devif_callback_s *prev=NULL;for(struct devif_callback_s *c=d->d_conncb;c;c=c->nxtconn){assert(++n<=8&&c->prevconn==prev);prev=c;}assert(prev==d->d_conncb_tail);return n;}
static void cleanup(struct net_driver_s *d){while(d->d_conncb)devif_callback_free(d,d->d_conncb,&d->d_conncb,&d->d_conncb_tail);assert(!d->d_devcb);}
static void run_work(void){assert(queued);void (*fn)(void *)=queued;void *arg=queued_arg;queued=NULL;queued_arg=NULL;fn(arg);}
"""
main=r"""
int main(void){
 struct net_driver_s in={0},target={.d_flags=1};struct iob_s packet={0};struct forward_s f={.f_dev=&target,.f_iob=&packet};current=&target;
 netdev_lock(&target);assert(netfwd_callback_alloc(&target));netdev_unlock(&target);
 busy=true;pending=&f;inject=true;netdev_lock(&target);devif_callback_free(&target,target.d_conncb,&target.d_conncb,&target.d_conncb_tail);netdev_unlock(&target);
#ifdef PREVIOUS
 assert(!target.d_conncb&&owned(&target)==1&&target.d_conncb_tail);puts("PREVIOUS_ORPHAN_REPRODUCED: target lists modified without target lock");
#else
 assert(waiting(&target)==0&&owned(&target)==0&&queued_arg==&f&&f.f_cb==NULL);
 // A target poll can remove its last callback while the packet remains owned
 // by queued work. There is no published new node for it to race.
 inject=false;cleanup(&target);assert(!f.f_cb);run_work();assert(waiting(&target)==1&&owned(&target)==1&&f.f_cb&&target.notifications==1&&!unsafe);cleanup(&target);
 // Enqueue failures retain caller ownership, no lists or payload freed.
 fail_queue=true;assert(netfwd_forward(&in,&f)==-ENOMEM&&!freed&&!packets_freed);fail_queue=false;
 // Deferred allocation failure must release the sole packet owner exactly once.
 f.f_cb=NULL;assert(!netfwd_forward(&in,&f));fail_alloc=true;run_work();assert(drops==1&&freed==1&&packets_freed==1&&!owned(&target));fail_alloc=false;
 // A target going down before deferred publication must release its packet.
 f.f_cb=NULL;assert(!netfwd_forward(&in,&f));target.d_flags=0;run_work();assert(drops==2&&freed==2&&packets_freed==2&&!owned(&target));target.d_flags=1;
 // Immediate publication has the same lock and preserves input device state.
 busy=false;uint8_t bytes[1];in.d_iob=&packet;in.d_buf=bytes;in.d_len=17;
 for(int i=0;i<100000;i++){f.f_cb=NULL;assert(!netfwd_forward(&in,&f));assert(waiting(&target)==1&&owned(&target)==1);cleanup(&target);assert(in.d_iob==&packet&&in.d_buf==bytes&&in.d_len==17&&!target.d_lock&&!unsafe);}
 puts("NETFWD_PUBLICATION_OK: deferred packet ownership, locked dual-list publication, error cleanup and 100000 immediate lifecycles");
#endif
 return 0;
}
"""
results={};relative='net/netforward/netfwd_forward.c'
for label,path in [('previous',a.reference/relative),('fixed',prepared/relative)]:
 text=path.read_text();parts=[base,free,alloc,hooks]
 if label=='fixed':parts.append(function(text,'netfwd_publish_locked'))
 parts += [function(text,'netfwd_forward_work'),function(text,'netfwd_forward'),main]
 file=out/(label+'.c');file.write_text('\n'.join(parts));exe=out/label
 cmd=['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Wno-unused-function','-Wno-unused-variable','-fsanitize=address,undefined','-fno-pie','-no-pie',str(file),'-o',str(exe)]
 if label=='previous':cmd.insert(1,'-DPREVIOUS')
 subprocess.run(cmd,check=True);ret=subprocess.run([str(exe)],capture_output=True,text=True,timeout=15);(out/(label+'.log')).write_text(ret.stdout+ret.stderr);assert not ret.returncode,ret.stderr
 print(ret.stdout,end='');results[label]=ret.stdout.strip()
(out/'RESULT.json').write_text(json.dumps({'passed':True,'results':results,'source_sha256':{relative:hashlib.sha256((prepared/relative).read_bytes()).hexdigest(),'devif_callback.c':hashlib.sha256(a.callbacks.read_bytes()).hexdigest()},'scope':'actual pinned callback insertion/removal and forwarding functions; controlled last-node removal interleaving, scheduler/mutex substitutes; ASan/UBSan; no hardware acceptance'},indent=2))
