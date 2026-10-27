/* SPDX-License-Identifier: Apache-2.0 */
#ifndef __APPS_SYSTEM_C6PROBE_C6_RX_BURST_H
#define __APPS_SYSTEM_C6PROBE_C6_RX_BURST_H

/* Bounded diagnostic control; no radio reset, RPC or persistent settings. */

#define C6_RX_DMA_BLOCKS 4u
int esp_hosted_rx_block_limit(unsigned int blocks);

#endif
