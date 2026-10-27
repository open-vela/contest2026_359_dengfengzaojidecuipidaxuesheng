/****************************************************************************
 * include/nuttx/video/p4_jpeg.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __INCLUDE_NUTTX_VIDEO_P4_JPEG_H
#define __INCLUDE_NUTTX_VIDEO_P4_JPEG_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/compiler.h>
#include <nuttx/fs/ioctl.h>
#include <stddef.h>
#include <stdint.h>

#define P4_JPEG_INFO   _IOC(0x7f00, 2)
#define P4_JPEG_DECODE _IOC(0x7f00, 3)
#define P4_JPEG_ENCODE _IOC(0x7f00, 4)

enum p4_jpeg_format_e
{
  P4_JPEG_RGB565 = 0,             /* Native little-endian RGB565. */
  P4_JPEG_RGB888,                 /* R, G, B byte order. */
  P4_JPEG_GRAY8
};

/* Board-private synchronous flat-build API. The caller retains exclusive
 * input/output ownership until ioctl returns. Input must be readable RAM;
 * output must be RAM with 128-byte aligned address and allocated capacity.
 * Output includes MCU padding. Stride describes padded rows, while width
 * and height describe the visible image. No framebuffer is accessed here.
 */

struct p4_jpeg_info_s
{
  FAR const uint8_t *input;
  size_t input_size;
  uint16_t width;
  uint16_t height;
  uint16_t padded_width;
  uint16_t padded_height;
  uint8_t components;
};

struct p4_jpeg_decode_s
{
  struct p4_jpeg_info_s info;
  FAR uint8_t *output;
  size_t output_capacity;
  size_t output_size;
  uint32_t stride;
  enum p4_jpeg_format_e format;
  uint32_t timeout_ms;            /* 1..5000; zero selects 250 ms. */
};

enum p4_jpeg_input_e
{
  P4_JPEG_INPUT_RGB565,           /* Native little-endian RGB565. */
  P4_JPEG_INPUT_BGR888,           /* B, G, R byte order for the encoder. */
  P4_JPEG_INPUT_GRAY8
};

enum p4_jpeg_sampling_e
{
  P4_JPEG_SAMPLE_444,
  P4_JPEG_SAMPLE_422,
  P4_JPEG_SAMPLE_420,
  P4_JPEG_SAMPLE_GRAY
};

/* Encoder input is a tightly packed MCU-padded frame. Repeat visible edge
 * pixels into padding before calling. Width/height describe the visible
 * JPEG, while DMA consumes ceil(width/MCU)*ceil(height/MCU) pixels. Output
 * address and capacity are 128-byte aligned and exclusive until return.
 * output_size is zero on every error, including ENOSPC (no partial JPEG).
 */

struct p4_jpeg_encode_s
{
  FAR const uint8_t *input;
  size_t input_size;
  FAR uint8_t *output;
  size_t output_capacity;
  size_t output_size;
  uint16_t width;
  uint16_t height;
  enum p4_jpeg_input_e format;
  enum p4_jpeg_sampling_e sampling;
  uint8_t quality;               /* 1..100. */
  uint32_t timeout_ms;           /* 1..5000; zero selects 250 ms. */
};

int esp32p4_jpeg_register(FAR const char *path);

#endif /* __INCLUDE_NUTTX_VIDEO_P4_JPEG_H */
