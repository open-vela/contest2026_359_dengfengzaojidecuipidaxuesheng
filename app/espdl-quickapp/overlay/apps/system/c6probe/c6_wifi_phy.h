/****************************************************************************
 * apps/system/c6probe/c6_wifi_phy.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APPS_SYSTEM_C6PROBE_C6_WIFI_PHY_H
#define __APPS_SYSTEM_C6PROBE_C6_WIFI_PHY_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/compiler.h>
#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Matched C6 IDF 5.5 / Hosted 2.0.13 ABI, not ESP-AT bit numbering. */

#define C6_WIFI_PROTOCOL_BGN    0x07
#define C6_WIFI_PROTOCOL_AX     0x40
#define C6_WIFI_PROTOCOL_MASK   0x4f
#define C6_WIFI_BW20            1
#define C6_WIFI_BW40            2
#define C6_WIFI_PHY_HE20        6

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct c6_wifi_phy_config_s
{
  uint8_t protocol;
  uint8_t bandwidth;
};

struct c6_wifi_phy_snapshot_s
{
  uint32_t generation;
  bool preferred;
  bool configured;
  bool valid;
  bool clients_valid;
  bool sta_valid;
  struct c6_wifi_phy_config_s before_ap;
  struct c6_wifi_phy_config_s before_sta;
  struct c6_wifi_phy_config_s ap;
  uint8_t clients;
  uint8_t he_clients;
  uint8_t sta_phy;
  int query_error;
  int apply_error;
  int restore_error;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int esp_hosted_rpc_phy_get_protocol(int iface, FAR uint8_t *value);
int esp_hosted_rpc_phy_set_protocol(int iface, uint8_t value);
int esp_hosted_rpc_phy_get_bandwidth(int iface, FAR uint8_t *value);
int esp_hosted_rpc_phy_set_bandwidth(int iface, uint8_t value);
int esp_hosted_rpc_phy_clients(FAR uint8_t *count, FAR uint8_t *he_count);
int esp_hosted_rpc_phy_sta(FAR uint8_t *mode);
int esp_hosted_rpc_phy_storage_restore(void);

/* RAM policy only; changes accepted only while local AP is stopped/known.
 * Refresh belongs to the persistent recovery worker, never an RX callback.
 * Client HE capability is not proof of negotiated data PHY.
 */

int c6_softap_phy_prefer(bool enable);
int c6_softap_phy_refresh(void);
int c6_softap_phy_read(FAR struct c6_wifi_phy_snapshot_s *out);

#endif /* __APPS_SYSTEM_C6PROBE_C6_WIFI_PHY_H */
