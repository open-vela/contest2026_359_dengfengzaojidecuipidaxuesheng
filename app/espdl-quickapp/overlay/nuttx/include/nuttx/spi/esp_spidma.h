/****************************************************************************
 * include/nuttx/spi/esp_spidma.h
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __INCLUDE_NUTTX_SPI_ESP_SPIDMA_H
#define __INCLUDE_NUTTX_SPI_ESP_SPIDMA_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct esp_spidma_stats_s
{
  uint32_t configured;
  uint32_t ready;
  uint32_t fault;
  uint32_t frequency;
  uint32_t transfers;
  uint32_t polls;
  uint32_t callbacks;
  uint32_t errors;
  uint32_t timeouts;
  uint32_t allocation_bytes;
  uint64_t tx_bytes;
  uint64_t rx_bytes;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Logical SPI port number: 2 or 3.  Does not initialize hardware. */

int esp_spidma_get_stats(int port, FAR struct esp_spidma_stats_s *stats);

#endif
