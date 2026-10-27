/****************************************************************************
 * arch/risc-v/src/esp32p4/esp32p4_dma2d.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_ESP32P4_ESP32P4_DMA2D_H
#define __ARCH_RISCV_SRC_ESP32P4_ESP32P4_DMA2D_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/compiler.h>
#include <stdint.h>

/* The P4 JPEG reorder path and PPA use the same DMA2D channel and reset
 * domain. A lease covers configuration, DMA, cache completion and abort.
 * The callback runs with interrupts disabled and must never block. Clients
 * must stop their peripheral before releasing the lease. Release drains the
 * DMA interrupt and waits for any callback before another owner can enter.
 */

typedef void (*esp32p4_dma2d_callback_t)(FAR void *arg, uint32_t status);

int esp32p4_dma2d_acquire(esp32p4_dma2d_callback_t callback, FAR void *arg);
void esp32p4_dma2d_stop(void);
void esp32p4_dma2d_release(void);

#endif /* __ARCH_RISCV_SRC_ESP32P4_ESP32P4_DMA2D_H */
