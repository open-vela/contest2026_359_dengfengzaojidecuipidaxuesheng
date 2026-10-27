/* SPDX-License-Identifier: Apache-2.0 */
#include <nuttx/config.h>
#ifdef CONFIG_NET_NAT44
#include <nuttx/net/espclaw_share.h>
#include <netutils/dhcpd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include "glass_ethernet.h"

void glass_ethernet_share_stop(void)
{
  struct espclaw_share_s state;
  (void)espclaw_share_update(false,&state);
}

void glass_ethernet_print_capacity(void)
{
  struct espclaw_share_nat_s state;
  espclaw_share_nat_stats(&state);
  printf("SHARING_NAT entries=%lu limit=%lu peak=%lu limit_hits=%lu alloc_failures=%lu heap=psram\n",
         (unsigned long)state.entries, (unsigned long)state.limit,
         (unsigned long)state.peak, (unsigned long)state.limit_hits,
         (unsigned long)state.alloc_failures);
  struct espclaw_share_fwd_s fwd;
  if (espclaw_share_fwd_status(&fwd) == 0)
    {
      printf("SHARING_FWD enabled=%d eth_valid=%d eth_poll=%08lx eth_wait=%lu eth_owned=%lu eth_wakes=%lu eth_missing=%lu ap_valid=%d ap_poll=%08lx ap_wait=%lu ap_owned=%lu ap_wakes=%lu ap_missing=%lu truncated=%d\n",
             fwd.enabled,fwd.eth.available,(unsigned long)fwd.eth.polltype,
             (unsigned long)fwd.eth.waiting,(unsigned long)fwd.eth.owned,
             (unsigned long)fwd.eth.wakeups,(unsigned long)fwd.eth.missing_flags,
             fwd.ap.available,(unsigned long)fwd.ap.polltype,
             (unsigned long)fwd.ap.waiting,(unsigned long)fwd.ap.owned,
             (unsigned long)fwd.ap.wakeups,(unsigned long)fwd.ap.missing_flags,
             fwd.eth.truncated || fwd.ap.truncated);
    }
}

int glass_ethernet_forward_guard(bool enable)
{
  int ret=espclaw_share_fwd_enable(enable);
  printf("FWDGUARD result=%d enabled=%d\n",ret,enable);
  return ret;
}

void glass_ethernet_maintain(bool share,struct glass_ethernet_s *out)
{
  struct espclaw_share_s state={0};
  glass_wired_snapshot(out);
  /* The existing wired worker exclusively owns DHCP, renewal and DNS.
   * Relay its DNS answer through NAT directly, supporting UDP and TCP DNS.
   * Atomic DHCPD fields allow updates without resetting client leases.
   */
  if(out->leased && out->dns) (void)dhcpd_set_dnsip(ntohl(out->dns));
  int ret=espclaw_share_update(share && out->leased && out->dns,&state);
  out->link=state.link;
  out->sharing=state.active;
  out->outbound=state.outbound;out->inbound=state.inbound;out->dropped=state.dropped;
  if(ret || (share && out->leased && state.error)) out->error=ret?ret:state.error;
}

static int ethernet_socket(int type,in_addr_t remote,uint16_t port)
{
  struct glass_ethernet_s state;
  glass_wired_snapshot(&state);
  if(!state.leased)return -ENETDOWN;
  int fd=socket(AF_INET,type,0);
  if(fd<0)return -errno;
  struct sockaddr_in local={.sin_family=AF_INET,.sin_addr.s_addr=state.address};
  struct sockaddr_in peer={.sin_family=AF_INET,.sin_port=htons(port),.sin_addr.s_addr=remote};
  int ret=0;
  if(fcntl(fd,F_SETFD,FD_CLOEXEC)<0 ||
     setsockopt(fd,SOL_SOCKET,SO_BINDTODEVICE,"eth0",5)<0 ||
     bind(fd,(struct sockaddr *)&local,sizeof(local))<0 ||
     fcntl(fd,F_SETFL,O_NONBLOCK)<0)
    {ret=-errno;goto failed;}
  if(connect(fd,(struct sockaddr *)&peer,sizeof(peer))<0 && errno!=EINPROGRESS)
    {ret=-errno;goto failed;}
  struct pollfd wait={.fd=fd,.events=POLLOUT};
  int n=poll(&wait,1,5000);
  if(n<=0){ret=n<0?-errno:-ETIMEDOUT;goto failed;}
  int error=0;socklen_t size=sizeof(error);
  if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&size)<0)
    {ret=-errno;goto failed;}
  if(error){ret=-error;goto failed;}
  if((wait.revents&(POLLERR|POLLHUP|POLLNVAL)) || !(wait.revents&POLLOUT))
    {ret=-EIO;goto failed;}
  return fd;
failed:
  close(fd);return ret;
}

int glass_ethernet_probe(const char *address,uint16_t port)
{
  struct in_addr remote;
  if(!port || inet_pton(AF_INET,address,&remote)!=1)return -EINVAL;
  int fd=ethernet_socket(SOCK_STREAM,remote.s_addr,port);
  int ret=fd<0?fd:0;
  if(fd>=0)close(fd);
  printf("ETHERNET_TCP interface=eth0 host=%s port=%u result=%d\n",address,port,ret);
  return ret;
}

int glass_ethernet_dns_probe(void)
{
  struct glass_ethernet_s state;glass_wired_snapshot(&state);
  const uint8_t query[]={0x63,0xd1,1,0,0,1,0,0,0,0,0,0,
    7,'e','x','a','m','p','l','e',3,'c','o','m',0,0,1,0,1};
  uint8_t reply[512];
  int fd=state.dns?ethernet_socket(SOCK_DGRAM,state.dns,53):-ENETDOWN;
  int ret=fd<0?fd:0;ssize_t bytes=0;
  if(fd>=0)
    {
      if(send(fd,query,sizeof(query),0)!=(ssize_t)sizeof(query))ret=-(errno?errno:EIO);
      struct pollfd wait={.fd=fd,.events=POLLIN};
      if(!ret)
        {
          int n=poll(&wait,1,3000);
          if(n<=0)ret=n<0?-errno:-ETIMEDOUT;
        }
      if(!ret)
        {
          bytes=recv(fd,reply,sizeof(reply),0);
          if(bytes<12 || reply[0]!=query[0] || reply[1]!=query[1] ||
             !(reply[2]&0x80) || (reply[3]&15) || !(reply[6]|reply[7]))ret=-EPROTO;
        }
      close(fd);
    }
  printf("ETHERNET_DNS interface=eth0 result=%d bytes=%ld\n",ret,(long)bytes);
  return ret;
}
#endif
