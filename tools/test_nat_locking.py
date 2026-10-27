#!/usr/bin/env python3
"""Compile actual SDK NAT locking/selection functions with controlled ports."""
from pathlib import Path
import argparse,subprocess,shutil,json,hashlib
from prepare_network_share import apply
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--reference',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
prepared=out/'prepared';shutil.copytree(a.reference,prepared,dirs_exist_ok=True);apply(prepared)
def public(text,typ,name):
 text=text[text.index(typ+' '+name+'('):];end=text.index('{')+1;depth=1
 while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[:end]
prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#define FAR
#define OK 0
#define CONFIG_NET_IPv4 1
#define CONFIG_NET_TCP 1
#define CONFIG_NET_UDP 1
#define CONFIG_NET_NAT44 1
#define IP_PROTO_TCP 6
#define IP_PROTO_UDP 17
#define HTONS(a) htons(a)
#define IFF_IS_NAT(f) ((f)&1)
#define nwarn(...) ((void)0)
typedef struct {unsigned depth;} mutex_t;
static mutex_t g_nat_lock;
static unsigned maximum;
static int nxmutex_lock(mutex_t *m){assert(!m->depth&&"non-recursive NAT lock re-entry");m->depth++;return 0;}
static int nxmutex_unlock(mutex_t *m){assert(m->depth);m->depth--;return 0;}
static int nxrmutex_lock(mutex_t *m){m->depth++;if(m->depth>maximum)maximum=m->depth;return 0;}
static int nxrmutex_unlock(mutex_t *m){assert(m->depth);m->depth--;return 0;}
union ip_addr_u {in_addr_t ipv4;};
union ip_binding_u {struct {in_addr_t laddr,raddr;} ipv4;};
struct net_driver_s {unsigned d_flags;in_addr_t d_ipaddr;};
struct ipv4_hdr_s {uint16_t srcipaddr[2],destipaddr[2];};
typedef int ipv4_nat_entry_t;
enum nat_manip_type_e {NAT_MANIP_SRC,NAT_MANIP_DST};
static ipv4_nat_entry_t entry;
static bool permit_entry;
void nat_lock(void);void nat_unlock(void);
bool nat_port_inuse(uint8_t,uint8_t,const union ip_addr_u *,uint16_t);
static ipv4_nat_entry_t *ipv4_nat_inbound_entry_find(uint8_t p,in_addr_t ip,uint16_t port,in_addr_t peer,uint16_t peerport,bool refresh)
{(void)p;(void)ip;(void)port;(void)peer;(void)peerport;(void)refresh;assert(g_nat_lock.depth);return NULL;}
static int tcp_selectport(uint8_t d,const union ip_addr_u *ip,uint16_t port,bool ignored)
{(void)port;(void)ignored;assert(!nat_port_inuse(d,6,ip,htons(45678)));return htons(45678);}
static int udp_select_port(uint8_t d,const union ip_binding_u *binding)
{union ip_addr_u ip={.ipv4=binding->ipv4.laddr};assert(!nat_port_inuse(d,17,&ip,htons(45679)));return 45679;}
static bool net_ipv4addr_hdrcmp(const uint16_t *a,const in_addr_t *b){return !memcmp(a,b,4);}
static ipv4_nat_entry_t *ipv4_nat_outbound_internal(struct net_driver_s *d,struct ipv4_hdr_s *h,enum nat_manip_type_e type)
{(void)d;(void)h;(void)type;return permit_entry?&entry:NULL;}
'''
main=r'''
int main(int argc,char **argv){
 assert(argc==2);struct net_driver_s dev={.d_flags=1,.d_ipaddr=htonl(0xc0000201)};
 if(!strcmp(argv[1],"port")){
  union ip_addr_u ip={.ipv4=dev.d_ipaddr};
  nat_lock();assert(nat_port_select(&dev,PF_INET,6,&ip,htons(12345)));nat_unlock();
  nat_lock();assert(nat_port_select(&dev,PF_INET,17,&ip,htons(12345)));nat_unlock();
  assert(!g_nat_lock.depth&&maximum==2);
 }else{
  struct ipv4_hdr_s h;memset(&h,0,sizeof(h));
  permit_entry=false;assert(ipv4_nat_outbound(&dev,&h,NAT_MANIP_SRC)==-ENOENT);
  assert(!g_nat_lock.depth&&"outbound failure retained NAT lock");
  permit_entry=true;assert(!ipv4_nat_outbound(&dev,&h,NAT_MANIP_SRC));assert(!g_nat_lock.depth);
 }
 puts("NATIVE_NAT_LOCK_OK");return 0;
}
'''
results={}
for label,root in [('original',a.reference),('patched',prepared)]:
 nat=(root/'net/nat/nat.c').read_text();ipv4=(root/'net/nat/ipv4_nat.c').read_text()
 body=prefix+'\n'+'\n'.join(public(nat,t,n) for t,n in [('void','nat_lock'),('void','nat_unlock'),('bool','nat_port_inuse'),('uint16_t','nat_port_select')])
 body+='\n'+public(ipv4,'int','ipv4_nat_outbound')+'\n'+main
 source=out/(label+'.c');source.write_text(body);binary=out/label
 subprocess.run(['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-parameter',
  '-fsanitize=address,undefined','-fno-pie','-no-pie',str(source),'-o',str(binary)],check=True)
 for case in ('port','error'):
  r=subprocess.run([str(binary),case],capture_output=True,text=True,timeout=10)
  (out/(label+'-'+case+'.log')).write_text(r.stdout+r.stderr)
  assert (r.returncode==0)==(label=='patched'),(label,case,r.stdout,r.stderr)
  results[label+'-'+case]=r.returncode
(out/'RESULT.json').write_text(json.dumps({'passed':True,'functions_from_actual_sdk':True,
 'negative_controls_fail':True,'recursive_tcp_udp_port_selection_verified':True,
 'outbound_error_unlock_verified':True,'returncodes':results,
 'prepared_source_hashes':{n:hashlib.sha256((prepared/n).read_bytes()).hexdigest() for n in ['net/nat/nat.c','net/nat/ipv4_nat.c']}},indent=2)+'\n')
print('NATIVE_NAT_LOCKING_OK: original port-selection/error controls both fail; patched TCP/UDP recursion and failure recovery pass')
