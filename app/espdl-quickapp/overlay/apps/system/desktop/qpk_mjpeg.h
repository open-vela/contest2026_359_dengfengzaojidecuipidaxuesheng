/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdint.h>
void *qpk_mjpeg_create(void);
void qpk_mjpeg_destroy(void *decoder);
/* Live view only: nearest-sample RGB at a reduced JPEG output size. Zero/zero
 * restores full preview detail. Still/region/photo decoding is unaffected. */
int qpk_mjpeg_preview_limits(void *decoder, unsigned width, unsigned height);
/* Complete baseline JPEG in read-only storage; never inserts MJPEG tables. */
int qpk_mjpeg_decode_still(void *decoder, const uint8_t *input, size_t size,
                           uint16_t *output, unsigned width, unsigned height);
/* Exact size or half-resolution baseline JPEG, expanded 2x in place. */
int qpk_mjpeg_decode_still_fit(void *decoder, const uint8_t *input, size_t size,
                               uint16_t *output, unsigned width, unsigned height);
/* Read the pixel size of a JPEG frame without decoding it. */
int qpk_mjpeg_probe(const uint8_t *input, size_t size, unsigned *width,
                    unsigned *height);
/* Valid after successful decode; includes any inserted standard DHT. */
size_t qpk_mjpeg_frame_size(void *decoder);
int qpk_mjpeg_decode(void *decoder, uint8_t *input, size_t size, size_t capacity,
                     uint16_t *output, unsigned width, unsigned height,
                     unsigned expected_width, unsigned expected_height);
/* Reduced cached artwork: never allocate a full-size hardware DMA frame. */
int qpk_mjpeg_decode_software(void *decoder, uint8_t *input, size_t size, size_t capacity,
                              uint16_t *output, unsigned width, unsigned height,
                              unsigned expected_width, unsigned expected_height);
/* Decode only the image region of a wider framebuffer, preserving controls. */
int qpk_mjpeg_decode_region(void *decoder, uint8_t *input, size_t size, size_t capacity,
                           uint16_t *output, unsigned width, unsigned height, unsigned stride,
                           unsigned expected_width, unsigned expected_height);
/* Same pixels as contain-to-width/height followed by nearest-neighbour
 * reduction to view_width/view_height; only writes the final view rectangle. */
int qpk_mjpeg_decode_view(void *decoder, uint8_t *input, size_t size, size_t capacity,
                         uint16_t *output, unsigned width, unsigned height, unsigned stride,
                         unsigned expected_width, unsigned expected_height,
                         unsigned view_width, unsigned view_height);
struct qpk_mjpeg_view {
  unsigned canvas_width,canvas_height,stride,width,height;
  unsigned crop_x,crop_y,crop_width,crop_height,style;
};
/* Fused reduced decode, canvas crop and colour transform. Writes only the
 * requested live view; still/region decoding keeps its original quality. */
int qpk_mjpeg_decode_preview(void *decoder,uint8_t *input,size_t size,size_t capacity,
                             uint16_t *output,unsigned expected_width,unsigned expected_height,
                             const struct qpk_mjpeg_view *view);
