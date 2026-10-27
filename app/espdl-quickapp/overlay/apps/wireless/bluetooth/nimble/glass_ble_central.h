/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
struct glass_ble_state;
#define GLASS_GATT_ITEMS 32
#define GLASS_GATT_BYTES 512
enum glass_gatt_operation { GLASS_GATT_SERVICES,GLASS_GATT_CHARACTERISTICS,
  GLASS_GATT_DESCRIPTORS,GLASS_GATT_READ,GLASS_GATT_WRITE,
  GLASS_GATT_SUBSCRIBE,GLASS_GATT_PAIR,GLASS_GATT_MTU };
struct glass_gatt_request {
  enum glass_gatt_operation operation;
  uint32_t generation;
  uint16_t start,end,handle;
  bool response;
  size_t length;
  uint8_t data[GLASS_GATT_BYTES];
};
struct glass_gatt_item { uint16_t start,end,value;uint8_t properties;char uuid[37]; };
struct glass_gatt_result {
  uint32_t generation,dropped;
  uint16_t handle,mtu;
  unsigned count;
  bool truncated,indication;
  size_t length;
  struct glass_gatt_item items[GLASS_GATT_ITEMS];
  uint8_t data[GLASS_GATT_BYTES];
};
int glass_ble_central_init(void);
/* Persistent host task group only; sleep until a mailbox command arrives. */
void glass_ble_central_wait(void);
void glass_ble_central_poll(void);
void glass_ble_central_reset(void);
void glass_ble_central_read(struct glass_ble_state *state);
int glass_ble_central_scan(void);
int glass_ble_central_scan_service(const uint8_t service[16]);
int glass_ble_central_scan_session(const uint8_t service[16],uint32_t *generation);
/* Stops discovery only; preserves the peer, pending GATT and HID. Zero
 * selects the current scan; a nonzero generation rejects stale UI work.
 */
int glass_ble_central_scan_stop(uint32_t scan_generation);
int glass_ble_central_connect(unsigned index,uint32_t scan_generation);
/* Admission returns a non-reused session generation, valid while connecting.
 * GATT, notifications, selection and disconnect address that exact session. */
int glass_ble_central_connect_session(unsigned index,uint32_t scan_generation,
                                      uint32_t *generation);
int glass_ble_central_select(uint32_t generation);
int glass_ble_central_cancel(void);
int glass_ble_central_disconnect(void);
int glass_ble_central_disconnect_generation(uint32_t generation);
/* Caller must be outside the host command/event threads. Pause preserves
 * logical session ownership, cancels operations and waits for physical idle.
 * Resume makes one connection attempt; it never replays GATT requests.
 */
int glass_ble_central_mouse_pause(void);
int glass_ble_central_mouse_resume(void);
int glass_ble_gatt(const struct glass_gatt_request *,struct glass_gatt_result *,atomic_bool *cancel);
int glass_ble_notification(uint32_t generation,struct glass_gatt_result *);
/* Ordinary sessions and HID use mutually exclusive application roles.
 * Caller holds the owner's state lock through claim/release. A reservation
 * includes pending/closing connections; release only on confirmed idle.
 */
#define GLASS_BLE_ROLE_CENTRAL 1u
#define GLASS_BLE_ROLE_HID     2u
int glass_ble_role_claim(unsigned role);
void glass_ble_role_release(unsigned role);

/* Set the central owner's total reservation, under its state lock. */
int glass_ble_central_reserve(unsigned count);
