#!/usr/bin/env python3
"""Reproduce the live DHCP socket/list ABBA with the real TX callbacks."""
from pathlib import Path
import argparse,json,subprocess,hashlib
from render_all_ui import function as production_function

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--previous',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();r=Path(__file__).resolve().parents[1]
source=r/'app/espdl-quickapp/overlay/apps/system/c6probe/c6net.c'
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)

def function(text,name):
 start=text.index('static ',text.index('\nstatic int '+name+'(')) if name=='c6net_txavail' else text.index('static void '+name+'(',text.index('static int c6net_txavail('))
 opening=text.index('{',start);end=opening+1;depth=1
 while depth:
  depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[start:end]

base=r'''
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#define FAR
#define IFF_UP 1
struct net_driver_s {unsigned d_flags;void *d_private;pthread_mutex_t lock;};
struct c6net_state_s {struct net_driver_s dev;bool tx_pending;uint8_t if_type;};
#ifdef DEFERRED
#define ESP_HOSTED_IF_AP 2
struct c6net_stats_s {uint32_t tx_notifications,tx_polls,tx_packets;};
static struct c6net_stats_s g_c6net_stats[2];
static void c6net_tx_notify(void) {}
#endif
static struct c6net_state_s sta,ap;
static pthread_mutex_t list=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t conn_b=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t event=PTHREAD_COND_INITIALIZER;
static bool b_ready,a_poll,a_returned,b_attempt,senders_done,requeue;
static unsigned polls;
static int c6net_txavail(struct net_driver_s *);
static int c6net_txpoll(struct net_driver_s *d){(void)d;return 0;}
static void netdev_lock(struct net_driver_s *d){assert(!pthread_mutex_lock(&d->lock));}
static void netdev_unlock(struct net_driver_s *d){assert(!pthread_mutex_unlock(&d->lock));}
static int devif_poll(struct net_driver_s *d,int (*cb)(struct net_driver_s *))
{
 assert(!pthread_mutex_lock(&list));
 if(d==&sta.dev && !senders_done)
  {
   pthread_mutex_lock(&gate);a_poll=true;pthread_cond_broadcast(&event);
   while(!b_attempt)pthread_cond_wait(&event,&gate);
   pthread_mutex_unlock(&gate);
   /* Match live udp_callback waiting on AP DHCP's socket while list held. */
   pthread_mutex_lock(&conn_b);pthread_mutex_unlock(&conn_b);
  }
 polls++;
 if(requeue){requeue=false;c6net_txavail(d);}
 pthread_mutex_unlock(&list);
 return cb(d);
}
'''
main=r'''
static void *sender_a(void *unused)
{
 (void)unused;pthread_mutex_lock(&gate);
 while(!b_ready)pthread_cond_wait(&event,&gate);
 pthread_mutex_unlock(&gate);
 c6net_txavail(&sta.dev);
 pthread_mutex_lock(&gate);a_returned=true;pthread_cond_broadcast(&event);
 pthread_mutex_unlock(&gate);return NULL;
}
static void *sender_b(void *unused)
{
 (void)unused;pthread_mutex_lock(&conn_b);
 pthread_mutex_lock(&gate);b_ready=true;pthread_cond_broadcast(&event);
 while(!a_poll&&!a_returned)pthread_cond_wait(&event,&gate);
 b_attempt=true;pthread_cond_broadcast(&event);pthread_mutex_unlock(&gate);
 /* Caller retains its socket lock while issuing TX notification. */
 c6net_txavail(&ap.dev);
 pthread_mutex_unlock(&conn_b);return NULL;
}
int main(void)
{
 sta.dev.d_private=&sta;ap.dev.d_private=&ap;
 sta.if_type=1;ap.if_type=2;
 sta.dev.d_flags=ap.dev.d_flags=IFF_UP;
 pthread_mutex_init(&sta.dev.lock,NULL);pthread_mutex_init(&ap.dev.lock,NULL);
 pthread_t a,b;pthread_create(&b,NULL,sender_b,NULL);pthread_create(&a,NULL,sender_a,NULL);
 pthread_join(a,NULL);pthread_join(b,NULL);senders_done=true;
#ifdef DEFERRED
 assert(polls==0&&sta.tx_pending&&ap.tx_pending);
 c6net_poll_pending(&sta);c6net_poll_pending(&ap);assert(polls==2);
 for(unsigned i=0;i<100;i++)c6net_txavail(&sta.dev);
 c6net_poll_pending(&sta);assert(polls==3&&!sta.tx_pending);
 requeue=true;c6net_txavail(&sta.dev);c6net_poll_pending(&sta);
 assert(polls==4&&sta.tx_pending);c6net_poll_pending(&sta);assert(polls==5&&!sta.tx_pending);
 ap.dev.d_flags=0;c6net_txavail(&ap.dev);c6net_poll_pending(&ap);
 assert(polls==5&&!ap.tx_pending);
 assert(g_c6net_stats[0].tx_notifications==103&&g_c6net_stats[0].tx_polls==4);
 assert(g_c6net_stats[1].tx_notifications==2&&g_c6net_stats[1].tx_polls==2);
#endif
 puts("C6_TX_DEFERRED_OK: simultaneous DHCP sends, coalescing, notification during drain, down interface");
 return 0;
}
'''
results={}
for name,path,deferred in [('previous',a.previous,False),('fixed',source,True)]:
 text=path.read_text();body=base
 if deferred:body+='\n'.join(production_function(text,n) for n in ['c6net_stats','c6net_stat_add'])
 body+=function(text,'c6net_txavail')
 if deferred:body+=function(text,'c6net_poll_pending')
 file=out/(name+'.c');file.write_text(body+main)
 exe=out/name
 cmd=['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-pthread','-fsanitize=undefined']
 if deferred:cmd+=['-DDEFERRED']
 subprocess.run(cmd+[str(file),'-o',str(exe)],check=True)
 try:
  result=subprocess.run([str(exe)],timeout=3,capture_output=True,text=True)
  (out/(name+'.log')).write_text(result.stdout+result.stderr)
  assert deferred and result.returncode==0,result.stderr
  results[name]='passed';print(result.stdout,end='')
 except subprocess.TimeoutExpired:
  assert not deferred,'Fixed callback deadlocked'
  results[name]='expected ABBA timeout'
  print('C6_TX_PREVIOUS_DEADLOCK_REPRODUCED')
(out/'RESULT.json').write_text(json.dumps({'passed':True,'results':results,
 'production_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),
 'scope':'real callback bodies; controlled pthread analogue of observed socket/list ownership'},indent=2)+'\n')
