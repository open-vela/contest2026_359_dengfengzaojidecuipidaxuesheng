/****************************************************************************
 * apps/system/c6probe/c6net.c
 *
 * ESP-Hosted STA Ethernet adapter for the NuttX network stack.
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_NET

#include <errno.h>
#include <net/if.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/mutex.h>
#include <nuttx/net/net.h>
#include <nuttx/net/netdev.h>
#include <nuttx/net/netconfig.h>
#ifdef CONFIG_NET_NAT44
#  include <nuttx/net/espclaw_share.h>
#endif

#include "c6net.h"
#include "c6_softap.h"
#include "c6_wifi_trial.h"
#include "link_state.h"
#include "esp_hosted.h"

#ifndef CONFIG_NET_ETH_PKTSIZE
#  define CONFIG_NET_ETH_PKTSIZE 1500
#endif

#define C6NET_BUFSIZE (CONFIG_NET_ETH_PKTSIZE + 64)
#define C6NET_POLL_US       10000
#define C6NET_TASK_PRIORITY 110
#define C6NET_TASK_STACK    4096

struct c6net_state_s
{
  struct net_driver_s dev;
  pid_t daemon_pid;
  bool registered;
  bool tx_pending;
  uint8_t if_type;
  uint8_t buf[C6NET_BUFSIZE] __attribute__((aligned(4)));
};

static struct c6net_state_s g_c6net;
static sem_t g_c6net_tx_event;
static bool g_c6net_tx_event_ready;
static bool g_c6net_tx_signalled;
static bool g_c6net_tx_wake_enabled = true;

int c6net_tx_wake(bool enable)
{
  __atomic_store_n(&g_c6net_tx_wake_enabled, enable, __ATOMIC_RELEASE);
  return 0;
}

static void c6net_tx_notify(void)
{
  if (__atomic_load_n(&g_c6net_tx_event_ready, __ATOMIC_ACQUIRE) &&
      __atomic_load_n(&g_c6net_tx_wake_enabled, __ATOMIC_ACQUIRE) &&
      !__atomic_exchange_n(&g_c6net_tx_signalled, true, __ATOMIC_ACQ_REL))
    {
      if (sem_post(&g_c6net_tx_event) < 0)
        {
          __atomic_store_n(&g_c6net_tx_signalled, false, __ATOMIC_RELEASE);
        }
    }
}

static void c6net_tx_wait(void)
{
  struct timespec deadline;
  int ret;

  if (clock_gettime(CLOCK_MONOTONIC, &deadline) < 0)
    {
      usleep(C6NET_POLL_US);
      return;
    }

  deadline.tv_nsec += C6NET_POLL_US * 1000;
  if (deadline.tv_nsec >= 1000000000)
    {
      deadline.tv_sec++;
      deadline.tv_nsec -= 1000000000;
    }

  do
    {
      ret = sem_clockwait(&g_c6net_tx_event, CLOCK_MONOTONIC, &deadline);
    }
  while (ret < 0 && errno == EINTR);

  /* Clear only after consuming a token. A post racing a timeout must
   * remain coalesced with that token until the next successful wait.
   * Pending netdev flags are drained at the start of the next iteration.
   */

  if (ret == 0)
    {
      __atomic_store_n(&g_c6net_tx_signalled, false, __ATOMIC_RELEASE);
    }
  else if (errno != ETIMEDOUT)
    {
      usleep(C6NET_POLL_US);
    }
}
/* Separate from the resettable netdev. Read-only diagnostics remain usable
 * during setup/failure without taking the connection, network or bus locks.
 */
static struct c6net_stats_s g_c6net_stats[2];

static FAR struct c6net_stats_s *c6net_stats(uint8_t if_type)
{
  return &g_c6net_stats[if_type == ESP_HOSTED_IF_AP ? 1 : 0];
}

static void c6net_stat_add(FAR uint32_t *counter, uint32_t value)
{
  (void)__atomic_fetch_add(counter, value, __ATOMIC_RELAXED);
}

static void c6net_stat_max(FAR uint32_t *counter, uint32_t value)
{
  uint32_t previous = __atomic_load_n(counter, __ATOMIC_RELAXED);
  while (value > previous &&
         !__atomic_compare_exchange_n(counter, &previous, value, false,
                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED))
    {
    }
}

static uint32_t c6net_now_us(void)
{
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (uint32_t)((uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000);
}

int c6net_stats_read(bool softap, FAR struct c6net_stats_s *out)
{
  FAR struct c6net_stats_s *stats = &g_c6net_stats[softap ? 1 : 0];
  if (out == NULL) return -EINVAL;
#define C6NET_READ_STAT(field) \
  out->field = __atomic_load_n(&stats->field, __ATOMIC_RELAXED)
  C6NET_READ_STAT(tx_notifications);
  C6NET_READ_STAT(tx_polls);
  C6NET_READ_STAT(tx_packets);
  C6NET_READ_STAT(tx_bytes);
  C6NET_READ_STAT(tx_errors);
  C6NET_READ_STAT(tx_down_drops);
  C6NET_READ_STAT(tx_send_max_us);
  C6NET_READ_STAT(rx_packets);
  C6NET_READ_STAT(rx_bytes);
  C6NET_READ_STAT(rx_invalid);
  C6NET_READ_STAT(rx_down_drops);
  C6NET_READ_STAT(rx_unsupported);
  C6NET_READ_STAT(tx_last_error);
#undef C6NET_READ_STAT
  return 0;
}
/* Singleton companion radio; keep mutex outside the resettable netdev. */
static struct c6_link_state g_c6link = C6_LINK_STATE_INITIALIZER;

int c6net_get_link_snapshot(struct c6_link_snapshot *out)
{
  return c6_link_read(&g_c6link, out);
}

static void c6net_sync_carrier(struct net_driver_s *dev)
{
  struct c6_link_snapshot value;
  netdev_lock(dev);
  if (c6_link_read(&g_c6link, &value) == 0)
    {
      bool ready = value.initialized && value.ifup && value.associated;
      if (ready) netdev_carrier_on(dev);
      else netdev_carrier_off(dev);
      if (c6_link_publish_carrier(&g_c6link, value.generation, ready) < 0)
        netdev_carrier_off(dev);
    }
  else netdev_carrier_off(dev);
  netdev_unlock(dev);
}
static pthread_mutex_t g_c6net_connect_lock = PTHREAD_MUTEX_INITIALIZER;

/* Protected by the connection lock; an explicit disconnect stays offline. */

static bool g_c6net_reconnect = true;
static bool g_c6net_trial_owned;
static bool g_c6net_trial_reconnect;

static void c6_softap_sync_carrier(void);
static void c6_softap_poll_pending(void);
static void c6net_poll_pending(FAR struct c6net_state_s *priv);
static int c6_softap_setup_mode(void);

static int c6net_daemon(int argc, FAR char *argv[])
{
  FAR struct c6net_state_s *priv = &g_c6net;

  UNUSED(argc);
  UNUSED(argv);

  while (c6net_is_initialized())
    {
      c6net_sync_carrier(&priv->dev);
      c6_softap_sync_carrier();

      /* Bound RX draining so a busy radio cannot starve deferred TX. */

      for (unsigned n = 0; n < 16 && !esp_hosted_rpc_is_waiting(); n++)
        {
          if (esp_hosted_poll() <= 0)
            {
              break;
            }
        }

      c6net_poll_pending(priv);
      c6_softap_poll_pending();
#ifdef CONFIG_NET_NAT44
      espclaw_share_fwd_poll();
#endif
      c6net_tx_wait();
    }

  priv->daemon_pid = -1;
  return 0;
}

static int c6net_txpoll(FAR struct net_driver_s *dev)
{
  if (dev->d_len > 0)
    {
      FAR struct c6net_state_s *priv = dev->d_private;
      FAR struct c6net_stats_s *stats = c6net_stats(priv->if_type);
      if (priv->if_type == ESP_HOSTED_IF_AP &&
          c6_softap_preserve_mode(1) != 3)
        {
          dev->d_len = 0;
          c6net_stat_add(&stats->tx_down_drops, 1);
          return -ENETDOWN;
        }

      uint32_t started = c6net_now_us();
      int ret = esp_hosted_send(priv->if_type, 0, dev->d_buf, dev->d_len);
      uint32_t ended = c6net_now_us();
      if (started != 0 && ended != 0)
        c6net_stat_max(&stats->tx_send_max_us, ended - started);
      if (ret < 0)
        {
          c6net_stat_add(&stats->tx_errors, 1);
          __atomic_store_n(&stats->tx_last_error, ret, __ATOMIC_RELAXED);
          return -EIO;
        }

      c6net_stat_add(&stats->tx_packets, 1);
      c6net_stat_add(&stats->tx_bytes, dev->d_len);
      dev->d_len = 0;
    }

  return 0;
}

static int c6net_ifup(FAR struct net_driver_s *dev)
{
  FAR struct c6net_state_s *priv = dev->d_private;
  if (priv->if_type == ESP_HOSTED_IF_AP)
    {
      if (c6_softap_preserve_mode(1) != 3)
        {
          return -ENETDOWN;
        }

      dev->d_flags |= IFF_UP | IFF_RUNNING;
      return 0;
    }

  int ret = c6_link_update(&g_c6link, C6_LINK_IFUP);
  if (ret < 0) return ret;
  dev->d_flags |= IFF_UP | IFF_RUNNING;
  return 0;
}

static int c6net_ifdown(FAR struct net_driver_s *dev)
{
  FAR struct c6net_state_s *priv = dev->d_private;
  if (priv->if_type == ESP_HOSTED_IF_AP)
    {
      dev->d_flags &= ~(IFF_UP | IFF_RUNNING);
      netdev_carrier_off(dev);
      return 0;
    }

  int ret = c6_link_update(&g_c6link, C6_LINK_IFDOWN);
  dev->d_flags &= ~(IFF_UP | IFF_RUNNING);
  netdev_carrier_off(dev);
  return ret;
}

static int c6net_txavail(FAR struct net_driver_s *dev)
{
  FAR struct c6net_state_s *priv = dev->d_private;

  /* A socket sender can hold its connection lock here. Polling inline
   * takes the UDP list lock and then other connection locks, deadlocking
   * simultaneous STA DHCP and AP DHCP sends. Only notify the daemon.
   */

  __atomic_store_n(&priv->tx_pending, true, __ATOMIC_RELEASE);
  c6net_tx_notify();
  c6net_stat_add(&c6net_stats(priv->if_type)->tx_notifications, 1);
  return 0;
}

static void c6net_poll_pending(FAR struct c6net_state_s *priv)
{
  if (__atomic_exchange_n(&priv->tx_pending, false, __ATOMIC_ACQ_REL))
    {
      c6net_stat_add(&c6net_stats(priv->if_type)->tx_polls, 1);
      FAR struct net_driver_s *dev = &priv->dev;
      netdev_lock(dev);
      if ((dev->d_flags & IFF_UP) != 0)
        {
          devif_poll(dev, c6net_txpoll);
        }

      netdev_unlock(dev);
    }
}

static void c6net_wifi_event(FAR void *arg, bool connected)
{
  UNUSED(arg);
  (void)c6_link_update(&g_c6link, connected ? C6_LINK_ASSOCIATED :
                                           C6_LINK_DISCONNECTED);
}

static void c6net_rx(FAR void *arg, uint8_t if_type,
                     FAR const uint8_t *payload, uint16_t len,
                     uint8_t pkt_type)
{
  FAR struct c6net_state_s *priv = arg;
  FAR struct c6net_stats_s *stats = c6net_stats(priv->if_type);
  uint16_t ethertype;

  UNUSED(pkt_type);

  struct c6_link_snapshot value;
  /* The legacy Hosted callback passes the interface selector, not the
   * header's instance number. The transport validates STA/AP instance zero.
   */

  if (if_type != priv->if_type || len < 14 || len > sizeof(priv->buf))
    {
      c6net_stat_add(&stats->rx_invalid, 1);
      return;
    }

  netdev_lock(&priv->dev);
  if ((priv->dev.d_flags & IFF_UP) == 0 ||
      (priv->if_type == ESP_HOSTED_IF_AP &&
       c6_softap_preserve_mode(1) != 3) ||
      (priv->if_type == ESP_HOSTED_IF_STA &&
       (c6_link_read(&g_c6link, &value) < 0 || !value.ifup)))
    {
      c6net_stat_add(&stats->rx_down_drops, 1);
      netdev_unlock(&priv->dev);
      return;
    }

  memcpy(priv->buf, payload, len);
  c6net_stat_add(&stats->rx_packets, 1);
  c6net_stat_add(&stats->rx_bytes, len);
  priv->dev.d_buf = priv->buf;
  priv->dev.d_len = len;

  ethertype = ((uint16_t)priv->buf[12] << 8) | priv->buf[13];
  if (ethertype == ETHTYPE_IP)
    {
      ipv4_input(&priv->dev);
    }
#ifdef CONFIG_NET_IPv6
  else if (ethertype == ETHTYPE_IPV6)
    {
      ipv6_input(&priv->dev);
    }
#endif
#ifdef CONFIG_NET_ARP
  else if (ethertype == ETHTYPE_ARP)
    {
      arp_input(&priv->dev);
    }
#endif
  else
    {
      c6net_stat_add(&stats->rx_unsupported, 1);
      priv->dev.d_len = 0;
    }

  /* NuttX returns protocol responses such as TCP ACKs and ARP replies in
   * d_buf/d_len.  Ethernet drivers must transmit that response before
   * releasing the network lock; dropping it stalls multi-segment streams. */

  if (priv->dev.d_len > 0)
    {
      (void)c6net_txpoll(&priv->dev);
    }

  netdev_unlock(&priv->dev);
}

static int c6net_setup(FAR const char *ssid, FAR const char *password,
                       bool associate)
{
  FAR struct c6net_state_s *priv = &g_c6net;
  uint8_t mac[6];
  int lockret;
  int ret;

  lockret = pthread_mutex_lock(&g_c6net_connect_lock);
  if (lockret != 0)
    {
      return -lockret;
    }

  if (!__atomic_load_n(&g_c6net_tx_event_ready, __ATOMIC_ACQUIRE))
    {
      if (sem_init(&g_c6net_tx_event, 0, 0) < 0)
        {
          ret = -errno;
          goto out;
        }

      if (sem_setprotocol(&g_c6net_tx_event, SEM_PRIO_NONE) < 0)
        {
          ret = -errno;
          sem_destroy(&g_c6net_tx_event);
          goto out;
        }

      __atomic_store_n(&g_c6net_tx_event_ready, true, __ATOMIC_RELEASE);
    }

  if (c6net_is_initialized())
    {
      if (!associate) { ret = 0; goto out; }
      if (c6_wifi_trial_pending()) { ret = -EBUSY; goto out; }
      g_c6net_reconnect = true;
      ret = c6_link_update(&g_c6link, C6_LINK_DISCONNECTED);
      if (ret < 0) goto out;
      ret = esp_hosted_rpc_wifi_connect(ssid, password);
      goto out;
    }

  if (priv->registered)
    {
      ret = c6_softap_setup_mode();
      if (ret < 0) goto out;
      goto start_daemon;
    }

  ret = esp_hosted_initialize(false);
  if (ret < 0)
    {
      goto out;
    }

  ret = esp_hosted_rpc_wifi_init();
  if (ret < 0)
    {
      goto out;
    }

  ret = c6_softap_setup_mode();
  if (ret < 0)
    {
      goto out;
    }

  ret = esp_hosted_rpc_wifi_start();
  if (ret < 0)
    {
      goto out;
    }

  ret = esp_hosted_rpc_get_mac(0, mac);
  if (ret < 0)
    {
      goto out;
    }

  memset(priv, 0, sizeof(*priv));
  priv->if_type = ESP_HOSTED_IF_STA;
  memcpy(priv->dev.d_mac.ether.ether_addr_octet, mac, 6);
  /* netdev_register() expects a format, not a fixed name.  Keep the
   * companion radio separate from the board's wired Ethernet interface. */

  strlcpy(priv->dev.d_ifname, C6NET_IFNAME_FORMAT,
          sizeof(priv->dev.d_ifname));
  priv->dev.d_llhdrlen = 14;
  priv->dev.d_pktsize = CONFIG_NET_ETH_PKTSIZE;
  priv->dev.d_buf = priv->buf;
  priv->dev.d_ifup = c6net_ifup;
  priv->dev.d_ifdown = c6net_ifdown;
  priv->dev.d_txavail = c6net_txavail;
  priv->dev.d_private = priv;

  ret = esp_hosted_rpc_set_wifi_event_cb(c6net_wifi_event, priv);
  if (ret < 0)
    {
      goto out;
    }

  ret = esp_hosted_register(ESP_HOSTED_IF_STA, c6net_rx, priv);
  if (ret < 0)
    {
      goto out;
    }

  ret = netdev_register(&priv->dev, NET_LL_ETHERNET);
  if (ret < 0)
    {
      goto out;
    }

  priv->registered = true;

start_daemon:
  ret = c6_link_update(&g_c6link, C6_LINK_STARTED);
  if (ret < 0) goto out;
  netdev_lock(&priv->dev);
  ret = c6net_ifup(&priv->dev);
  netdev_unlock(&priv->dev);
  if (ret < 0)
    {
      (void)c6_link_update(&g_c6link, C6_LINK_STOPPED);
      goto out;
    }

  priv->daemon_pid = task_create("c6net", C6NET_TASK_PRIORITY,
                                 C6NET_TASK_STACK, c6net_daemon, NULL);
  if (priv->daemon_pid < 0)
    {
      ret = -errno;
      (void)c6_link_update(&g_c6link, C6_LINK_STOPPED);
      netdev_lock(&priv->dev);
      c6net_ifdown(&priv->dev);
      netdev_unlock(&priv->dev);
      goto out;
    }

  /* An empty SSID asks the C6 to reconnect with the credentials persisted in
   * its own NVS.  A failed association does not tear down eth0, so Settings
   * can provide new credentials without resetting the transport. */

  if (associate) g_c6net_reconnect = true;
  ret = associate ? esp_hosted_rpc_wifi_connect(ssid, password) : 0;

out:
  pthread_mutex_unlock(&g_c6net_connect_lock);
  return ret;
}


int c6net_disconnect(void)
{
  int ret = pthread_mutex_lock(&g_c6net_connect_lock);
  if (ret != 0) return -ret;
  if (c6_wifi_trial_pending()) ret = -EBUSY;
  else if (!c6net_is_initialized()) ret = -ENETDOWN;
  else
    {
      g_c6net_reconnect = false;
      ret = esp_hosted_rpc_wifi_disconnect();
      if (ret == 0) ret = c6_link_update(&g_c6link, C6_LINK_DISCONNECTED);
    }
  pthread_mutex_unlock(&g_c6net_connect_lock);
  return ret;
}

int c6net_reconnect(void)
{
  struct c6_link_snapshot state;
  int ret = pthread_mutex_lock(&g_c6net_connect_lock);

  if (ret != 0)
    {
      return -ret;
    }

  if (c6_wifi_trial_pending())
    {
      pthread_mutex_unlock(&g_c6net_connect_lock);
      return -EBUSY;
    }

  ret = c6_link_read(&g_c6link, &state);
  if (ret == 0 && g_c6net_reconnect && state.initialized && state.ifup &&
      !state.associated)
    {
      /* RPC needs RX to make progress; call from the network worker. */

      ret = esp_hosted_rpc_wifi_connect("", "");
    }

  pthread_mutex_unlock(&g_c6net_connect_lock);
  return ret;
}

int c6net_prepare(void)
{
  return c6net_setup(NULL, NULL, false);
}

int c6net_trial_connect(FAR const char *ssid, FAR const char *password)
{
  struct c6_link_snapshot before;
  struct c6_link_snapshot after;
  int ret = c6net_prepare();

  if (ret < 0)
    {
      return ret;
    }

  ret = pthread_mutex_lock(&g_c6net_connect_lock);
  if (ret != 0)
    {
      return -ret;
    }

  if (g_c6net_trial_owned || c6_wifi_trial_pending())
    {
      ret = -EBUSY;
      goto out;
    }

  g_c6net_trial_reconnect = g_c6net_reconnect;
  g_c6net_trial_owned = true;
  g_c6net_reconnect = false;
  ret = c6_link_read(&g_c6link, &before);
  if (ret == 0)
    {
      ret = c6_wifi_trial_begin(ssid, password);
    }

  if (!c6_wifi_trial_pending())
    {
      g_c6net_reconnect = g_c6net_trial_reconnect;
      g_c6net_trial_owned = false;
    }
  else if (c6_link_read(&g_c6link, &after) == 0 &&
           after.generation == before.generation)
    {
      /* Real events received during begin already invalidate the old DHCP
       * generation. Do not overwrite a new association received there.
       * Pre-mutation validation/snapshot failures leave the old link intact.
       */

      (void)c6_link_update(&g_c6link, C6_LINK_DISCONNECTED);
    }

out:
  pthread_mutex_unlock(&g_c6net_connect_lock);
  return ret;
}

int c6net_trial_finish(bool commit)
{
  int ret = pthread_mutex_lock(&g_c6net_connect_lock);

  if (ret != 0)
    {
      return -ret;
    }

  if (!g_c6net_trial_owned)
    {
      ret = -ENOENT;
      goto out;
    }

  ret = c6_wifi_trial_finish(commit);
  if (ret < 0)
    {
      goto out;
    }

  g_c6net_reconnect = commit ? true : g_c6net_trial_reconnect;
  g_c6net_trial_owned = false;
  if (!commit)
    {
      (void)c6_link_update(&g_c6link, C6_LINK_DISCONNECTED);
      if (g_c6net_reconnect)
        {
          /* Rollback is complete even when the old AP is currently absent.
           * The normal network worker retains the restored retry policy.
           */

          (void)esp_hosted_rpc_wifi_connect("", "");
        }
    }

out:
  pthread_mutex_unlock(&g_c6net_connect_lock);
  return ret;
}

int c6net_initialize(FAR const char *ssid, FAR const char *password)
{
  return c6net_setup(ssid, password, true);
}

int c6net_connect(FAR const char *ssid, FAR const char *password)
{
  return c6net_initialize(ssid, password);
}

bool c6net_is_initialized(void)
{
  struct c6_link_snapshot value;
  return c6_link_read(&g_c6link, &value) == 0 && value.initialized;
}

bool c6net_is_associated(void)
{
  struct c6_link_snapshot value;
  return c6_link_read(&g_c6link, &value) == 0 && value.initialized &&
         value.ifup && value.associated && value.carrier_ready;
}

#include "c6_softap_net.inc"

#endif /* CONFIG_NET */
