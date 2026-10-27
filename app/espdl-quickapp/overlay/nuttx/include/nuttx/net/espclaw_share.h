/****************************************************************************
 * include/nuttx/net/espclaw_share.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __INCLUDE_NUTTX_NET_ESPCLAW_SHARE_H
#define __INCLUDE_NUTTX_NET_ESPCLAW_SHARE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/compiler.h>
#include <stdbool.h>
#include <stdint.h>
#include <netinet/in.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct net_driver_s;
#define ESPCLAW_SHARE_NAT_LIMIT 2048

struct espclaw_share_nat_s
{
  uint32_t entries;
  uint32_t peak;
  uint32_t limit;
  uint32_t limit_hits;
  uint32_t alloc_failures;
};

struct espclaw_share_s
{
  bool link;
  bool ready;
  bool active;
  int error;
  in_addr_t address;
  in_addr_t mask;
  in_addr_t gateway;
  uint32_t outbound;
  uint32_t inbound;
  uint32_t dropped;
};

struct espclaw_share_fwd_dev_s
{
  uint32_t polltype;
  uint32_t waiting;
  uint32_t owned;
  uint32_t wakeups;
  uint32_t missing_flags;
  bool available;
  bool truncated;
};

struct espclaw_share_fwd_s
{
  bool enabled;
  struct espclaw_share_fwd_dev_s eth;
  struct espclaw_share_fwd_dev_s ap;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Flat-build service. All addresses use network byte order. State is
 * serialized by the NAT gate; no app pointers are retained by the data path.
 */

int espclaw_share_update(bool enable, FAR struct espclaw_share_s *out);
FAR struct net_driver_s *espclaw_share_forward(
  FAR struct net_driver_s *input, in_addr_t source, in_addr_t destination);
void espclaw_share_clear_entries(in_addr_t address);
void espclaw_share_nat_stats(FAR struct espclaw_share_nat_s *out);
int espclaw_share_fwd_status(FAR struct espclaw_share_fwd_s *out);
int espclaw_share_fwd_enable(bool enable);
/* Single resident network-task owner; no inline transmit or allocation. */
void espclaw_share_fwd_poll(void);

#endif /* __INCLUDE_NUTTX_NET_ESPCLAW_SHARE_H */
