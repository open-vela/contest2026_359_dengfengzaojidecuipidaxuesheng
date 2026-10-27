/* SPDX-License-Identifier: Apache-2.0 */

#include <nuttx/config.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netutils/dhcpd.h>
#include <netutils/dhcpd_runtime.h>
#include <netutils/netlib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "c6net.h"
#include "c6_softap.h"
#include "c6_wifi_trial.h"
#include "desktop_backend.h"
#include "link_state.h"
#include "glass_network_recovery.h"
#include "glass_portal.h"

#define RECOVERY_JOIN_SECONDS 100
#define RECOVERY_DHCP_SECONDS 8

struct recovery_s
{
  struct c6_desktop_worker worker;
  struct glass_recovery_snapshot_s view;
  struct c6_desktop_link link;
  uint64_t join_deadline;
  uint64_t dhcp_deadline;
  uint64_t phy_deadline; /* Only accessed by the persistent worker. */
  uint64_t trial_generation;
  bool trial_finalized;
  bool dhcp_started;
  char password[13];
};

/* One persistent desktop owner and one mailbox. Large scan/mailbox state is
 * allocated after PSRAM initialization instead of extending early boot BSS.
 */

static pthread_mutex_t g_recovery_lock = PTHREAD_MUTEX_INITIALIZER;
static struct recovery_s *g_recovery;

static uint64_t recovery_seconds(void)
{
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec;
}

static void recovery_wipe(void *data, size_t size)
{
  volatile unsigned char *p = data;
  while (size--) *p++ = 0;
}

static int recovery_credentials_valid(const char *ssid, const char *password)
{
  if (!ssid || !password) return -EINVAL;
  size_t n = strnlen(ssid, 33), p = strnlen(password, 65);
  if (!n || n > 32 || p > 64 || (p && p < 8)) return -EINVAL;
  for (size_t i = 0; i < n; )
    {
      uint32_t code = (uint8_t)ssid[i++];
      unsigned extra = 0;
      uint32_t minimum = 0;
      if (code < 0x80)
        {
          if (code < 32 || code == 127) return -EINVAL;
        }
      else
        {
          if (code >= 0xc2 && code <= 0xdf)
            { extra = 1; minimum = 0x80; code &= 31; }
          else if (code >= 0xe0 && code <= 0xef)
            { extra = 2; minimum = 0x800; code &= 15; }
          else if (code >= 0xf0 && code <= 0xf4)
            { extra = 3; minimum = 0x10000; code &= 7; }
          else return -EINVAL;
          if (extra > n - i) return -EINVAL;
          while (extra--)
            {
              unsigned c = (uint8_t)ssid[i++];
              if ((c & 0xc0) != 0x80) return -EINVAL;
              code = (code << 6) | (c & 63);
            }
          if (code < minimum || code > 0x10ffff ||
              (code >= 0xd800 && code <= 0xdfff)) return -EINVAL;
        }
    }
  for (size_t i = 0; i < p; i++)
    {
      unsigned c = (uint8_t)password[i];
      if (p == 64)
        {
          if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                (c >= 'A' && c <= 'F'))) return -EINVAL;
        }
      else if (c < 32 || c > 126) return -EINVAL;
    }
  return 0;
}

static int recovery_random(uint8_t *data, size_t size)
{
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0) return -errno;
  int ret = 0;
  while (size)
    {
      ssize_t n = read(fd, data, size);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) { ret = -(n < 0 && errno ? errno : EIO); break; }
      data += n;
      size -= n;
    }
  if (close(fd) < 0 && !ret) ret = -errno;
  return ret;
}

static bool recovery_overlaps(uint32_t candidate, const char *interface)
{
  struct in_addr address = {0}, mask = {0};
  if (netlib_get_ipv4addr(interface, &address) < 0 || !address.s_addr)
    return false;
  if (netlib_get_ipv4netmask(interface, &mask) < 0 || !mask.s_addr)
    return true;
  uint32_t ip = ntohl(address.s_addr), subnet = ntohl(mask.s_addr);
  return (candidate & subnet) == (ip & subnet) ||
         (candidate & 0xffffff00u) == (ip & 0xffffff00u);
}

static int recovery_address(uint8_t out[4])
{
  const uint32_t candidates[] = {0xc0a80401, 0xc0a82a01, 0xac170001};
  for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++)
    if (!recovery_overlaps(candidates[i], "eth0") &&
        !recovery_overlaps(candidates[i], C6NET_IFNAME))
      {
        uint32_t ip = htonl(candidates[i]);
        memcpy(out, &ip, 4);
        return 0;
      }
  return -EADDRINUSE;
}

static int recovery_clear_address(void)
{
  struct in_addr zero = {0};
  int ret = 0;
  if (netlib_set_ipv4addr(C6_SOFTAP_IFNAME, &zero) < 0) ret = -errno;
  if (netlib_set_dripv4addr(C6_SOFTAP_IFNAME, &zero) < 0 && !ret) ret = -errno;
  if (netlib_set_ipv4netmask(C6_SOFTAP_IFNAME, &zero) < 0 && !ret) ret = -errno;
  return ret;
}

static int recovery_stop_worker(void)
{
  glass_ethernet_share_stop();
  int ret = 0;
  int observed;
  struct c6_softap_snapshot_s ap = {0};
  if (c6_wifi_trial_pending())
    {
      ret = c6net_trial_finish(false);
      if (c6_wifi_trial_pending()) { if (!ret) ret = -EIO; goto done; }
    }
  pthread_mutex_lock(&g_recovery_lock);
  bool started = g_recovery->dhcp_started;
  pthread_mutex_unlock(&g_recovery_lock);
  if (started)
    {
      int stopped = dhcpd_stop();
      if (stopped < 0) { if (!ret) ret = stopped; goto done; }
      pthread_mutex_lock(&g_recovery_lock);
      g_recovery->dhcp_started = false;
      pthread_mutex_unlock(&g_recovery_lock);
    }
  int stopped = c6_softap_stop();
  if (stopped && !ret) ret = stopped;
  if (!stopped)
    {
      int cleared = recovery_clear_address();
      if (cleared != -ENODEV && cleared != -ENXIO && cleared != -ENOENT && !ret)
        ret = cleared;
    }
done:
  observed = c6_softap_status(&ap);
  if (observed && !ret) ret = observed;
  pthread_mutex_lock(&g_recovery_lock);
  struct recovery_s *s = g_recovery;
  s->view.ap_running = observed ? true : ap.active || ap.state_unknown;
  s->view.dhcp_ready = false;
  s->view.joining = c6_wifi_trial_pending();
  s->view.cleanup_error = ret;
  s->view.state = ret || s->view.ap_running ? GLASS_RECOVERY_ERROR : GLASS_RECOVERY_OFF;
  if (ret && !s->view.ap_error) s->view.ap_error = ret;
  if (!observed && !s->view.ap_running)
    {
      recovery_wipe(s->password, sizeof(s->password));
      memset(s->view.ssid, 0, sizeof(s->view.ssid));
      memset(s->view.ap_ipv4, 0, sizeof(s->view.ap_ipv4));
    }
  pthread_mutex_unlock(&g_recovery_lock);
  return ret;
}

static int recovery_start_worker(const char *ssid, const char *password)
{
  uint8_t ip[4];
  struct in_addr address, mask = {.s_addr = htonl(0xffffff00u)};
  struct in_addr gateway = {0};
  int ret = recovery_address(ip);
  if (ret) goto failed;
  ret = c6_softap_start(ssid, password);
  if (ret) goto failed;
  memcpy(&address.s_addr, ip, 4);
  if (netlib_set_ipv4addr(C6_SOFTAP_IFNAME, &address) < 0 ||
      netlib_set_ipv4netmask(C6_SOFTAP_IFNAME, &mask) < 0 ||
      netlib_set_dripv4addr(C6_SOFTAP_IFNAME, &gateway) < 0)
    { ret = -(errno ? errno : EIO); goto failed; }
  ret = dhcpd_set_startip(ntohl(address.s_addr) + 1);
  if (!ret) ret = dhcpd_set_netmask(0xffffff00u);
#ifdef CONFIG_NET_NAT44
  if (!ret) ret = dhcpd_set_routerip(ntohl(address.s_addr));
#endif
  if (!ret) ret = portal_service_start();
  if (!ret)
    {
      struct dhcpd_runtime_status_s status = {0};
      ret = dhcpd_runtime_status(&status);
      if (!ret && (status.running || status.pid > 0)) ret = -EBUSY;
      if (!ret)
        {
          /* A failed start can still have a child waiting to be reaped. */
          pthread_mutex_lock(&g_recovery_lock);
          g_recovery->dhcp_started = true;
          pthread_mutex_unlock(&g_recovery_lock);
          ret = dhcpd_start(C6_SOFTAP_IFNAME);
        }
    }
  if (ret) goto failed;
  pthread_mutex_lock(&g_recovery_lock);
  g_recovery->dhcp_deadline = recovery_seconds() + RECOVERY_DHCP_SECONDS;
  g_recovery->view.ap_running = true;
  memcpy(g_recovery->view.ap_ipv4, ip, 4);
  pthread_mutex_unlock(&g_recovery_lock);
  return 0;
failed:
  recovery_stop_worker();
  pthread_mutex_lock(&g_recovery_lock);
  g_recovery->view.ap_error = ret;
  g_recovery->view.state = GLASS_RECOVERY_ERROR;
  pthread_mutex_unlock(&g_recovery_lock);
  return ret;
}

static int recovery_connect_worker(const char *ssid, const char *password)
{
  struct c6_link_snapshot link = {0};
  int ret = c6net_trial_connect(ssid, password);
  if (!ret) ret = c6net_get_link_snapshot(&link);
  int cleanup = 0;
  if (ret && c6_wifi_trial_pending()) cleanup = c6net_trial_finish(false);
  pthread_mutex_lock(&g_recovery_lock);
  g_recovery->view.error = ret;
  g_recovery->view.cleanup_error = cleanup;
  g_recovery->view.joining = c6_wifi_trial_pending();
  g_recovery->trial_finalized = ret != 0;
  g_recovery->trial_generation = link.generation;
  g_recovery->join_deadline = recovery_seconds() + RECOVERY_JOIN_SECONDS;
  pthread_mutex_unlock(&g_recovery_lock);
  if (!ret) glass_wifi_request_lease();
  return ret;
}

static int recovery_disconnect_worker(void)
{
  int ret = 0;
  if (c6_wifi_trial_pending()) ret = c6net_trial_finish(false);
  if (!c6_wifi_trial_pending()) ret = g_c6_desktop_backend.disconnect();
  pthread_mutex_lock(&g_recovery_lock);
  g_recovery->view.joining = c6_wifi_trial_pending();
  g_recovery->view.error = ret;
  pthread_mutex_unlock(&g_recovery_lock);
  return ret;
}

static void recovery_maintain(void)
{
  struct c6_desktop_link link = {0};
  struct dhcpd_runtime_status_s dhcp = {0};
  int ret = g_c6_desktop_backend.status(&link);
  pthread_mutex_lock(&g_recovery_lock);
  struct recovery_s *s = g_recovery;
  bool joining = s->view.joining;
  bool finalized = s->trial_finalized;
  bool starting = s->view.state == GLASS_RECOVERY_STARTING && s->dhcp_started;
  bool ready = s->view.state == GLASS_RECOVERY_READY;
  uint64_t join_deadline = s->join_deadline, dhcp_deadline = s->dhcp_deadline;
  uint64_t generation = s->trial_generation;
  pthread_mutex_unlock(&g_recovery_lock);

  struct glass_ethernet_s ethernet = {0};
  glass_ethernet_maintain(ready, &ethernet);
  pthread_mutex_lock(&g_recovery_lock);
  s->view.ethernet = ethernet;
  pthread_mutex_unlock(&g_recovery_lock);

#ifdef CONFIG_NET_NAT44
  if (ready && !joining && ethernet.error == -EADDRINUSE)
    {
      /* A newly leased upstream can overlap an already-running AP. Restart
       * the same SSID/key on a disjoint subnet so clients renegotiate DHCP.
       * Never relocate or discard user credentials/configuration files.
       */
      char ssid[33], password[13];
      pthread_mutex_lock(&g_recovery_lock);
      memcpy(ssid, s->view.ssid, sizeof(ssid));
      memcpy(password, s->password, sizeof(password));
      s->view.state = GLASS_RECOVERY_STOPPING;
      pthread_mutex_unlock(&g_recovery_lock);
      int stopped = recovery_stop_worker();
      if (!stopped)
        {
          pthread_mutex_lock(&g_recovery_lock);
          memcpy(s->view.ssid, ssid, sizeof(ssid));
          memcpy(s->password, password, sizeof(password));
          s->view.state = GLASS_RECOVERY_STARTING;
          pthread_mutex_unlock(&g_recovery_lock);
          (void)recovery_start_worker(ssid, password);
        }
      recovery_wipe(password, sizeof(password));
      return;
    }
#endif

  if (joining && !finalized && c6_wifi_trial_pending())
    {
      bool valid_ip = !ret && link.generation >= generation &&
                      link.ipv4_ready && glass_wifi_lease_ready(link.generation);
      if (valid_ip || recovery_seconds() >= join_deadline)
        {
          pthread_mutex_lock(&g_recovery_lock);
          s->trial_finalized = true;
          pthread_mutex_unlock(&g_recovery_lock);
          int finished = c6net_trial_finish(valid_ip);
          pthread_mutex_lock(&g_recovery_lock);
          s->view.joining = c6_wifi_trial_pending();
          s->view.error = finished ? finished : valid_ip ? 0 : -ETIMEDOUT;
          pthread_mutex_unlock(&g_recovery_lock);
        }
    }

  if (starting || ready)
    {
      int status = dhcpd_runtime_status(&dhcp);
      if (!status && dhcp.ready && dhcp.running)
        {
          pthread_mutex_lock(&g_recovery_lock);
          s->view.dhcp_ready = true;
          s->view.state = GLASS_RECOVERY_READY;
          pthread_mutex_unlock(&g_recovery_lock);
        }
      else if (ready || status || dhcp.last_error || recovery_seconds() >= dhcp_deadline)
        {
          int failure = status ? status : dhcp.last_error ? dhcp.last_error : -ETIMEDOUT;
          recovery_stop_worker();
          pthread_mutex_lock(&g_recovery_lock);
          s->view.ap_error = failure;
          s->view.state = GLASS_RECOVERY_ERROR;
          pthread_mutex_unlock(&g_recovery_lock);
        }
    }
  /* A finish/rollback can itself dispatch association events. Publish a
   * fresh generation after that RPC, never the pre-commit IP snapshot.
   */
  ret = g_c6_desktop_backend.status(&link);
  pthread_mutex_lock(&g_recovery_lock);
  if (!ret) s->link = link;
  else memset(&s->link, 0, sizeof(s->link));
  bool query_phy = s->view.state == GLASS_RECOVERY_READY &&
                   !s->view.joining;
  pthread_mutex_unlock(&g_recovery_lock);
  if (query_phy && recovery_seconds() >= s->phy_deadline)
    {
      (void)c6_softap_phy_refresh();
      s->phy_deadline = recovery_seconds() + 15;
    }
}

int glass_network_recovery_init(void)
{
  pthread_mutex_lock(&g_recovery_lock);
  if (g_recovery) { pthread_mutex_unlock(&g_recovery_lock); return 0; }
  struct recovery_s *s = calloc(1, sizeof(*s));
  if (!s) { pthread_mutex_unlock(&g_recovery_lock); return -ENOMEM; }
  struct c6_desktop_backend backend = g_c6_desktop_backend;
  backend.connect = recovery_connect_worker;
  backend.disconnect = recovery_disconnect_worker;
  backend.softap_start = recovery_start_worker;
  backend.softap_stop = recovery_stop_worker;
  backend.maintain = recovery_maintain;
  g_recovery = s;
  int ret = c6_desktop_worker_start(&s->worker, &backend);
  if (ret) { g_recovery = NULL; free(s); }
  pthread_mutex_unlock(&g_recovery_lock);
  return ret;
}

int glass_wifi_service_submit(enum c6_desktop_operation operation,
                               const char *ssid, const char *password)
{
  if (operation == C6_DESKTOP_SOFTAP_START || operation == C6_DESKTOP_SOFTAP_STOP)
    return -EINVAL;
  if (operation == C6_DESKTOP_CONNECT)
    {
      int valid = recovery_credentials_valid(ssid, password);
      if (valid) return valid;
    }
  int ret = glass_network_recovery_init();
  if (ret) return ret;
  pthread_mutex_lock(&g_recovery_lock);
  struct recovery_s *s = g_recovery;
  if (s->view.state == GLASS_RECOVERY_STARTING || s->view.state == GLASS_RECOVERY_STOPPING ||
      (s->view.joining && operation != C6_DESKTOP_STATUS && operation != C6_DESKTOP_DISCONNECT))
    ret = -EBUSY;
  else
    {
      ret = c6_desktop_worker_submit(&s->worker, operation, ssid, password);
      if (!ret && operation == C6_DESKTOP_CONNECT)
        {
          s->view.joining = true; s->view.error = 0;
          s->trial_finalized = false;
          memset(&s->link, 0, sizeof(s->link));
        }
      if (!ret && operation == C6_DESKTOP_DISCONNECT) s->view.error = 0;
    }
  pthread_mutex_unlock(&g_recovery_lock);
  return ret;
}

int glass_wifi_service_read(struct c6_desktop_result *out)
{
  if (!out) return -EINVAL;
  pthread_mutex_lock(&g_recovery_lock);
  if (!g_recovery) { memset(out, 0, sizeof(*out)); pthread_mutex_unlock(&g_recovery_lock); return 0; }
  int ret = c6_desktop_worker_read(&g_recovery->worker, out);
  if (!ret && out->operation == C6_DESKTOP_STATUS)
    {
      if (g_recovery->view.joining) out->link.ipv4_ready = false;
      if (g_recovery->view.error) out->error = g_recovery->view.error;
    }
  pthread_mutex_unlock(&g_recovery_lock);
  return ret;
}

int glass_network_recovery_start(void)
{
  uint8_t random[15];
  const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  int ret = glass_network_recovery_init();
  if (ret) return ret;
  ret = recovery_random(random, sizeof(random));
  if (ret) return ret;
  pthread_mutex_lock(&g_recovery_lock);
  struct recovery_s *s = g_recovery;
  if (s->view.ap_running || s->view.joining ||
      s->view.state == GLASS_RECOVERY_STARTING || s->view.state == GLASS_RECOVERY_STOPPING)
    ret = -EBUSY;
  else
    {
      char ssid[33], password[13];
      snprintf(ssid, sizeof(ssid), "ESPClaw-%02X%02X%02X", random[0], random[1], random[2]);
      for (unsigned i = 0; i < 12; i++) password[i] = alphabet[random[i + 3] & 31];
      password[12] = 0;
      ret = c6_desktop_worker_submit(&s->worker, C6_DESKTOP_SOFTAP_START, ssid, password);
      if (!ret)
        {
          strcpy(s->view.ssid, ssid); strcpy(s->password, password);
          s->view.state = GLASS_RECOVERY_STARTING;
          s->view.ap_error = s->view.cleanup_error = 0;
        }
      recovery_wipe(password, sizeof(password));
    }
  pthread_mutex_unlock(&g_recovery_lock);
  recovery_wipe(random, sizeof(random));
  return ret;
}

int glass_network_recovery_stop(void)
{
  pthread_mutex_lock(&g_recovery_lock);
  int ret = 0;
  if (g_recovery)
    {
      ret = c6_desktop_worker_submit(&g_recovery->worker, C6_DESKTOP_SOFTAP_STOP, NULL, NULL);
      if (!ret) g_recovery->view.state = GLASS_RECOVERY_STOPPING;
    }
  pthread_mutex_unlock(&g_recovery_lock);
  return ret;
}

int glass_network_recovery_read(struct glass_recovery_snapshot_s *out)
{
  if (!out) return -EINVAL;
  pthread_mutex_lock(&g_recovery_lock);
  memset(out, 0, sizeof(*out));
  if (g_recovery)
    {
      struct recovery_s *s = g_recovery;
      struct c6_desktop_result result;
      *out = s->view;
      out->associated = s->link.associated;
      out->ipv4_ready = s->link.ipv4_ready && !s->view.joining && !s->view.error &&
                        glass_wifi_lease_ready(s->link.generation);
      if (out->ipv4_ready) memcpy(out->ipv4, s->link.ipv4, 4);
      if (!c6_desktop_worker_read(&s->worker, &result))
        {
          out->sequence = result.sequence;
          out->operation = result.operation;
          out->busy = result.busy || out->joining ||
                      out->state == GLASS_RECOVERY_STARTING || out->state == GLASS_RECOVERY_STOPPING;
        }
    }
  pthread_mutex_unlock(&g_recovery_lock);
  return 0;
}

int glass_network_recovery_credentials(char *ssid, size_t ssid_size,
                                        char *password, size_t password_size)
{
  if (!ssid || !password || ssid_size < 33 || password_size < 13) return -EINVAL;
  pthread_mutex_lock(&g_recovery_lock);
  int ret = -ENOENT;
  if (g_recovery && g_recovery->view.ssid[0] && g_recovery->password[0])
    {
      strcpy(ssid, g_recovery->view.ssid);
      strcpy(password, g_recovery->password);
      ret = 0;
    }
  else { ssid[0] = password[0] = 0; }
  pthread_mutex_unlock(&g_recovery_lock);
  return ret;
}
