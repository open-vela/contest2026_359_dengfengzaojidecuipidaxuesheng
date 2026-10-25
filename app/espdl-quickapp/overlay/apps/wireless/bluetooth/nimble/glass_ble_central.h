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
int glass_ble_central_connect(unsigned index,uint32_t scan_generation);
int glass_ble_central_cancel(void);
int glass_ble_central_disconnect(void);
int glass_ble_gatt(const struct glass_gatt_request *,struct glass_gatt_result *,atomic_bool *cancel);
int glass_ble_notification(uint32_t generation,struct glass_gatt_result *);
/* One controller connection: central and the HID peripheral cannot steal it. */
int glass_ble_role_claim(unsigned role);
void glass_ble_role_release(unsigned role);
