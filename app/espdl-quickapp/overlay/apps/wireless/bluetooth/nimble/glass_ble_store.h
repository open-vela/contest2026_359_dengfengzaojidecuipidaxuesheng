/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
void glass_ble_store_init(void);
bool glass_ble_store_has_bond(void);
bool glass_ble_store_has_hid_bond(void);
int glass_ble_store_mark_hid_peer(uint8_t type, const uint8_t address[6]);
int glass_ble_store_last_error(void);
