/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GLASS_ETHERNET_H
#define GLASS_ETHERNET_H
#include <nuttx/config.h>
#include <stdbool.h>
#include <stdint.h>
struct glass_ethernet_s
{
  bool link;
  bool leased;
  bool sharing;
  int error;
  uint32_t address;
  uint32_t mask;
  uint32_t gateway;
  uint32_t dns;
  uint32_t outbound;
  uint32_t inbound;
  uint32_t dropped;
};
#ifdef CONFIG_NET_NAT44
void glass_wired_snapshot(struct glass_ethernet_s *out);
void glass_ethernet_maintain(bool share, struct glass_ethernet_s *out);
void glass_ethernet_share_stop(void);
int glass_ethernet_probe(const char *address, uint16_t port);
int glass_ethernet_forward_guard(bool enable);
int glass_ethernet_dns_probe(void);
void glass_ethernet_print_capacity(void);
#else
#define glass_ethernet_maintain(share,out) ((void)(share),(void)(out))
#define glass_ethernet_share_stop() ((void)0)
#endif
#endif
