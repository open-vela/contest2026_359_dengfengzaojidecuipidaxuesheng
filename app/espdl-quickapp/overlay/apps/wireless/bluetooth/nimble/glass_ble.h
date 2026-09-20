/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Keep the existing desktop scanner ABI, but do not run a second host. */
#define GLASS_BLE_LIMIT 32
#define GLASS_BLE_NAME_MAX 63
struct glass_ble_device
{
  uint8_t type, address[6];
  int rssi;
  bool name_complete;
  char name[GLASS_BLE_NAME_MAX + 1];
};
struct glass_ble_state
{
  bool started, ready, scanning, connecting, connected;
  int error;
  unsigned count;
  struct glass_ble_device devices[GLASS_BLE_LIMIT];
};
struct glass_ble_hid_state
{
  bool enabled, advertising, connected, encrypted, mouse_notify;
  bool suspended, boot_mode, host_ready;
  uint8_t buttons;
  uint32_t epoch;
  int error, ble_error;
};
int glass_ble_start(void);
int glass_ble_scan(void);
int glass_ble_connect(unsigned index);
int glass_ble_cancel(void);
int glass_ble_disconnect(void);
void glass_ble_read(struct glass_ble_state *state);
int glass_ble_hid_start(void);
int glass_ble_hid_stop(void);
int glass_ble_hid_mouse(int buttons, int x, int y, int wheel);
int glass_ble_hid_click(unsigned button);
int glass_ble_hid_release(void);
void glass_ble_hid_read(struct glass_ble_hid_state *state);
