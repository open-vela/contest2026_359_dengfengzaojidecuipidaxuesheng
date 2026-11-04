/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SEAGULL_SIDE 128u
#define SEAGULL_BYTES (SEAGULL_SIDE * SEAGULL_SIDE * 3u)
#define SEAGULL_PACKED_COUNT 42u
#define SEAGULL_REAR 32u
#define SEAGULL_YAW_BASE 33u

struct seagull_packed_frame {
  const uint8_t *rgb; size_t rgb_bytes;
  const uint8_t *alpha; size_t alpha_bytes;
};
extern const struct seagull_packed_frame seagull_packed_frames[SEAGULL_PACKED_COUNT];
/* Immutable packed resources are shared; callers own independent pixel buffers.
 * Output is native RGB565 little-endian plane followed by A8. No file I/O. */
int glass_seagull_decode(unsigned index,uint8_t *pixels,unsigned side);
int glass_seagull_render(uint8_t *pixels,unsigned side,unsigned emotion,
                        unsigned frame,bool gaze,int gaze_x,int gaze_y);
unsigned glass_seagull_yaw(int x);
