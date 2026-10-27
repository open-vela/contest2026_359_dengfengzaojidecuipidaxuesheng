#!/usr/bin/env python3
"""Exercise pinned DHCP lease allocation with failed offers/private MAC churn."""
from pathlib import Path
import argparse,re,json,subprocess,hashlib
from render_all_ui import function
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--apps',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
r=Path(__file__).resolve().parents[1];builder=(r/'tools/build_bilibili_v3.py').read_text()
count=int(re.search(r"\('NETUTILS_DHCPD_MAXLEASES','(\d+)'\)",builder)[1]);offer=int(re.search(r"\('NETUTILS_DHCPD_OFFERTIME','(\d+)'\)",builder)[1])
assert 4<=count<=16 and 10<=offer<=60
source=a.apps/'netutils/dhcpd/dhcpd.c';text=source.read_text();start=text.index('struct lease_s\n');end=text.index('\n};',start)+3;lease=text[start:end]
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
base=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <netinet/in.h>
#include <stdio.h>
#define FAR
#define HAVE_LEASE_TIME 1
#define DHCP_HLEN_ETHERNET 6
#define ninfo(...) ((void)0)
static time_t now=100;
static time_t dhcpd_time(void){return now;}
'''+lease+r'''
static struct {struct lease_s ds_leases[CONFIG_NETUTILS_DHCPD_MAXLEASES];} g_state;
static struct {in_addr_t ds_startip,ds_endip;} g_dhcpd_config={0xc0a80402,0xc0a80402+CONFIG_NETUTILS_DHCPD_MAXLEASES-1};
'''
core='\n'.join(function(text,n) for n in ['dhcpd_leaseexpired','dhcpd_setlease','dhcpd_findbyipaddr','dhcpd_allocipaddr'])
main=r'''
int main(void){
 uint8_t mac[6]={2,0,0,0,0,1};
 for(unsigned i=0;i<2;i++){
  in_addr_t ip=dhcpd_allocipaddr();assert(ip==g_dhcpd_config.ds_startip+i);
  mac[5]=i+1;assert(dhcpd_setlease(mac,ip,CONFIG_NETUTILS_DHCPD_OFFERTIME));
 }
 in_addr_t next=dhcpd_allocipaddr();
#ifdef PREVIOUS
 assert(!next);now+=31;assert(!dhcpd_allocipaddr());
 puts("PREVIOUS_TWO_OFFERS_EXHAUST_POOL_FOR_HOUR");
#else
 assert(next==g_dhcpd_config.ds_startip+2);
 mac[5]=9;assert(dhcpd_setlease(mac,g_dhcpd_config.ds_startip,120));
 for(unsigned i=3;i<CONFIG_NETUTILS_DHCPD_MAXLEASES;i++)assert(dhcpd_allocipaddr());
 assert(!dhcpd_allocipaddr());
 now+=31;
 assert(dhcpd_allocipaddr()==g_dhcpd_config.ds_startip+1);
 assert(g_state.ds_leases[0].allocated&&g_state.ds_leases[0].mac[5]==9);
 assert(!dhcpd_setlease(mac,g_dhcpd_config.ds_endip+1,120));
 puts("DHCPD_LEASE_BUDGET_OK: pending offers bounded, expired slots reused, active lease preserved, capacity bounded");
#endif
 return 0;
}
'''
results={}
for label,n,seconds in [('previous',2,3600),('fixed',count,offer)]:
 file=out/(label+'.c');file.write_text(base+core+main);exe=out/label
 cmd=['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=undefined',f'-DCONFIG_NETUTILS_DHCPD_MAXLEASES={n}',f'-DCONFIG_NETUTILS_DHCPD_OFFERTIME={seconds}',str(file),'-o',str(exe)]
 if label=='previous':cmd.insert(1,'-DPREVIOUS')
 subprocess.run(cmd,check=True);ret=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10);assert not ret.returncode,ret.stderr
 (out/(label+'.log')).write_text(ret.stdout+ret.stderr);print(ret.stdout,end='');results[label]=ret.stdout.strip()
(out/'RESULT.json').write_text(json.dumps({'passed':True,'results':results,'lease_capacity':count,'offer_hold_seconds':offer,'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'scope':'actual allocator/expiry/setlease bodies; does not claim phone DORA packet capture'},indent=2))
