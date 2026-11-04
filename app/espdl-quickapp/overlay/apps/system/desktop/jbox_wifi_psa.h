/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <psa/crypto.h>
#include <stddef.h>
#include <stdint.h>
struct jbox_wifi_crypto {
  mbedtls_svc_key_id_t private_key, aes_key;
  psa_cipher_operation_t cipher;
};
void jbox_wifi_crypto_init(struct jbox_wifi_crypto *);
void jbox_wifi_crypto_free(struct jbox_wifi_crypto *);
int jbox_wifi_crypto_keypair(struct jbox_wifi_crypto *, uint8_t public_key[32]);
int jbox_wifi_crypto_start(struct jbox_wifi_crypto *, const uint8_t peer[32],
                           const uint8_t iv[16]);
int jbox_wifi_crypto_update(struct jbox_wifi_crypto *, const uint8_t *, size_t,
                            uint8_t *);
