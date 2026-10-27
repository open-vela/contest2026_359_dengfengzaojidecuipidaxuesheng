/****************************************************************************
 * apps/system/c6probe/c6_softap.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APPS_SYSTEM_C6PROBE_C6_SOFTAP_H
#define __APPS_SYSTEM_C6PROBE_C6_SOFTAP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/compiler.h>

#include <stdbool.h>
#include <stdint.h>
#include "c6_wifi_phy.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define C6_SOFTAP_IFNAME        "ap0"
#define C6_SOFTAP_IFNAME_FORMAT "ap%d"
#define C6_SOFTAP_SSID_MAX      32
#define C6_SOFTAP_PASSWORD_MIN  8
#define C6_SOFTAP_PASSWORD_MAX  63

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct c6_softap_snapshot_s
{
  bool active;
  bool state_unknown;
  bool sta_interrupted;
  int error;
  int restore_error;
  char ifname[16];
  char ssid[C6_SOFTAP_SSID_MAX + 1];
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Blocking operations belong to the existing desktop network worker.
 * Start temporarily stops Wi-Fi to avoid broadcasting a previous/default AP
 * configuration.  The transport and Bluetooth are not reset.  STA credentials
 * and the explicit disconnect policy are retained.  AP and STA have separate
 * Ethernet interfaces; this layer starts neither DHCP nor a portal.
 */

int c6_softap_start(FAR const char *ssid, FAR const char *password);
/* Confirm the remote mode even when this P4 has no local AP object. Removing
 * only its AP bit does not initialize/reset the transport or start/connect
 * STA. An unknown remote state is an error, not a confirmed stopped AP.
 */
int c6_softap_stop(void);
int c6_softap_status(FAR struct c6_softap_snapshot_s *out);

/* Prepared RPC hooks; never call synchronous RPC from RX callbacks. */

int c6_softap_preserve_mode(int mode);
int esp_hosted_rpc_wifi_stop(void);
int esp_hosted_rpc_wifi_get_mode(FAR int *mode);
int esp_hosted_rpc_softap_configure(FAR const char *ssid,
                                  FAR const char *password);
int esp_hosted_rpc_softap_verify(FAR const char *ssid,
                               FAR const char *password);

#endif /* __APPS_SYSTEM_C6PROBE_C6_SOFTAP_H */
