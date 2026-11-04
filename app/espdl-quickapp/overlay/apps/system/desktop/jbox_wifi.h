/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdint.h>
/* ESP-IDF protocomm Security 1, no PoP, as required by JBOX's physical
 * provisioning window. No Security 0 downgrade and no action retries.
 * Crypto/transport callbacks are private native boundaries, never JS/MCP. */
struct jbox_wifi_ops {
  void *context;
  int (*check)(void *); /* cancellation / absolute deadline */
  int (*keypair)(void *, uint8_t public_key[32]);
  int (*cipher_start)(void *, const uint8_t peer[32], const uint8_t iv[16]);
  int (*crypt)(void *, const uint8_t *, size_t, uint8_t *);
  int (*exchange)(void *, int session, const uint8_t *, size_t, uint8_t *,
                  size_t *);
  int (*pause)(void *, unsigned milliseconds);
};
/* Caller owns/wipes crypto state. Password and transient plaintext are erased
 * before return. ip is populated only after a connected status with IPv4. */
int jbox_wifi_provision(const struct jbox_wifi_ops *, const char *ssid,
                        const char *password, char ip[16]);
