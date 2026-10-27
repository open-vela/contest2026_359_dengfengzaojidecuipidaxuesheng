/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <errno.h>
#include <nuttx/net/netdev.h>
#include <nuttx/net/espclaw_share.h>
#include "glass_ethernet.h"
static struct net_driver_s wan,ap,wifi;
static struct glass_ethernet_s wired;
static int locked,enables,disables,clears;
static in_addr_t cleared;
static uint32_t dns_option;
void net_lock(void){assert(!locked);locked=1;}
void net_unlock(void){assert(locked);locked=0;}
void nat_lock(void){assert(!locked);locked=1;}
void nat_unlock(void){assert(locked);locked=0;}
struct net_driver_s *netdev_findbyname(const char *name)
{assert(!locked);return !strcmp(name,"eth0")?&wan:!strcmp(name,"ap0")?&ap:&wifi;}
int nat_enable(struct net_driver_s *d)
{assert(locked);assert(!IFF_IS_NAT(d->d_flags));d->d_flags|=TEST_NAT;enables++;return 0;}
int nat_disable(struct net_driver_s *d)
{assert(locked);assert(IFF_IS_NAT(d->d_flags));d->d_flags&=~TEST_NAT;disables++;return 0;}
void espclaw_share_clear_entries(in_addr_t ip)
{assert(locked);clears++;cleared=ip;}
void glass_wired_snapshot(struct glass_ethernet_s *out){*out=wired;}
void espclaw_share_nat_stats(struct espclaw_share_nat_s *out)
{memset(out,0,sizeof(*out));out->limit=ESPCLAW_SHARE_NAT_LIMIT;}
int dhcpd_set_dnsip(in_addr_t ip){dns_option=ip;return 0;}
static in_addr_t ip(const char *s){struct in_addr a;assert(inet_aton(s,&a));return a.s_addr;}
static struct net_driver_s *route(struct net_driver_s *d,const char *s,const char *t)
{struct net_driver_s *out=espclaw_share_forward(d,ip(s),ip(t));assert(!locked);return out;}
int main(void)
{
  struct espclaw_share_s s;
  struct glass_ethernet_s out;
  wan.d_flags=ap.d_flags=wifi.d_flags=IFF_UP|IFF_RUNNING;
  wan.d_ipaddr=ip("192.168.31.64");wan.d_netmask=ip("255.255.255.0");wan.d_draddr=ip("192.168.31.1");
  wifi.d_ipaddr=ip("192.168.31.33");wifi.d_netmask=wan.d_netmask;wifi.d_draddr=wan.d_draddr;
  ap.d_ipaddr=ip("192.168.4.1");ap.d_netmask=wan.d_netmask;
  assert(!espclaw_share_update(false,&s)&&s.ready&&!s.active);
  assert(!route(&ap,"192.168.4.2","1.1.1.1"));
  wired.link=wired.leased=true;wired.dns=ip("192.168.31.1");
  glass_ethernet_maintain(true,&out);
  assert(out.sharing&&dns_option==0xc0a81f01&&enables==1);
  assert(route(&ap,"192.168.4.2","1.1.1.1")==&wan);
  assert(route(&wan,"1.1.1.1","192.168.4.2")==&ap);
  assert(!route(&wifi,"192.168.4.2","1.1.1.1"));
  assert(!route(&ap,"192.168.31.2","1.1.1.1"));
  assert(!route(&ap,"192.168.4.2","192.168.4.255"));
  assert(!route(&ap,"192.168.4.2","127.0.0.1"));
  assert(!route(&ap,"192.168.4.2","224.0.0.1"));
  assert(!route(&ap,"192.168.4.2","0.1.2.3"));
  assert(!espclaw_share_update(true,&s)&&enables==1);
  wan.d_flags&=~IFF_RUNNING;
  assert(!route(&ap,"192.168.4.2","1.1.1.1"));
  assert(!espclaw_share_update(true,&s)&&!s.active&&!s.link&&disables==1&&clears==1);
  assert(cleared==ip("192.168.31.64"));
  assert(!espclaw_share_update(false,&s)&&disables==1);
  wan.d_flags|=IFF_RUNNING;
  assert(!espclaw_share_update(true,&s)&&s.active&&enables==2);
  wan.d_ipaddr=ip("192.168.31.65");
  assert(!route(&ap,"192.168.4.2","1.1.1.1"));
  assert(!espclaw_share_update(true,&s)&&s.active&&enables==3&&disables==2);
  assert(cleared==ip("192.168.31.64"));
  wan.d_ipaddr=ip("192.168.4.64");wan.d_draddr=ip("192.168.4.254");
  assert(!espclaw_share_update(true,&s)&&!s.active&&s.error==-EADDRINUSE);
  assert(cleared==ip("192.168.31.65"));
  ap.d_ipaddr=ip("192.168.42.1");
  assert(!espclaw_share_update(true,&s)&&s.active);
  assert(route(&ap,"192.168.42.2","1.1.1.1")==&wan);
  glass_ethernet_share_stop();
  wan.d_flags|=TEST_NAT;
  assert(espclaw_share_update(true,&s)==-EBUSY&&!s.active);
  wan.d_flags&=~TEST_NAT;
  wired.leased=false;
  glass_ethernet_maintain(true,&out);assert(!out.sharing);
  wired.leased=true;wired.dns=0;
  glass_ethernet_maintain(true,&out);assert(!out.sharing);
  wired.dns=ip("223.5.5.5");
  glass_ethernet_maintain(true,&out);assert(out.sharing&&dns_option==0xdf050505);
  glass_ethernet_maintain(false,&out);assert(!out.sharing);
  puts("NETWORK_SHARE_OK: eth0 pinning, local-only OFF, lease/DNS gating, link loss, IP replacement, overlap, recovery and ownership");
  return 0;
}
