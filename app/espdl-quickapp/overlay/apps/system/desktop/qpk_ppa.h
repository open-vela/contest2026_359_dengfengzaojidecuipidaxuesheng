/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>

/* Exact uniform downscale only, in 1/16 steps. Zero means unsupported.
 * Limits match the board PPA driver; strides below are measured in pixels.
 */
unsigned qpk_ppa_rgb565_scale256(unsigned width, unsigned height,
                                unsigned out_width, unsigned out_height);

/* Copy source rows into private aligned RAM, then commit to caller output
 * only after synchronous DMA success. Never DMA into caller/framebuffer RAM.
 * Source/output may alias because input is snapshotted before output commit.
 */
int qpk_ppa_rgb565_scale(const uint16_t *input, unsigned width,
                         unsigned height, unsigned stride, uint16_t *output,
                         unsigned out_width, unsigned out_height,
                         unsigned out_stride);
