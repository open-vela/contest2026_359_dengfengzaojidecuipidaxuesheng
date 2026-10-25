/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
int glass_ble_hid_service_init(void);
void glass_ble_hid_synced(unsigned char address_type);
void glass_ble_hid_reset(int reason);
int glass_ble_host_wait(void);
void glass_ble_central_poll(void); /* Only a persistent host-group pthread. */
void glass_ble_central_hid_wake(void);
void glass_ble_hid_poll(void); /* Only the persistent host task group. */
