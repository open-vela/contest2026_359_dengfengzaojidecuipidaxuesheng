/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define GLASS_STATUS_HEIGHT 32
#define GLASS_CONTENT_HEIGHT (600 - GLASS_STATUS_HEIGHT)

/* Link plus IPv4 lease, not a claim of Internet reachability. */
struct glass_status_network
{
  bool wifi, wired, joining;
  bool wired_link; /* Physical carrier; may still be waiting for IPv4. */
  bool rssi_valid;
  int rssi_dbm;
};
/* Unknown capacity is -1. Never estimates a battery percentage from uptime. */
void glass_status_power_read(int *percent, bool *charging);

/* Permanent desktop worker; UI reads only an atomic readiness snapshot. */
int glass_status_storage_start(void);
bool glass_status_storage_ready(void);
uint32_t glass_status_storage_epoch(void);
