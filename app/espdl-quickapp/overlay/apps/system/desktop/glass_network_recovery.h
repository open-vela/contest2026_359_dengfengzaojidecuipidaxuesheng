/* SPDX-License-Identifier: Apache-2.0 */

#ifndef GLASS_NETWORK_RECOVERY_H
#define GLASS_NETWORK_RECOVERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "desktop_worker.h"
#include "glass_ethernet.h"

enum glass_recovery_state_e
{
  GLASS_RECOVERY_OFF,
  GLASS_RECOVERY_STARTING,
  GLASS_RECOVERY_READY,
  GLASS_RECOVERY_STOPPING,
  GLASS_RECOVERY_ERROR
};

struct glass_recovery_snapshot_s
{
  enum glass_recovery_state_e state;
  bool ap_running;
  bool dhcp_ready;
  bool joining;
  bool busy;
  bool associated;
  bool ipv4_ready;
  int error;
  int ap_error;
  int cleanup_error;
  unsigned long sequence;
  unsigned operation;
  char ssid[33];
  uint8_t ap_ipv4[4];
  uint8_t ipv4[4];
  struct glass_ethernet_s ethernet;
};

int glass_network_recovery_init(void);
int glass_network_recovery_start(void);
int glass_network_recovery_stop(void);
int glass_network_recovery_read(struct glass_recovery_snapshot_s *out);
/* Only the device's local UI may request the ephemeral AP password.
 * HTTP status/config replies must never include this value.
 */
int glass_network_recovery_credentials(char *ssid, size_t ssid_size,
                                        char *password, size_t password_size);
int glass_wifi_service_submit(enum c6_desktop_operation operation,
                               const char *ssid, const char *password);
int glass_wifi_service_read(struct c6_desktop_result *out);
bool glass_wifi_lease_ready(uint64_t generation);
void glass_wifi_request_lease(void);

#endif
