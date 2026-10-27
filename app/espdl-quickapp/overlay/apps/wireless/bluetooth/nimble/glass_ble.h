/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "glass_ble_capacity_config.h"
/* Keep the existing desktop scanner ABI, but do not run a second host. */
#define GLASS_BLE_LIMIT 32
#define GLASS_BLE_NAME_MAX 63
struct glass_ble_device
{
  uint8_t type, address[6];
  int rssi;
  bool name_complete;
  bool connectable_known, connectable, bonded;
  uint16_t appearance, service_hint;
  bool service128;
  char name[GLASS_BLE_NAME_MAX + 1];
};
struct glass_ble_session
{
  uint32_t generation;
  uint16_t mtu;
  bool connecting, connected, encrypted, bonded, disconnecting, mouse_paused;
  int error;
  struct glass_ble_device peer;
};
/* Outcome of one device in the latest mouse-exit recovery batch.
 * connected is the observed outcome at completion, not a live link state. */
struct glass_ble_restore_result
{
  uint32_t generation;
  bool attempted, connected;
  int error;
};
struct glass_ble_state
{
  bool started, ready, scanning, connecting, connected;
  int error;
  int store_error;
  unsigned count;
  bool scan_truncated;
  struct glass_ble_device devices[GLASS_BLE_LIMIT];
  uint32_t generation,scan_generation;
  uint16_t mtu;
  bool encrypted,bonded;
  bool peer_valid,disconnecting,mouse_paused;
  struct glass_ble_device peer;
  unsigned session_count, connection_count, central_capacity;
  struct glass_ble_session sessions[GLASS_BLE_CENTRAL_LIMIT];
  bool restoring;
  unsigned restore_count;
  struct glass_ble_restore_result restores[GLASS_BLE_CENTRAL_LIMIT];
};
struct glass_ble_hid_state
{
  bool enabled, advertising, connected, encrypted, mouse_notify;
  bool suspended, boot_mode, host_ready;
  uint8_t buttons;
  uint32_t epoch;
  int error, ble_error;
  int store_error;
};
struct glass_ble_capacity_state
{
  unsigned capacity, reserved, central_reservations;
  bool central_reserved, hid_reserved;
};
int glass_ble_start(void);
unsigned glass_ble_capacity(void);
void glass_ble_capacity_read(struct glass_ble_capacity_state *state);
const char *glass_ble_identity_name(void);
const char *glass_ble_identity_address(void);
int glass_ble_scan(void);
int glass_ble_connect(unsigned index);
int glass_ble_cancel(void);
int glass_ble_disconnect(void);
/* Central-device snapshot only; HID has its own snapshot and lifecycle. */
void glass_ble_read(struct glass_ble_state *state);
int glass_ble_hid_start(void);
int glass_ble_hid_stop(void);
int glass_ble_hid_mouse(int buttons, int x, int y, int wheel);
int glass_ble_hid_click(unsigned button);
int glass_ble_hid_release(void);
void glass_ble_hid_read(struct glass_ble_hid_state *state);
struct glass_ble_hid_input_state
{
  uint32_t session, epoch, accepted, submitted, merged, retries, discarded;
  unsigned queued;
  bool releasing;
  uint16_t interval, latency, supervision_timeout;
  int parameter_result, parameter_status;
  unsigned parameter_attempts, parameter_updates;
  uint32_t disconnects, unexpected_disconnects, last_disconnect_epoch;
  int last_disconnect_reason;
  unsigned last_parameter_attempts, last_parameter_updates;
  uint16_t last_interval;
  uint64_t last_secure_age_ms;
};
/* An input session accepts owned, bounded copies. Success here means queued,
 * not a peer receipt. Closing/expired sessions never replay old input. */
int glass_ble_hid_input_begin(uint32_t epoch, uint32_t *session);
int glass_ble_hid_input_report(uint32_t session,int buttons,int x,int y,int wheel);
int glass_ble_hid_input_click(uint32_t session,unsigned button);
int glass_ble_hid_input_keepalive(uint32_t session);
int glass_ble_hid_input_end(uint32_t session);
void glass_ble_hid_input_read(struct glass_ble_hid_input_state *state);
