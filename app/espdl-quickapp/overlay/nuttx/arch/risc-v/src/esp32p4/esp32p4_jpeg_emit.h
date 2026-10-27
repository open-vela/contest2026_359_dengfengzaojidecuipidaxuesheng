/****************************************************************************
 * arch/risc-v/src/esp32p4/esp32p4_jpeg_emit.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_ESP32P4_ESP32P4_JPEG_EMIT_H
#define __ARCH_RISCV_SRC_ESP32P4_ESP32P4_JPEG_EMIT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/video/p4_jpeg.h>

int esp32p4_jpeg_emit(FAR const struct p4_jpeg_encode_s *job,
                     FAR uint32_t quant[2][64], FAR size_t *length);

#endif /* __ARCH_RISCV_SRC_ESP32P4_ESP32P4_JPEG_EMIT_H */
