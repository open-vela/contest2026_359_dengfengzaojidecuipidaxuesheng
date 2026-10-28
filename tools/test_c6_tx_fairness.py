#!/usr/bin/env python3
"""Run the production C6 flat-buffer callbacks against an endless producer."""
from pathlib import Path
import argparse,subprocess
from render_all_ui import function
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args()
r=Path(__file__).resolve().parents[1];c=r/'app/espdl-quickapp/overlay/apps/system/c6probe'
s=(c/'c6net.c').read_text();header=(c/'c6net.h').read_text()
stats=header[header.index('struct c6net_stats_s'):header.index('};',header.index('struct c6net_stats_s'))+2]
a.output.mkdir(exist_ok=True,parents=True)
base=r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#define FAR
#define IFF_UP 1
#define ESP_HOSTED_IF_AP 2
#define C6NET_TX_BUDGET 8
struct net_driver_s {unsigned d_flags,d_len;void *d_private;uint8_t *d_buf;};
struct c6net_state_s {struct net_driver_s dev;bool tx_pending;unsigned tx_budget;uint8_t if_type;};
'''
base+=stats+r'''
static struct c6net_stats_s g_c6net_stats[2];
static unsigned remaining,packets_in_lock,max_packets_in_lock,locks,notifies;
static bool locked;
static void c6net_tx_notify(void){notifies++;}
static int c6_softap_preserve_mode(int mode){(void)mode;return 3;}
static void netdev_lock(struct net_driver_s *d){(void)d;assert(!locked);locked=true;locks++;packets_in_lock=0;}
static void netdev_unlock(struct net_driver_s *d){(void)d;assert(locked);locked=false;if(packets_in_lock>max_packets_in_lock)max_packets_in_lock=packets_in_lock;}
static int esp_hosted_send(unsigned type,unsigned n,const uint8_t *data,unsigned len)
{(void)type;(void)n;(void)data;assert(locked&&len==80);packets_in_lock++;return 0;}
static int devif_poll(struct net_driver_s *d,int (*callback)(struct net_driver_s *))
{
 while(remaining){remaining--;d->d_len=80;if(callback(d))return 1;}
 return 0;
}
'''
for name in ['c6net_stats','c6net_stat_add','c6net_stat_max','c6net_now_us','c6net_txpoll','c6net_poll_pending']:
 base+=function(s,name)+'\n'
base+=r'''
int main(void)
{
 struct c6net_state_s p={.if_type=1,.tx_pending=true};p.dev.d_private=&p;p.dev.d_flags=IFF_UP;
 remaining=250;unsigned passes=0;
 while(p.tx_pending){assert(passes++<40);c6net_poll_pending(&p);assert(!locked&&!g_c6net_stats[0].tx_active);}
 assert(!remaining&&passes==32&&locks==32&&max_packets_in_lock==8);
 assert(g_c6net_stats[0].tx_packets==250&&g_c6net_stats[0].tx_yields==31&&notifies==31);
 p.dev.d_flags=0;p.tx_pending=true;remaining=100;c6net_poll_pending(&p);
 assert(remaining==100&&!p.tx_pending&&!locked&&p.tx_budget==0);
 puts("C6_TX_FAIRNESS_OK: real callbacks; 250 packets; <=8 packets per network-lock hold; 32 passes; continuation retained; down interface");
}
'''
src=a.output/'check.c';src.write_text(base);exe=a.output/'check'
subprocess.run(['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(src),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True,timeout=5)
