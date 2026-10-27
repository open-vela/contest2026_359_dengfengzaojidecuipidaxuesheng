/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "c6_ota.h"

/* Reuse the SHA-256 implementation already linked by the desktop. */

void qpk_dl_sha256(const void *data, size_t length, uint8_t digest[32]);

static uint32_t c6_ota_le32(const uint8_t *p)
{
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
         (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int c6_ota_hex(unsigned char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int c6_ota_validate(const uint8_t *image, size_t size, const char *digest)
{
  uint8_t actual[32];
  size_t offset = 24;
  unsigned checksum = 0xef;
  unsigned difference = 0;

  if (image == NULL || digest == NULL || strlen(digest) != 64 ||
      size < 64 || size > C6_OTA_SLOT_SIZE)
    {
      return -EINVAL;
    }

  /* ESP image chip_id 13 is ESP32-C6. Require the appended image hash,
   * ordinary DIO/QIO/etc. image, and a bounded nonempty segment list.
   * P4 Simple Boot images and merged Flash backups are rejected here.
   */

  if (image[0] != 0xe9 || image[1] == 0 || image[1] > 16 ||
      image[2] > 3 || image[12] != 13 || image[13] != 0 || image[23] != 1)
    {
      return -ENOEXEC;
    }

  for (unsigned i = 0; i < image[1]; i++)
    {
      size_t length;

      if (size - offset < 8) return -ENOEXEC;
      length = c6_ota_le32(image + offset + 4);
      offset += 8;
      if (length > size - offset) return -ENOEXEC;
      for (size_t j = 0; j < length; j++) checksum ^= image[offset + j];
      offset += length;
    }

  /* ESP checksum occupies the last byte of the next 16-byte boundary. */

  offset = (offset + 16) & ~(size_t)15;
  if (offset > size || size - offset != 32 || image[offset - 1] != checksum)
    {
      return -ENOEXEC;
    }

  qpk_dl_sha256(image, offset, actual);
  if (memcmp(actual, image + offset, 32) != 0) return -EBADMSG;

  qpk_dl_sha256(image, size, actual);
  for (unsigned i = 0; i < 32; i++)
    {
      int high = c6_ota_hex((unsigned char)digest[i * 2]);
      int low = c6_ota_hex((unsigned char)digest[i * 2 + 1]);

      if (high < 0 || low < 0) return -EINVAL;
      difference |= actual[i] ^ (unsigned)(high * 16 + low);
    }

  return difference == 0 ? 0 : -EBADMSG;
}

int c6_ota_transfer(const uint8_t *image, size_t size,
                    c6_ota_exchange_t exchange, void *arg,
                    struct c6_ota_result_s *result)
{
  int ret;

  if (result == NULL) return -EINVAL;
  memset(result, 0, sizeof(*result));
  if (image == NULL || size == 0 || size > C6_OTA_SLOT_SIZE || exchange == NULL)
    {
      result->error = -EINVAL;
      return result->error;
    }

  result->begin_attempted = 1;
  ret = exchange(C6_OTA_BEGIN, NULL, 0, arg);
  while (ret == 0 && result->acknowledged < size)
    {
      size_t count = size - result->acknowledged;
      if (count > C6_OTA_CHUNK_SIZE) count = C6_OTA_CHUNK_SIZE;
      ret = exchange(C6_OTA_WRITE, image + result->acknowledged, count, arg);
      if (ret == 0) result->acknowledged += count;
    }

  if (ret == 0)
    {
      result->commit_attempted = 1;
      ret = exchange(C6_OTA_END, NULL, 0, arg);
      if (ret == 0) result->committed = 1;
    }

  /* Never retry a timed-out write or OTAEnd: the companion may already
   * have consumed it. A reset/recovery is required before another attempt.
   */

  result->error = ret;
  return ret;
}
