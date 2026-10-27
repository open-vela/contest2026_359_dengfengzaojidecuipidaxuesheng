/* SPDX-License-Identifier: Apache-2.0 */

#ifndef __APPS_SYSTEM_C6PROBE_C6_OTA_H
#define __APPS_SYSTEM_C6PROBE_C6_OTA_H

#include <stddef.h>
#include <stdint.h>

/* Hosted 2.0.13 commits and schedules a reboot in OTAEnd. There is no
 * independent activation or idempotent write/abort operation in that ABI.
 */

#define C6_OTA_SLOT_SIZE (1536u * 1024u)
#define C6_OTA_CHUNK_SIZE 192u

enum c6_ota_operation_e
{
  C6_OTA_BEGIN,
  C6_OTA_WRITE,
  C6_OTA_END
};

struct c6_ota_result_s
{
  size_t acknowledged;
  int error;
  int begin_attempted;
  int commit_attempted;
  int committed;
};

typedef int (*c6_ota_exchange_t)(enum c6_ota_operation_e operation,
                                const uint8_t *data, size_t size, void *arg);

/* digest is supplied through a trusted maintenance console. It checks
 * integrity against that approved release; it is not a digital signature.
 * image must remain privately owned and unchanged until apply returns.
 */

int c6_ota_validate(const uint8_t *image, size_t size, const char *digest);
int c6_ota_transfer(const uint8_t *image, size_t size,
                    c6_ota_exchange_t exchange, void *arg,
                    struct c6_ota_result_s *result);
int esp_hosted_rpc_c6_ota(const uint8_t *image, size_t size,
                          struct c6_ota_result_s *result);
int c6_ota_command(int argc, char *argv[]);

#endif
