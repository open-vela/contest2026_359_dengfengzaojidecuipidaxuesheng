/* SPDX-License-Identifier: Apache-2.0 */
#include "jbox_wifi_psa.h"
#include <errno.h>
#include <string.h>
static void wipe(void *p, size_t n) {
  volatile uint8_t *v = p;
  while (n--)
    *v++ = 0;
}
void jbox_wifi_crypto_init(struct jbox_wifi_crypto *c) {
  memset(c, 0, sizeof(*c));
  c->cipher = (psa_cipher_operation_t)PSA_CIPHER_OPERATION_INIT;
}
void jbox_wifi_crypto_free(struct jbox_wifi_crypto *c) {
  psa_cipher_abort(&c->cipher);
  psa_destroy_key(c->aes_key);
  psa_destroy_key(c->private_key);
  wipe(c, sizeof(*c));
}
int jbox_wifi_crypto_keypair(struct jbox_wifi_crypto *c,
                             uint8_t public_key[32]) {
  psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
  size_t n = 0;
  if (psa_crypto_init() != PSA_SUCCESS)
    return -ENOSYS;
  psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
  psa_set_key_bits(&a, 255);
  psa_set_key_usage_flags(&a, PSA_KEY_USAGE_DERIVE);
  psa_set_key_algorithm(&a, PSA_ALG_ECDH);
  psa_status_t r = psa_generate_key(&a, &c->private_key);
  psa_reset_key_attributes(&a);
  if (r != PSA_SUCCESS)
    return -EIO;
  r = psa_export_public_key(c->private_key, public_key, 32, &n);
  return r == PSA_SUCCESS && n == 32 ? 0 : -EIO;
}
int jbox_wifi_crypto_start(struct jbox_wifi_crypto *c, const uint8_t peer[32],
                           const uint8_t iv[16]) {
  uint8_t shared[32] = {0};
  size_t n = 0;
  psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
  psa_status_t r = psa_raw_key_agreement(PSA_ALG_ECDH, c->private_key, peer, 32,
                                         shared, sizeof(shared), &n);
  if (r != PSA_SUCCESS || n != 32) {
    wipe(shared, sizeof(shared));
    return -EACCES;
  }
  unsigned nonzero = 0;
  for (unsigned i = 0; i < 32; i++)
    nonzero |= shared[i];
  if (!nonzero) {
    wipe(shared, sizeof(shared));
    return -EACCES;
  }
  psa_set_key_type(&a, PSA_KEY_TYPE_AES);
  psa_set_key_bits(&a, 256);
  psa_set_key_usage_flags(&a, PSA_KEY_USAGE_ENCRYPT);
  psa_set_key_algorithm(&a, PSA_ALG_CTR);
  r = psa_import_key(&a, shared, 32, &c->aes_key);
  psa_reset_key_attributes(&a);
  wipe(shared, sizeof(shared));
  if (r != PSA_SUCCESS)
    return -EIO;
  r = psa_cipher_encrypt_setup(&c->cipher, c->aes_key, PSA_ALG_CTR);
  if (r == PSA_SUCCESS)
    r = psa_cipher_set_iv(&c->cipher, iv, 16);
  return r == PSA_SUCCESS ? 0 : -EIO;
}
int jbox_wifi_crypto_update(struct jbox_wifi_crypto *c, const uint8_t *in,
                            size_t n, uint8_t *out) {
  size_t written = 0;
  psa_status_t r = psa_cipher_update(&c->cipher, in, n, out, n, &written);
  return r == PSA_SUCCESS && written == n ? 0 : -EIO;
}
