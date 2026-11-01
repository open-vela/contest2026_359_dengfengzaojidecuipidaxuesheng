/* SPDX-License-Identifier: Apache-2.0 */
/* Host-only API double. Never copied to the native desktop include path. */
#ifndef TEST_ESP_SHA_H
#define TEST_ESP_SHA_H
#include <stdbool.h>
#include <stddef.h>
#include <openssl/sha.h>
#include <stdatomic.h>
struct esp_sha256_context_s {SHA256_CTX reference;};
extern int glv_test_sha_fault;
extern atomic_uint glv_test_sha_initializations;
int esp_sha_init(void);
void esp_sha256_init(struct esp_sha256_context_s *);
int esp_sha256_starts(struct esp_sha256_context_s *,bool);
int esp_sha256_update(struct esp_sha256_context_s *,const unsigned char *,size_t);
int esp_sha256_finish(struct esp_sha256_context_s *,unsigned char[32]);
#endif
