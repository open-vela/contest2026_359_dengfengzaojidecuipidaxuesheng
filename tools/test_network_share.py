#!/usr/bin/env python3
"""Exercise the production sharing controller and pinned-SDK transformation."""
from pathlib import Path
import argparse,subprocess,json,hashlib,shutil,os
from prepare_network_share import apply
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--reference',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
a=p.parse_args();repo=Path(__file__).resolve().parents[1];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
overlay=repo/'app/espdl-quickapp/overlay';desktop=overlay/'apps/system/desktop'
include=out/'include'
headers={
 'nuttx/config.h':'#define CONFIG_NET_NAT44 1\n',
 'nuttx/compiler.h':'#pragma once\n#define FAR\n',
 'nuttx/net/net.h':'void net_lock(void); void net_unlock(void);\n',
 'nuttx/net/netdev.h':'''#pragma once
#include <stdint.h>
#include <netinet/in.h>
#include <net/if.h>
#define TEST_NAT 0x10000u
#define IFF_IS_UP(f) ((f)&IFF_UP)
#define IFF_IS_RUNNING(f) ((f)&IFF_RUNNING)
#define IFF_IS_NAT(f) ((f)&TEST_NAT)
struct net_driver_s {unsigned d_flags;in_addr_t d_ipaddr,d_netmask,d_draddr;};
struct net_driver_s *netdev_findbyname(const char *);
''',
 'nat/nat.h':'struct net_driver_s;void nat_lock(void);void nat_unlock(void);int nat_enable(struct net_driver_s *);int nat_disable(struct net_driver_s *);\n',
 'netutils/dhcpd.h':'#include <netinet/in.h>\nint dhcpd_set_dnsip(in_addr_t);\n'}
for name,body in headers.items():
 f=include/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_text(body)
f=include/'nuttx/net/espclaw_share.h';f.write_bytes((overlay/'nuttx/include/nuttx/net/espclaw_share.h').read_bytes())
sources=[overlay/'nuttx/net/espclaw_share.c',desktop/'glass_ethernet.c',repo/'app/espdl-quickapp/tests/network_share_host.c']
command=['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
 '-fno-omit-frame-pointer','-fno-pie','-no-pie','-I'+str(include),'-I'+str(desktop),
 *map(str,sources),'-o',str(out/'share-host')]
for label,cmd in [('compile',command),('run',[str(out/'share-host')])]:
 r=subprocess.run(cmd,capture_output=True,text=True,timeout=60)
 (out/(label+'.log')).write_text(r.stdout+r.stderr)
 if r.returncode:raise RuntimeError(r.stdout+r.stderr)
 print(r.stdout,end='')
prepared=out/'prepared';shutil.copytree(a.reference,prepared,dirs_exist_ok=True)
first=apply(prepared);assert apply(prepared)==first
def function(text,name):
 start=text.index(name+'(');start=text.rfind('\nstatic ',0,start)
 if start<0:start=text.index('int '+name+'(')
 else:start+=1
 opening=text.index('{',start);end=opening+1;depth=1
 while depth:
  depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[start:end]
entry=(prepared/'net/nat/ipv4_nat_entry.c').read_text()
native=(prepared/'net/nat/nat.c').read_text()
body=r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <netinet/in.h>
#define FAR
#define OK 0
#define CONFIG_NET_NAT44 1
#include <nuttx/net/espclaw_share.h>
#define nwarn(...) ((void)0)
#define ninfo(...) ((void)0)
#define IFF_IS_NAT(f) ((f)&1)
#define IFF_SET_NAT(f) ((f)|=1)
#define IFF_CLR_NAT(f) ((f)&=~1u)
#define hashtable_add(...) ((void)0)
#define hashtable_delete(...) ((void)0)
typedef struct {uint8_t protocol;uint32_t external_ip,local_ip;uint16_t external_port,local_port;} ipv4_nat_entry_t;
struct net_driver_s {unsigned d_flags;};
static unsigned g_share_entries,g_share_peak,allocations;
static uint32_t g_share_limit_hits,g_share_alloc_failures;
static bool fail_alloc,held;
static void *kumm_malloc(size_t size){if(fail_alloc)return NULL;allocations++;return malloc(size);}
static void kumm_free(void *p){assert(allocations);allocations--;free(p);}
static void ipv4_nat_entry_refresh(ipv4_nat_entry_t *p){(void)p;}
static void nat_lock(void){assert(!held);held=true;}
static void nat_unlock(void){assert(held);held=false;}
static void ipv4_nat_entry_clear(struct net_driver_s *d){(void)d;assert(!g_share_entries);}
'''
body+='\n'+function(entry,'ipv4_nat_entry_create')+'\n'+function(entry,'ipv4_nat_entry_delete')
# Public functions have no static prefix: extract from their exact signature.
def public(text,name):
 text=text[text.index('int '+name+'('):];end=text.index('{')+1;depth=1
 while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[:end]
body+='\n'+public(native,'nat_enable')+'\n'+public(native,'nat_disable')
body+=r'''
int main(void){
 ipv4_nat_entry_t *entries[ESPCLAW_SHARE_NAT_LIMIT];
 for(unsigned i=0;i<ESPCLAW_SHARE_NAT_LIMIT;i++){entries[i]=ipv4_nat_entry_create(17,1,2,3,4,5,6);assert(entries[i]);}
 assert(g_share_entries==ESPCLAW_SHARE_NAT_LIMIT&&allocations==ESPCLAW_SHARE_NAT_LIMIT);
 assert(g_share_peak==ESPCLAW_SHARE_NAT_LIMIT&&!g_share_limit_hits&&!g_share_alloc_failures);
 assert(!ipv4_nat_entry_create(17,1,2,3,4,5,6));
 assert(g_share_limit_hits==1&&!g_share_alloc_failures);
 /* A released slot is reusable, without evicting any active mapping. */
 ipv4_nat_entry_delete(entries[4]);entries[4]=ipv4_nat_entry_create(17,1,2,3,4,5,6);assert(entries[4]);
 for(unsigned i=0;i<ESPCLAW_SHARE_NAT_LIMIT;i++)ipv4_nat_entry_delete(entries[i]);
 assert(!g_share_entries&&!allocations);
 fail_alloc=true;assert(!ipv4_nat_entry_create(17,1,2,3,4,5,6));assert(!g_share_entries);
 assert(g_share_alloc_failures==1&&g_share_limit_hits==1);
 fail_alloc=false;entries[0]=ipv4_nat_entry_create(17,1,2,3,4,5,6);assert(entries[0]);ipv4_nat_entry_delete(entries[0]);
 struct net_driver_s dev={0};assert(!nat_enable(&dev)&&!held);
 assert(nat_enable(&dev)==-EEXIST&&!held);assert(!nat_disable(&dev)&&!held);
 assert(nat_disable(&dev)==-ENODEV&&!held);return 0;
}
'''
(out/'native-nat-functions.c').write_text(body)
cmd=['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
 '-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(include),str(out/'native-nat-functions.c'),'-o',str(out/'native-nat-functions')]
subprocess.run(cmd,check=True);subprocess.run([str(out/'native-nat-functions')],check=True,timeout=10)
print('NATIVE_NAT_FUNCTIONS_OK: actual prepared function bodies and production capacity header; PSRAM cap, slot reuse, allocation failure, complete release, repeated-enable lock balance')
with (prepared/'net/nat/nat.c').open('a') as f:f.write('\n')
try:apply(prepared)
except ValueError:pass
else:raise AssertionError('Changed source was accepted')
(out/'RESULT.json').write_text(json.dumps({'passed':True,'scope':'host only; no actual forwarding claimed',
 'sources':{str(s.relative_to(repo)):hashlib.sha256(s.read_bytes()).hexdigest() for s in sources},
 'prepared':first},indent=2)+'\n')
print('NETWORK_SHARE_PREPARE_OK: repeatable transformation, unknown source rejected')
