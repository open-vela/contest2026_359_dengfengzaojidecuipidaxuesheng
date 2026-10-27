/****************************************************************************
 * apps/system/c6probe/c6net.h
 ****************************************************************************/

#ifndef __APPS_SYSTEM_C6PROBE_C6NET_H
#define __APPS_SYSTEM_C6PROBE_C6NET_H

#include <nuttx/config.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <nuttx/compiler.h>

#define C6NET_IFNAME "wlan0"
#define C6NET_IFNAME_FORMAT "wlan%d"

/* Per-boot, modulo-2^32 counters. Individual fields are atomic; a snapshot
 * does not stop traffic or promise that all fields describe one instant.
 * TX success means acceptance by Hosted, not over-the-air delivery.
 */
struct c6net_stats_s
{
  uint32_t tx_notifications, tx_polls, tx_packets, tx_bytes, tx_errors;
  uint32_t tx_down_drops, tx_send_max_us;
  uint32_t rx_packets, rx_bytes, rx_invalid, rx_down_drops, rx_unsupported;
  int tx_last_error;
};

#if defined(CONFIG_NET) && defined(CONFIG_SYSTEM_C6PROBE)
int c6net_initialize(FAR const char *ssid, FAR const char *password);
int c6net_connect(FAR const char *ssid, FAR const char *password);
int c6net_disconnect(void);
int c6net_reconnect(void);
int c6net_prepare(void);
/* Runtime scheduling control for paired tests; does not reconnect or save. */
int c6net_tx_wake(bool enable);
int c6net_trial_connect(FAR const char *ssid, FAR const char *password);
int c6net_trial_finish(bool commit);
struct c6_link_snapshot;
int c6net_get_link_snapshot(struct c6_link_snapshot *out);
bool c6net_is_initialized(void);
bool c6net_is_associated(void);
/* Read P4 counters only; no radio initialization, RPC, reset or bus access. */
int c6net_stats_read(bool softap, FAR struct c6net_stats_s *out);
#else
static inline int c6net_tx_wake(bool enable)
{
  (void)enable;
  return -ENOSYS;
}

static inline int c6net_stats_read(bool softap,
                                 FAR struct c6net_stats_s *out)
{
  (void)softap;
  (void)out;
  return -ENOSYS;
}
static inline int c6net_initialize(FAR const char *ssid,
  FAR const char *password)
{
  (void)ssid;
  (void)password;
  return -ENOSYS;
}

static inline int c6net_connect(FAR const char *ssid,
  FAR const char *password)
{
  (void)ssid;
  (void)password;
  return -ENOSYS;
}

static inline bool c6net_is_initialized(void)
{
  return false;
}

static inline bool c6net_is_associated(void)
{
  return false;
}
#endif

#endif
