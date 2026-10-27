#!/usr/bin/env python3
"""Reproduce NAT/TCP table ABBA using real selectors and two host threads."""
from pathlib import Path
import argparse,subprocess,json,shutil,hashlib
from prepare_network_share import apply
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--reference',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
prepared=out/'prepared';shutil.copytree(a.reference,prepared,dirs_exist_ok=True);apply(prepared)
def function(text,typ,name):
 text=text[text.index(typ+' '+name+'('):];end=text.index('{')+1;depth=1
 while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[:end]
prefix=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <stdatomic.h>
#define FAR
#define CONFIG_NET_IPv4 1
#define CONFIG_NET_TCP 1
#define CONFIG_NET_NAT 1
#define CONFIG_NET_NAT44 1
#define IP_PROTO_TCP 6
#define HTONS(a) htons(a)
#define DEFINE_PER_CPU_BMP(type,name) type name
#define this_cpu_var_bmp(name) name
#define NET_PORT_RANDOM_INIT(last) ((last)=40000)
#define NET_PORT_NEXT_NH(port,last) do{(last)++;(port)=htons(last);}while(0)
union ip_addr_u {in_addr_t ipv4;};
struct net_driver_s {unsigned flags;};
typedef int ipv4_nat_entry_t;
static pthread_mutex_t gate,table;
static pthread_t main_thread;
static _Atomic bool other_has_table;
static int g_nat_lock;
static _Thread_local unsigned gate_depth;
static int nxrmutex_lock(void *unused){(void)unused;int r=pthread_mutex_lock(&gate);assert(!r);gate_depth++;return r;}
static int nxrmutex_unlock(void *unused){(void)unused;assert(gate_depth);gate_depth--;return pthread_mutex_unlock(&gate);}
static void tcp_conn_list_lock(void){assert(!pthread_mutex_lock(&table));if(!pthread_equal(pthread_self(),main_thread))other_has_table=true;}
static void tcp_conn_list_unlock(void){assert(!pthread_mutex_unlock(&table));}
static void *tcp_listener(uint8_t d,const union ip_addr_u *ip,uint16_t port,bool reuse)
{(void)d;(void)ip;(void)port;(void)reuse;return NULL;}
static ipv4_nat_entry_t *ipv4_nat_inbound_entry_find(uint8_t p,in_addr_t ip,uint16_t port,in_addr_t peer,uint16_t peerport,bool refresh)
{(void)p;(void)ip;(void)port;(void)peer;(void)peerport;(void)refresh;assert(gate_depth);return NULL;}
void nat_lock(void);void nat_unlock(void);
bool nat_port_inuse(uint8_t,uint8_t,const union ip_addr_u *,uint16_t);
int tcp_selectport(uint8_t,const union ip_addr_u *,uint16_t,bool);
'''
main=r'''
static void *bind_thread(void *unused){
 (void)unused;union ip_addr_u ip={.ipv4=htonl(0xc0000201)};
 assert(tcp_selectport(PF_INET,&ip,htons(45678),false)>0);assert(!gate_depth);return NULL;
}
int main(void){
 pthread_mutexattr_t attr;assert(!pthread_mutexattr_init(&attr));
 assert(!pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE));
 assert(!pthread_mutex_init(&gate,&attr));assert(!pthread_mutex_init(&table,&attr));
 pthread_mutexattr_destroy(&attr);main_thread=pthread_self();alarm(2);
 nat_lock();pthread_t binder;assert(!pthread_create(&binder,NULL,bind_thread,NULL));
 if(EXPECT_GATE){usleep(50000);assert(!atomic_load(&other_has_table));}
 else {for(unsigned i=0;i<500&&!atomic_load(&other_has_table);i++)usleep(1000);assert(atomic_load(&other_has_table));}
 struct net_driver_s dev={0};union ip_addr_u ip={.ipv4=htonl(0xc0000201)};
 assert(nat_port_select(&dev,PF_INET,6,&ip,htons(45679))>0);
 nat_unlock();assert(!gate_depth);assert(!pthread_join(binder,NULL));alarm(0);
 puts("NATIVE_NAT_PORT_ORDER_OK");return 0;
}
'''
nat=(prepared/'net/nat/nat.c').read_text();results={}
for label,root in [('recursive-only',a.reference),('ordered',prepared)]:
 tcp=(root/'net/tcp/tcp_conn.c').read_text()
 body=prefix+'\n'+'\n'.join(function(nat,t,n) for t,n in [('void','nat_lock'),('void','nat_unlock'),('bool','nat_port_inuse')])
 if label=='ordered':body+='\n'+function(tcp,'int','tcp_selectport_internal')
 body+='\n'+function(tcp,'int','tcp_selectport')+'\n'+function(nat,'uint16_t','nat_port_select')+'\n'+main
 src=out/(label+'.c');src.write_text(body);binary=out/label
 subprocess.run(['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
  '-pthread','-fsanitize=address,undefined','-fno-pie','-no-pie','-DEXPECT_GATE='+str(int(label=='ordered')),str(src),'-o',str(binary)],check=True)
 r=subprocess.run([str(binary)],capture_output=True,text=True,timeout=5)
 (out/(label+'.log')).write_text(r.stdout+r.stderr)
 if label=='recursive-only':assert r.returncode==-14,(r.returncode,r.stdout,r.stderr)
 else:assert r.returncode==0,(r.returncode,r.stdout,r.stderr)
 results[label]=r.returncode
(out/'RESULT.json').write_text(json.dumps({'passed':True,'scope':'actual native TCP/NAT selectors, controlled connection tables and two threads',
 'recursive_only_negative_control_times_out':True,'nat_before_tcp_table_verified':True,
 'returncodes':results,'source_sha256':{n:hashlib.sha256((prepared/n).read_bytes()).hexdigest() for n in ['net/nat/nat.c','net/tcp/tcp_conn.c','net/udp/udp_conn.c']}},indent=2)+'\n')
print('NATIVE_NAT_ABBA_OK: recursive-only native selectors deadlock; NAT-before-table native selectors complete')
