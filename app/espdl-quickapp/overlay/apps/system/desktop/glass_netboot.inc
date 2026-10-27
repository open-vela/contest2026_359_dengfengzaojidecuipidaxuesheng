/* Maintain Wi-Fi and wired IPv4 connectivity for the desktop lifetime.
 *
 * The companion radio is only a bridge: the host owns wlan0 and IPv4, so a
 * lease has to be requested after the station associates. The slave keeps the
 * credentials of the last successful join in its own NVS, which is what makes
 * the boot-time association possible without storing a password on the host.
 * Permanent workers reacquire leases at half their lifetime and after link
 * changes. All deadlines use monotonic time rather than the NTP-adjusted clock.
 */
#include "glass_status.h"
#ifdef CONFIG_NET_NAT44
#include "glass_ethernet.h"
#endif
#ifdef CONFIG_SYSTEM_C6_DESKTOP
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <netutils/netlib.h>
#include <netutils/dhcpc.h>
#include <net/if.h>
#ifdef CONFIG_NETUTILS_NTPCLIENT
#  include <netutils/ntpclient.h>
#endif
#include "c6net.h"
#include "link_state.h"
#include "../c6probe/status_rssi.h"

#define GLASS_NET_IFNAME C6NET_IFNAME
/* The companion-radio bring-up and the DHCP client run on these threads and
 * need the same stack budget as the C6 desktop worker. A smaller stack
 * overflows silently and corrupts the heap. */
#ifndef CONFIG_SYSTEM_C6_DESKTOP_STACKSIZE
#  define GLASS_NET_STACKSIZE 8192
#else
#  define GLASS_NET_STACKSIZE CONFIG_SYSTEM_C6_DESKTOP_STACKSIZE
#endif

static pthread_mutex_t g_net_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_net_notice[96];
static bool g_net_notice_pending;
static bool g_net_lease_requested;
static bool g_net_lease_announce;
static bool g_net_time_started;
static uint64_t g_net_wifi_lease_generation;
static bool g_net_wifi_lease_ready;
/* A nonzero interface address may still belong to the previous association.
 * Settings only confirms success after the permanent DHCP owner commits it. */
static bool glass_net_wifi_ready(uint64_t generation)
{
  pthread_mutex_lock(&g_net_lock);
  bool ready = g_net_wifi_lease_ready &&
               g_net_wifi_lease_generation == generation;
  pthread_mutex_unlock(&g_net_lock);
  return ready;
}
bool glass_wifi_lease_ready(uint64_t generation)
{
  return glass_net_wifi_ready(generation);
}
static struct glass_status_network g_status_network;
static void glass_status_network_get(struct glass_status_network *out)
{
  pthread_mutex_lock(&g_net_lock); *out = g_status_network;
  pthread_mutex_unlock(&g_net_lock);
}
static void glass_status_network_publish(bool wired, bool linked, bool usable)
{
  pthread_mutex_lock(&g_net_lock);
  if (wired) g_status_network.wired = usable;
  else { g_status_network.wifi = usable; g_status_network.joining = linked && !usable;
    if(!linked)g_status_network.rssi_valid=false; }
  pthread_mutex_unlock(&g_net_lock);
}
static pthread_mutex_t g_net_dns_lock = PTHREAD_MUTEX_INITIALIZER;
/* dhcpc_open binds UDP port 68 before SO_BINDTODEVICE. Serialize clients
 * across interfaces so simultaneous boot-time leases cannot fail to bind. */
static pthread_mutex_t g_net_dhcp_lock = PTHREAD_MUTEX_INITIALIZER;
static struct in_addr g_net_wifi_dns, g_net_wired_dns, g_net_active_dns;
#ifdef CONFIG_NET_NAT44
static struct glass_ethernet_s g_wired_snapshot;

void glass_wired_snapshot(struct glass_ethernet_s *out)
{
  pthread_mutex_lock(&g_net_lock);
  *out = g_wired_snapshot;
  pthread_mutex_unlock(&g_net_lock);
}
#endif
static int g_net_dns_source = -1;

/* Each permanent interface worker owns its lease deadlines. Wall-clock NTP
 * corrections must not postpone renewal or revive an expired address. */
struct glass_net_lease_s
{
  uint64_t generation;
  uint64_t renew_at;
  uint64_t expires_at;
  uint64_t retry_at;
  bool configured;
  bool ready;
};

static uint64_t glass_net_seconds(void)
{
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec;
}

/* Choose the same order as the IPv4 default-device policy: usable wired
 * gateway first, then Wi-Fi. Carrier without a DHCP address is not usable. */
static bool glass_net_usable(const char *name)
{
  uint8_t flags;
  struct in_addr ip, gateway;
  return netlib_getifstatus(name, &flags) == 0 && (flags & IFF_RUNNING) &&
         netlib_get_ipv4addr(name, &ip) == 0 && ip.s_addr != 0 &&
         netlib_get_dripv4addr(name, &gateway) == 0 && gateway.s_addr != 0;
}

static void glass_net_select_dns(void)
{
  struct in_addr dns;
  int source;

  pthread_mutex_lock(&g_net_dns_lock);
  source = glass_net_usable("eth0") ? 1 :
           glass_net_usable(GLASS_NET_IFNAME) ? 0 : -1;
  pthread_mutex_lock(&g_net_lock);
  dns = source == 1 ? g_net_wired_dns : g_net_wifi_dns;
  pthread_mutex_unlock(&g_net_lock);
  if (source >= 0 && dns.s_addr != 0 &&
      (source != g_net_dns_source || dns.s_addr != g_net_active_dns.s_addr))
    {
      netlib_clear_dnsaddr();
      if (netlib_set_ipv4dnsaddr(&dns) == 0)
        {
          g_net_active_dns = dns;
          g_net_dns_source = source;
          printf("[net] internet=%s dns=%s\n",
                 source == 1 ? "eth0" : GLASS_NET_IFNAME, inet_ntoa(dns));
        }
    }
  else if ((source < 0 || dns.s_addr == 0) && g_net_dns_source >= 0)
    {
      netlib_clear_dnsaddr();
      g_net_active_dns.s_addr = 0;
      g_net_dns_source = -1;
    }
  pthread_mutex_unlock(&g_net_dns_lock);
}

/* Retain each DHCP DNS answer instead of letting whichever client finishes
 * last overwrite the global resolver. No DHCP work runs on the UI thread. */
static int glass_net_obtain_ipv4addr(const char *name, bool wired,
                                    struct glass_net_lease_s *state)
{
  uint8_t mac[IFHWADDRLEN];
  struct dhcpc_state lease;
  void *handle;
  uint64_t started;
  int ret = netlib_getmacaddr(name, mac);
  if (ret < 0) return ret;
  pthread_mutex_lock(&g_net_dhcp_lock);
  handle = dhcpc_open(name, mac, sizeof(mac));
  if (!handle)
    {
      ret = errno ? -errno : -ENOMEM;
      pthread_mutex_unlock(&g_net_dhcp_lock);
      return ret;
    }
  memset(&lease, 0, sizeof(lease));
  started = glass_net_seconds();
  ret = dhcpc_request(handle, &lease);
  if (ret < 0) ret = errno ? -errno : -EIO;
  dhcpc_close(handle);
  pthread_mutex_unlock(&g_net_dhcp_lock);
  if (ret < 0) return ret;
  if (lease.ipaddr.s_addr == 0 || lease.lease_time == 0) return -ESTALE;
  if (!wired)
    {
      struct c6_link_snapshot current;
      if (c6net_get_link_snapshot(&current) < 0 || !current.initialized ||
          !current.ifup || !current.associated || !current.carrier_ready ||
          current.generation != state->generation)
        return -EAGAIN;
    }
  else
    {
      uint8_t flags;
      if (netlib_getifstatus(name, &flags) < 0 || !(flags & IFF_RUNNING))
        return -EAGAIN;
    }

  /* dhcpc_request also installs the ACK address. Its caller must discard it
   * if the link changed during negotiation, before publishing DNS/routes. */

  if (lease.lease_time != UINT32_MAX &&
      glass_net_seconds() >= started + lease.lease_time)
    return -ESTALE;
  ret = netlib_set_ipv4addr(name, &lease.ipaddr);
  if (ret == 0) ret = netlib_set_ipv4netmask(name, &lease.netmask);
  if (ret == 0) ret = netlib_set_dripv4addr(name, &lease.default_router);
  if (ret < 0) return -ESTALE;
  state->configured = true;
  state->renew_at = lease.lease_time == UINT32_MAX ? UINT64_MAX :
                   started + (lease.lease_time > 1 ? lease.lease_time / 2 : 1);
  state->expires_at = lease.lease_time == UINT32_MAX ? UINT64_MAX :
                     started + lease.lease_time;
  printf("[net] %s lease=%lu seconds\n", name,
         (unsigned long)lease.lease_time);
  pthread_mutex_lock(&g_net_lock);
  if (wired) g_net_wired_dns = lease.dnsaddr;
  else g_net_wifi_dns = lease.dnsaddr;
  pthread_mutex_unlock(&g_net_lock);
  glass_net_select_dns();
  return 0;
}

static void glass_net_clear_ipv4(const char *name, bool wired,
                                struct glass_net_lease_s *state)
{
  struct in_addr zero = {0};

  (void)netlib_set_dripv4addr(name, &zero);
  (void)netlib_set_ipv4addr(name, &zero);
  (void)netlib_set_ipv4netmask(name, &zero);
  state->configured = false;
  pthread_mutex_lock(&g_net_lock);
  if (wired) g_net_wired_dns = zero;
  else g_net_wifi_dns = zero;
  pthread_mutex_unlock(&g_net_lock);
  glass_net_select_dns();
}

/* Retry failed renewals while the old lease remains valid. After expiry or a
 * link change, remove the old address and route before requesting a new one.
 * Only the owning worker calls this, including requests from Settings. */
static int glass_net_maintain(const char *name, bool wired,
                              struct glass_net_lease_s *state, bool ready,
                              uint64_t generation, bool requested)
{
  uint64_t now = glass_net_seconds();
  struct in_addr ip;
  int ret;

  if (ready != state->ready || generation != state->generation)
    {
      glass_net_clear_ipv4(name, wired, state);
      state->generation = generation;
      state->ready = ready;
      state->retry_at = 0;
    }

  if (!ready) return -ENETDOWN;

  if (state->configured && (now >= state->expires_at ||
      netlib_get_ipv4addr(name, &ip) < 0 || ip.s_addr == 0))
    glass_net_clear_ipv4(name, wired, state);

  if (!requested && (now < state->retry_at ||
      (state->configured && now < state->renew_at)))
    return -EINPROGRESS;

  ret = glass_net_obtain_ipv4addr(name, wired, state);
  now = glass_net_seconds();
  if (ret < 0)
    {
      if (ret == -EAGAIN || ret == -ESTALE || !state->configured ||
          now >= state->expires_at ||
          netlib_get_ipv4addr(name, &ip) < 0 || ip.s_addr == 0)
        glass_net_clear_ipv4(name, wired, state);
      state->retry_at = now + 10;
      printf("[net] %s DHCP retry: %d\n", name, ret);
    }
  else state->retry_at = 0;
  return ret;
}

/* The system clock has to be right for HTTPS, and the desktop only shows a
 * date once it is. The daemon keeps retrying, so starting it once is enough. */
static void glass_net_time_sync(void)
{
#ifdef CONFIG_NETUTILS_NTPCLIENT
  pthread_mutex_lock(&g_net_lock);
  if (g_net_time_started)
    {
      pthread_mutex_unlock(&g_net_lock);
      return;
    }
  g_net_time_started = true;
  pthread_mutex_unlock(&g_net_lock);
  printf("[net] ntp start: %d\n", ntpc_start());
#endif
}

/* Messages are handed to the LVGL thread instead of touching the desktop from
 * the network worker. */
static void glass_net_post(const char *text)
{
  pthread_mutex_lock(&g_net_lock);
  snprintf(g_net_notice, sizeof(g_net_notice), "%s", text);
  g_net_notice_pending = true;
  pthread_mutex_unlock(&g_net_lock);
}

static const char *glass_net_take_notice(void)
{
  static char text[sizeof(g_net_notice)];

  pthread_mutex_lock(&g_net_lock);
  if (!g_net_notice_pending)
    {
      pthread_mutex_unlock(&g_net_lock);
      return NULL;
    }

  strlcpy(text, g_net_notice, sizeof(text));
  g_net_notice_pending = false;
  pthread_mutex_unlock(&g_net_lock);
  return text;
}

static bool glass_net_address(char *text, size_t length)
{
  struct in_addr address;

  if (netlib_get_ipv4addr(GLASS_NET_IFNAME, &address) < 0 || address.s_addr == 0)
    {
      return false;
    }

  snprintf(text, length, "%u.%u.%u.%u",
           (unsigned)(address.s_addr & 0xff),
           (unsigned)((address.s_addr >> 8) & 0xff),
           (unsigned)((address.s_addr >> 16) & 0xff),
           (unsigned)((address.s_addr >> 24) & 0xff));
  return true;
}

/* Settings only posts a request. DHCP always runs on the permanent worker,
 * even when thread allocation fails or a connect finishes before association. */
static void glass_net_lease(bool announce)
{
  pthread_mutex_lock(&g_net_lock);
  g_net_lease_requested = true;
  g_net_lease_announce |= announce;
  pthread_mutex_unlock(&g_net_lock);
}
void glass_wifi_request_lease(void)
{
  glass_net_lease(true);
}

static void *glass_net_boot_thread(void *arg)
{
  struct glass_net_lease_s lease = {0};
  uint64_t reconnect_at = glass_net_seconds() + 12;
  bool announce = true;
  int ret = -1;

  (void)arg;
  for (int attempt = 0; attempt < 4; attempt++)
    {
      ret = c6net_prepare();
      if (ret == 0) break;
      printf("[net] companion radio attempt %d: %d\n", attempt + 1, ret);
      sleep(2);
    }

  if (ret < 0)
    {
      printf("[net] companion radio unavailable: %d\n", ret);
      return NULL;
    }

  glass_net_clear_ipv4(GLASS_NET_IFNAME, false, &lease);
  uint64_t rssi_at=0, rssi_generation=UINT64_MAX;
  for (;;)
    {
      struct c6_link_snapshot link;
      bool requested = false;
      bool ready;

      if (c6net_get_link_snapshot(&link) < 0)
        {
          glass_status_network_publish(false, false, false);
          sleep(1);
          continue;
        }

      if(link.generation!=rssi_generation){
        rssi_generation=link.generation;rssi_at=0;
        pthread_mutex_lock(&g_net_lock);
        g_status_network.rssi_valid=false;
        pthread_mutex_unlock(&g_net_lock);
      }
      ready = link.initialized && link.ifup && link.associated &&
              link.carrier_ready;
      if(ready&&glass_net_seconds()>=rssi_at){
        int dbm=0;int measured=esp_hosted_rpc_wifi_rssi(&dbm);
        struct c6_link_snapshot current;
        bool same=c6net_get_link_snapshot(&current)==0&&current.generation==link.generation&&current.associated;
        pthread_mutex_lock(&g_net_lock);
        g_status_network.rssi_valid=measured==0&&same;
        g_status_network.rssi_dbm=dbm;
        pthread_mutex_unlock(&g_net_lock);
        rssi_at=glass_net_seconds()+5;
      }
      /* Keep manual requests pending until association/carrier is ready. */

      if (ready)
        {
          pthread_mutex_lock(&g_net_lock);
          requested = g_net_lease_requested;
          announce |= g_net_lease_announce;
          g_net_lease_requested = g_net_lease_announce = false;
          pthread_mutex_unlock(&g_net_lock);
          reconnect_at = glass_net_seconds() + 12;
        }

      glass_status_network_publish(false, ready, ready && lease.configured && lease.generation == link.generation);
      ret = glass_net_maintain(GLASS_NET_IFNAME, false, &lease, ready,
                               link.generation, requested);
      if (ret == 0)
        {
          char address[24];
          glass_net_time_sync();
          if (announce && glass_net_address(address, sizeof(address)))
            {
              char text[sizeof(g_net_notice)];
              snprintf(text, sizeof(text), "已连接 Wi-Fi · %s", address);
              glass_net_post(text);
              announce = false;
            }
        }

      glass_status_network_publish(false, ready, ready && glass_net_usable(GLASS_NET_IFNAME));
      pthread_mutex_lock(&g_net_lock);
      g_net_wifi_lease_generation = lease.generation;
      g_net_wifi_lease_ready = ready && lease.configured &&
                             lease.generation == link.generation;
      pthread_mutex_unlock(&g_net_lock);
      if (!link.associated && glass_net_seconds() >= reconnect_at)
        {
          ret = c6net_reconnect();
          if (ret < 0) printf("[net] reconnect retry: %d\n", ret);
          reconnect_at = glass_net_seconds() + 30;
        }

      glass_net_select_dns();
      sleep(1);
    }

  return NULL;
}

static void *glass_net_wired_thread(void *arg)
{
  struct glass_net_lease_s lease = {0};
  bool was_running = false;
  uint64_t generation = 0;
  uint8_t flags;
  (void)arg;

  /* The desktop owns both DHCP clients; NSH_NETINIT must be disabled to
   * prevent two clients binding eth0's DHCP port and competing over DNS. */
  glass_net_clear_ipv4("eth0", true, &lease);
  (void)netlib_ifup("eth0");
  for (;;)
    {
      bool running = netlib_getifstatus("eth0", &flags) == 0 &&
                     (flags & IFF_RUNNING);
      if (running != was_running) generation++;
      glass_status_network_publish(true, running, running && lease.configured && lease.generation == generation);
      int ret = glass_net_maintain("eth0", true, &lease, running,
                                   generation, false);
      glass_status_network_publish(true, running, running && glass_net_usable("eth0"));
#ifdef CONFIG_NET_NAT44
      struct glass_ethernet_s snapshot = {0};
      struct in_addr ip = {0}, mask = {0}, gateway = {0};
      (void)netlib_get_ipv4addr("eth0", &ip);
      (void)netlib_get_ipv4netmask("eth0", &mask);
      (void)netlib_get_dripv4addr("eth0", &gateway);
      snapshot.link = running;
      snapshot.leased = running && lease.configured &&
                        lease.generation == generation &&
                        glass_net_seconds() < lease.expires_at &&
                        ip.s_addr && mask.s_addr && gateway.s_addr;
      snapshot.address = ip.s_addr;
      snapshot.mask = mask.s_addr;
      snapshot.gateway = gateway.s_addr;
      snapshot.error = snapshot.leased ? 0 : ret;
      pthread_mutex_lock(&g_net_lock);
      snapshot.dns = g_net_wired_dns.s_addr ? g_net_wired_dns.s_addr : gateway.s_addr;
      g_wired_snapshot = snapshot;
      pthread_mutex_unlock(&g_net_lock);
#endif
      if (ret == 0) glass_net_time_sync();
      was_running = running;
      glass_net_select_dns();
      sleep(1);
    }
  return NULL;
}

static void glass_net_autostart(void)
{
  pthread_t thread;
  pthread_attr_t attr;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, GLASS_NET_STACKSIZE);
  if (pthread_create(&thread, &attr, glass_net_wired_thread, NULL) == 0)
    pthread_detach(thread);
  else
    printf("[net] cannot start wired network worker\n");
  if (pthread_create(&thread, &attr, glass_net_boot_thread, NULL) != 0)
    {
      pthread_attr_destroy(&attr);
      printf("[net] cannot start the network worker\n");
      return;
    }

  pthread_attr_destroy(&attr);
  pthread_detach(thread);
}
#endif /* CONFIG_SYSTEM_C6_DESKTOP */
