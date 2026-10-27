/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#define GLASS_BLE_CHANNEL_BYTES 244
#define GLASS_BLE_CHANNEL_QUEUE 4
#define GLASS_BLE_CHANNEL_SERVICE "69d14e9d-5e2a-4564-8b03-7acf16a046bc"
#define GLASS_BLE_CHANNEL_RX "fe295b09-ba85-4b80-aec3-095121e8ae2f"
#define GLASS_BLE_CHANNEL_TX "9fc89159-b5c1-4797-8f7a-0f375f7e8138"
#define GLASS_BLE_CHANNEL_INFO "e86cb6ea-0566-4ea9-b513-1d94e99c9a54"

struct glass_ble_channel_state {
  bool ready,connected,encrypted,subscribed;
  uint32_t epoch,received,sent,rejected;
  uint16_t connection,mtu,payload;
  unsigned queued;
  int error;
};
/* Opaque application data only: no remote tool dispatch or device commands.
 * A successful send means accepted by NimBLE, not peer application readback.
 */
void glass_ble_channel_read(struct glass_ble_channel_state *state);
int glass_ble_channel_send(uint32_t epoch,const uint8_t *data,size_t length,
                           atomic_bool *cancel);
int glass_ble_channel_receive(uint32_t epoch,uint8_t *data,size_t *length);
int glass_ble_channel_clear(uint32_t epoch);

/* Persistent host group only. HID owns and publishes this shared link;
 * these functions never acquire HID's state lock or reserve another ACL. */
int glass_ble_channel_init(void);
void glass_ble_channel_update(uint16_t connection,uint32_t epoch,
                              bool connected,bool encrypted);
void glass_ble_channel_subscribe(uint16_t connection,uint16_t attribute,bool notify);
void glass_ble_channel_poll(void);
