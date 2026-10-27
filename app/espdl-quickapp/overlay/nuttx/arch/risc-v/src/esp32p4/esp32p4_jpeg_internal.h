/****************************************************************************
 * arch/risc-v/src/esp32p4/esp32p4_jpeg_internal.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_ESP32P4_ESP32P4_JPEG_INTERNAL_H
#define __ARCH_RISCV_SRC_ESP32P4_ESP32P4_JPEG_INTERNAL_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/compiler.h>
#include <stddef.h>
#include <stdint.h>

#define P4_JPEG_MAX_STREAM (4 * 1024 * 1024)
#define P4_JPEG_MAX_PIXELS (4 * 1024 * 1024)
#define P4_JPEG_MAX_DIMENSION 4096

/* CPU-only metadata belongs in the user/PSRAM heap on the flat P4 build.
 * Neither this structure nor the Huffman scratch array is a DMA descriptor.
 */

struct p4_jpeg_header_s
{
  uint32_t quant[3][64];
  uint8_t bits[2][2][16];
  uint8_t values[2][2][256];
  uint16_t width;
  uint16_t height;
  uint16_t padded_width;
  uint16_t padded_height;
  uint16_t restart_interval;
  uint8_t components;
  uint8_t horizontal[3];
  uint8_t vertical[3];
  uint8_t identifier[3];
  uint8_t quant_id[3];
  uint8_t dc_id[3];
  uint8_t ac_id[3];
  uint8_t quant_mask;
  uint8_t dc_mask;
  uint8_t ac_mask;
  int8_t adobe_transform;
  size_t scan_offset;
  size_t scan_length;
};

int esp32p4_jpeg_parse(FAR const uint8_t *data, size_t length,
                      FAR struct p4_jpeg_header_s *header);

#endif /* __ARCH_RISCV_SRC_ESP32P4_ESP32P4_JPEG_INTERNAL_H */
