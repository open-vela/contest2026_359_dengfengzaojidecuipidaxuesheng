/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdint.h>
/* Try a complete, full-resolution RGB565 frame. The private DMA buffer is
 * copied to the caller only after successful completion; errors permit the
 * existing software decoder to run. No active framebuffer is used for DMA.
 */
int qpk_jpeg_hw_decode(const uint8_t *input, size_t size, uint16_t *output,
                       unsigned width, unsigned height, unsigned stride);
/* Decode to private RGB565, then PPA-downscale by an exact uniform 1/16
 * ratio. Commit only on success; unsupported geometry permits software.
 */
int qpk_jpeg_hw_decode_scaled(const uint8_t *input, size_t size,
                              uint16_t *output, unsigned width,
                              unsigned height, unsigned out_width,
                              unsigned out_height, unsigned stride);
int qpk_jpeg_hw_selftest(void);
/* Encode an owned RGB565 frame; returns a malloc-owned complete JPEG only
 * on success. No file I/O, no automatic retries or software format fallback.
 */
int qpk_jpeg_hw_encode565(const uint16_t *input, unsigned width,
                          unsigned height, unsigned stride, unsigned quality,
                          uint8_t **output, size_t *size);
int qpk_jpeg_encode_selftest(void);
/* Local SD photo verification; emits only geometry and a pixel fingerprint. */
int qpk_jpeg_file_selftest(const char *path);
