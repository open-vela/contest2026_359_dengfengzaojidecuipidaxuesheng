/****************************************************************************
 * net/espclaw_share.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <string.h>
#include <net/if.h>

#include <nuttx/net/net.h>
#include <nuttx/net/netdev.h>
#include <nuttx/net/espclaw_share.h>

#include "nat/nat.h"
#ifdef CONFIG_NET_IPFORWARD
#  include <time.h>
#  include <nuttx/mutex.h>
#  include "devif/devif.h"
#  include "netdev/netdev.h"
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct espclaw_share_s g_share;
static in_addr_t g_external;

#ifdef CONFIG_NET_IPFORWARD
static bool g_fwd_enabled = true;
static uint32_t g_fwd_wakeups[2];
static uint32_t g_fwd_missing[2];
static uint64_t g_fwd_last_ms;

static void share_fwd_read(FAR struct net_driver_s *dev, unsigned int index,
                           FAR struct espclaw_share_fwd_dev_s *out)
{
  FAR struct devif_callback_s *cb;
  unsigned int budget;

  out->polltype = dev->d_polltype;
  for (cb = dev->d_conncb, budget = 64; cb && budget;
       cb = cb->nxtconn, budget--)
    {
      if ((cb->flags & IPFWD_POLL) != 0) out->waiting++;
    }

  out->truncated = cb != NULL;
  for (cb = dev->d_devcb, budget = 64; cb && budget;
       cb = cb->nxtdev, budget--)
    {
      if ((cb->flags & IPFWD_POLL) != 0) out->owned++;
    }

  out->truncated |= cb != NULL;
  out->wakeups = __atomic_load_n(&g_fwd_wakeups[index], __ATOMIC_RELAXED);
  out->missing_flags = __atomic_load_n(&g_fwd_missing[index],
                                      __ATOMIC_RELAXED);
  out->available = true;
}

int espclaw_share_fwd_enable(bool enable)
{
  __atomic_store_n(&g_fwd_enabled, enable, __ATOMIC_RELEASE);
  return 0;
}

int espclaw_share_fwd_status(FAR struct espclaw_share_fwd_s *out)
{
  FAR struct net_driver_s *dev;
  FAR struct espclaw_share_fwd_dev_s *row;
  unsigned int i;

  if (!out) return -EINVAL;
  memset(out, 0, sizeof(*out));
  out->enabled = __atomic_load_n(&g_fwd_enabled, __ATOMIC_ACQUIRE);
  for (i = 0; i < 2; i++)
    {
      dev = netdev_findbyname(i == 0 ? "eth0" : "ap0");
      if (!dev || nxrmutex_trylock(&dev->d_lock) < 0) continue;
      row = i == 0 ? &out->eth : &out->ap;
      share_fwd_read(dev, i, row);
      netdev_unlock(dev);
    }

  return 0;
}

void espclaw_share_fwd_poll(void)
{
  FAR struct net_driver_s *dev;
  struct espclaw_share_fwd_dev_s row;
  struct timespec ts;
  uint64_t now;
  unsigned int i;
  bool active;

  if (!__atomic_load_n(&g_fwd_enabled, __ATOMIC_ACQUIRE)) return;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) return;
  now = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
  if (now - g_fwd_last_ms < 10) return;
  g_fwd_last_ms = now;
  nat_lock();
  active = g_share.active;
  nat_unlock();
  if (!active) return;

  for (i = 0; i < 2; i++)
    {
      dev = netdev_findbyname(i == 0 ? "eth0" : "ap0");
      if (!dev || nxrmutex_trylock(&dev->d_lock) < 0) continue;
      memset(&row, 0, sizeof(row));
      share_fwd_read(dev, i, &row);
      if (IFF_IS_UP(dev->d_flags) && !row.truncated && row.waiting)
        {
          if ((dev->d_polltype & IPFWD_POLL) == 0)
            __atomic_fetch_add(&g_fwd_missing[i], 1, __ATOMIC_RELAXED);
          __atomic_fetch_add(&g_fwd_wakeups[i], 1, __ATOMIC_RELAXED);
          netdev_txnotify_dev(dev, IPFWD_POLL);
        }

      netdev_unlock(dev);
    }
}
#else
int espclaw_share_fwd_enable(bool enable)
{
  (void)enable;
  return -ENOSYS;
}

int espclaw_share_fwd_status(FAR struct espclaw_share_fwd_s *out)
{
  if (!out) return -EINVAL;
  memset(out, 0, sizeof(*out));
  return -ENOSYS;
}

void espclaw_share_fwd_poll(void)
{
}
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool share_up(FAR struct net_driver_s *dev)
{
  return dev != NULL && IFF_IS_UP(dev->d_flags) &&
         IFF_IS_RUNNING(dev->d_flags);
}

static bool share_host(in_addr_t ip, in_addr_t base, in_addr_t mask)
{
  uint32_t host = ntohl(ip & ~mask);
  return ip != 0 && (ip & mask) == (base & mask) && host != 0 &&
         host != ntohl(~mask);
}

static bool share_valid(FAR struct net_driver_s *wan,
                        FAR struct net_driver_s *ap)
{
  return share_up(wan) && share_up(ap) && wan->d_ipaddr != 0 &&
         wan->d_netmask != 0 && wan->d_draddr != 0 &&
         ap->d_ipaddr != 0 && ap->d_netmask != 0 &&
         share_host(wan->d_draddr, wan->d_ipaddr, wan->d_netmask) &&
         (wan->d_ipaddr & wan->d_netmask) !=
         (ap->d_ipaddr & wan->d_netmask) &&
         (wan->d_ipaddr & ap->d_netmask) !=
         (ap->d_ipaddr & ap->d_netmask);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int espclaw_share_update(bool enable, FAR struct espclaw_share_s *out)
{
  FAR struct net_driver_s *wan;
  FAR struct net_driver_s *ap;
  bool valid;
  int ret = 0;

  if (out == NULL)
    {
      return -EINVAL;
    }

  wan = netdev_findbyname("eth0");
  ap = netdev_findbyname("ap0");
  nat_lock();
  g_share.link = share_up(wan);
  g_share.address = wan != NULL ? wan->d_ipaddr : 0;
  g_share.mask = wan != NULL ? wan->d_netmask : 0;
  g_share.gateway = wan != NULL ? wan->d_draddr : 0;
  g_share.ready = g_share.link && g_share.address != 0 &&
                  g_share.mask != 0 &&
                  share_host(g_share.gateway, g_share.address, g_share.mask);
  valid = enable && share_valid(wan, ap);
  if (g_share.active && (!valid || g_external != g_share.address))
    {
      /* Clear the old address even if DHCP already replaced it. */

      espclaw_share_clear_entries(g_external);
      if (wan != NULL && IFF_IS_NAT(wan->d_flags))
        {
          ret = nat_disable(wan);
        }

      g_share.active = false;
      g_external = 0;
    }

  if (valid && !g_share.active && ret == 0)
    {
      /* Do not adopt another owner's NAT flag. */

      ret = IFF_IS_NAT(wan->d_flags) ? -EBUSY : nat_enable(wan);
      if (ret == 0)
        {
          g_share.active = true;
          g_external = wan->d_ipaddr;
        }
    }

  g_share.error = ret != 0 ? ret : enable && !valid ?
                  (g_share.ready ? -EADDRINUSE : -ENETDOWN) : 0;
  *out = g_share;
  nat_unlock();
  return ret;
}

FAR struct net_driver_s *espclaw_share_forward(
  FAR struct net_driver_s *input, in_addr_t source, in_addr_t destination)
{
  FAR struct net_driver_s *wan = netdev_findbyname("eth0");
  FAR struct net_driver_s *ap = netdev_findbyname("ap0");

  /* Called with the input device locked. Resolve permanent devices before
   * taking the NAT gate, then serialize state with the same gate as update.
   * Never fall through to wlan0 or turn
   * other interfaces into a router just because IPFORWARD was built in.
   */

  nat_lock();
  if (g_share.active && share_valid(wan, ap) &&
      wan->d_ipaddr == g_external && IFF_IS_NAT(wan->d_flags))
    {
      if (input == ap && share_host(source, ap->d_ipaddr, ap->d_netmask) &&
          (destination & ap->d_netmask) !=
          (ap->d_ipaddr & ap->d_netmask) &&
          ntohl(destination) >= 0x01000000u &&
          ntohl(destination) < 0xe0000000u &&
          (ntohl(destination) >> 24) != 127)
        {
          g_share.outbound++;
          nat_unlock();
          return wan;
        }

      if (input == wan &&
          share_host(destination, ap->d_ipaddr, ap->d_netmask))
        {
          g_share.inbound++;
          nat_unlock();
          return ap;
        }
    }

  g_share.dropped++;
  nat_unlock();
  return NULL;
}
