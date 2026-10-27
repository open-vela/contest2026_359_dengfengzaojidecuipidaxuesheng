/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
/* HID serializes state and publishes an owner-checked copy from its host
 * context. Observe the central's parameters; do not start automatic updates. */
struct glass_ble_hid_link
{
  uint16_t connection, interval, latency, timeout;
  uint32_t epoch;
  bool connected, secure, sampled;
  unsigned attempts;
  uint32_t changed;
  uint64_t secured_at;
  int result, status;
};
void glass_ble_hid_link_state(struct glass_ble_hid_link *,uint16_t,uint32_t,
                              bool,bool,uint64_t);
void glass_ble_hid_link_poll(struct glass_ble_hid_link *,uint64_t);
void glass_ble_hid_link_changed(struct glass_ble_hid_link *,uint16_t,int);
