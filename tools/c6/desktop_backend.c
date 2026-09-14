/* SPDX-License-Identifier: Apache-2.0 */
#include "desktop_backend.h"
#include "c6net.h"
#include "link_state.h"
#include <arpa/inet.h>
#include <netutils/netlib.h>

static int scan(struct c6_scan_ap *records, size_t capacity, size_t *count)
{
  struct c6_link_snapshot state;
  if (!count) return -EINVAL;
  *count = 0;
  if (!records || !capacity) return -EINVAL;
  int ret = c6net_get_link_snapshot(&state);
  if (ret < 0) return ret;
  if (!state.initialized)
    {
      ret = c6net_prepare();
      if (ret < 0) return ret;
    }
  return esp_hosted_rpc_wifi_scan_results(records, capacity, count);
}

/* Clear address first: a later partial failure must not leave a usable old
 * address associated with another AP. Never clear the global DNS list here. */
static int clear_ipv4(void)
{
  const struct in_addr zero = {0};
  if (netlib_set_ipv4addr("eth0", &zero) < 0 ||
      netlib_set_dripv4addr("eth0", &zero) < 0 ||
      netlib_set_ipv4netmask("eth0", &zero) < 0)
    return -(errno ? errno : EIO);
  return 0;
}

static int connect_ap(const char *ssid, const char *password)
{
  if (!ssid || !password) return -EINVAL;
  size_t length = strnlen(ssid, 33);
  if (!length || length > 32 || strnlen(password, 65) > 64) return -EINVAL;
  return c6net_connect(ssid, password);
}

static int disconnect_ap(void)
{
  int ret = c6net_disconnect();
  if (ret < 0) return ret;
  return clear_ipv4();
}

static int status(struct c6_desktop_link *link)
{
  struct c6_link_snapshot state;
  if (!link) return -EINVAL;
  memset(link, 0, sizeof(*link));
  int ret = c6net_get_link_snapshot(&state);
  if (ret < 0) return ret;
  link->initialized = state.initialized;
  link->associated = state.associated;
  link->carrier_ready = state.carrier_ready;
  if (state.carrier_ready)
    {
      struct in_addr address;
      struct c6_link_snapshot after;
      ret = netlib_get_ipv4addr("eth0", &address);
      if (ret < 0)
        {
          memset(link, 0, sizeof(*link));
          return -(errno ? errno : EIO);
        }
      ret = c6net_get_link_snapshot(&after);
      if (ret < 0)
        {
          memset(link, 0, sizeof(*link));
          return ret;
        }
      if (after.generation != state.generation || !after.carrier_ready)
        {
          memset(link, 0, sizeof(*link));
          return -EAGAIN;
        }
      uint32_t host = ntohl(address.s_addr);
      if (host != 0 && host != UINT32_MAX && (host >> 24) != 127 &&
          (host >> 28) < 14)
        {
          memcpy(link->ipv4, &address.s_addr, sizeof(link->ipv4));
          link->ipv4_ready = true;
        }
    }
  return 0;
}

const struct c6_desktop_backend g_c6_desktop_backend = {
  .scan = scan,
  .connect = connect_ap,
  .status = status,
  .disconnect = disconnect_ap
};
