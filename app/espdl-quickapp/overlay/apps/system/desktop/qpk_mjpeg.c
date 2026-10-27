/* SPDX-License-Identifier: Apache-2.0 */
#ifdef __NuttX__
#include <nuttx/config.h>
#endif
#include "qpk_mjpeg.h"
#include "camera_color.h"
#include "qpk_tjpgd.h"
#include "qpk_mjpeg_dht.h"
#ifdef CONFIG_ESP32P4_JPEG
#include "qpk_jpeg_hw.h"
#endif
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct mjpeg_decoder
{
  JDEC jpeg;
  uint8_t workspace[16384];
  const uint8_t *input;
  size_t size, position;
  uint16_t *pixels;
  uint16_t *scratch, *columns;
  size_t scratch_capacity;
  unsigned column_capacity;
  unsigned preview_width, preview_height;
  unsigned stride, height, width;
};
void *qpk_mjpeg_create(void) { return calloc(1, sizeof(struct mjpeg_decoder)); }
void qpk_mjpeg_destroy(void *decoder)
{
  struct mjpeg_decoder *d = decoder;
  if (d) { free(d->scratch); free(d->columns); free(d); }
}
int qpk_mjpeg_preview_limits(void *decoder, unsigned width, unsigned height)
{
  if (!decoder || width > 4096 || height > 4096 || (!width != !height)) return -EINVAL;
  struct mjpeg_decoder *d = decoder;
  d->preview_width = width; d->preview_height = height;
  return 0;
}
size_t qpk_mjpeg_frame_size(void *decoder)
{ return decoder ? ((struct mjpeg_decoder *)decoder)->size : 0; }
static size_t jpeg_input(JDEC *jd, uint8_t *buffer, size_t count)
{
  struct mjpeg_decoder *d = jd->device;
  if (count > d->size - d->position) count = d->size - d->position;
  if (buffer) memcpy(buffer, d->input + d->position, count);
  d->position += count;
  return count;
}
static int jpeg_output(JDEC *jd, void *bitmap, JRECT *r)
{
  struct mjpeg_decoder *d = jd->device;
  if (r->right >= d->width || r->bottom >= d->height) return 0;
  const uint16_t *src = bitmap;
  for (unsigned y = r->top; y <= r->bottom; y++)
    {
      size_t n = r->right - r->left + 1;
      memcpy(d->pixels + y * d->stride + r->left, src, n * 2);
      src += n;
    }
  return 1;
}
/* UVC MJPEG permits omission of the standard JPEG Huffman tables. Insert
 * the ISO/IEC 10918-1 default tables only if the frame has no DHT segment. */
static int jpeg_tables(uint8_t *input, size_t *size, size_t capacity)
{
  if (*size < 4 || input[0] != 255 || input[1] != 216) return -EBADMSG;
  bool dht = false;
  size_t p = 2;
  while (p + 4 <= *size)
    {
      if (input[p] != 255) return -EBADMSG;
      if (input[p + 1] == 255) { p++; continue; }
      uint8_t marker = input[p + 1];
      if (marker == 0xda)
        {
          if (dht) return 0;
          if (*size > capacity || sizeof(g_mjpeg_dht) > capacity - *size) return -EOVERFLOW;
          memmove(input + p + sizeof(g_mjpeg_dht), input + p, *size - p);
          memcpy(input + p, g_mjpeg_dht, sizeof(g_mjpeg_dht));
          *size += sizeof(g_mjpeg_dht);
          return 0;
        }
      if (marker == 0xc4) dht = true;
      if (marker == 0xd9 || marker == 0xd8 || marker == 0 || (marker >= 0xd0 && marker <= 0xd7)) return -EBADMSG;
      size_t n = ((size_t)input[p + 2] << 8) | input[p + 3];
      if (n < 2 || n > *size - p - 2) return -EBADMSG;
      p += n + 2;
    }
  return -EBADMSG;
}
/* Header-only parse used to size an on-screen image before decoding it. */
int qpk_mjpeg_probe(const uint8_t *input, size_t size, unsigned *width,
                    unsigned *height)
{
  if (!input || !size || !width || !height) return -EINVAL;
  struct mjpeg_decoder *d = calloc(1, sizeof(struct mjpeg_decoder));
  if (!d) return -ENOMEM;
  d->input = input;
  d->size = size;
  d->position = 0;
  JRESULT result = qpk_jd_prepare(&d->jpeg, jpeg_input, d->workspace,
                                  sizeof(d->workspace), d);
  if (result == JDR_OK)
    {
      *width = d->jpeg.width;
      *height = d->jpeg.height;
    }
  free(d);
  return result == JDR_OK ? 0 : -EBADMSG;
}

static int jpeg_decode_pixels(void *decoder, const uint8_t *input, size_t size,
                              uint16_t *output, unsigned width, unsigned height, unsigned stride,
                              unsigned expected_width, unsigned expected_height,
                              unsigned view_width, unsigned view_height,const struct qpk_mjpeg_view *view)
{
  struct mjpeg_decoder *d = decoder;
  if (!d || !input || !output || !width || !height || stride < width ||
      stride > 4096 || height > 4096 ||
      (view_width && (!view_height || view_width > width || view_height > height))) return -EINVAL;
  d->input = input; d->size = size; d->position = 0;
  if (qpk_jd_prepare(&d->jpeg, jpeg_input, d->workspace, sizeof(d->workspace), d) != JDR_OK) return -EBADMSG;
  if (!expected_width && !expected_height) {
    expected_width = d->jpeg.width; expected_height = d->jpeg.height;
    if (!((expected_width == width && expected_height == height) ||
          (expected_width * 2 == width && expected_height * 2 == height))) return -EPROTO;
  }
  if (d->jpeg.width != expected_width || d->jpeg.height != expected_height ||
      !expected_width || !expected_height || expected_width > 4096 || expected_height > 4096) return -EPROTO;
  unsigned scale = 0;
  unsigned limit_width = width, limit_height = height;
  if (view_width && d->preview_width) {
    if (limit_width > d->preview_width) limit_width = d->preview_width;
    if (limit_height > d->preview_height) limit_height = d->preview_height;
  }
  while (scale < 3 && ((expected_width >> scale) > limit_width || (expected_height >> scale) > limit_height)) scale++;
  d->width = expected_width >> scale; d->height = expected_height >> scale;
  if (!d->width || !d->height || d->width > width || d->height > height) return -ENOTSUP;
  size_t pixels = (size_t)d->width * d->height;
  /* The AI preview exactly matches the reduced JPEG. Decode into its worker
   * buffer and publish only on success, avoiding a temporary frame and copy. */
  bool direct = !view_width && d->width == width && d->height == height;
  bool twice = !view_width && stride == width && d->width * 2 == width && d->height * 2 == height;
#ifdef CONFIG_ESP32P4_JPEG
  /* Full-size frames use private aligned DMA output. Reduced live previews
   * retain their existing decoder and sampling policy. Unsupported streams
   * and hardware failures fall through to the software decoder.
   */
  if (direct && scale == 0 &&
      qpk_jpeg_hw_decode(input, size, output, width, height, stride) == 0)
    return 0;
  if ((!direct || scale > 0) && !view_width && (!view || !view->style))
    {
      unsigned fw = width;
      unsigned fh = (uint64_t)expected_height * width / expected_width;
      if (fh > height)
        {
          fh = height;
          fw = (uint64_t)expected_width * height / expected_height;
        }

      unsigned left = (width - fw) / 2;
      unsigned top = (height - fh) / 2;
      if (fw && fh && qpk_jpeg_hw_decode_scaled(input, size,
          output + (size_t)top * stride + left, expected_width,
          expected_height, fw, fh, stride) == 0)
        {
          /* No caller pixels were changed on failure. On success, clear
           * only the letterbox while preserving stride padding.
           */
          for (unsigned y = 0; y < height; y++)
            {
              uint16_t *row = output + (size_t)y * stride;
              if (y < top || y >= top + fh)
                memset(row, 0, (size_t)width * 2);
              else
                {
                  memset(row, 0, (size_t)left * 2);
                  memset(row + left + fw, 0, (size_t)(width - left - fw) * 2);
                }
            }

          return 0;
        }
    }
#endif
  if (!direct && !twice && pixels > d->scratch_capacity) {
    uint16_t *scratch = realloc(d->scratch, pixels * sizeof(*scratch));
    if (!scratch) return -ENOMEM;
    d->scratch = scratch; d->scratch_capacity = pixels;
  }
  d->pixels = direct || twice ? output : d->scratch; d->stride = direct ? stride : d->width;
  JRESULT result = view_width && d->preview_width ?
    qpk_jd_decomp_preview(&d->jpeg, jpeg_output, scale) : qpk_jd_decomp(&d->jpeg, jpeg_output, scale);
  if (result != JDR_OK) {
    d->pixels = NULL; return -EBADMSG;
  }
  if(view&&view->style)for(size_t i=0;i<pixels;i++)d->pixels[i]=camera_color_pixel(d->pixels[i],view->style);
  if (direct) { d->pixels = NULL; return 0; }
  if (twice) {
    /* Expand backwards in the caller's frame: no temporary allocation or
     * per-pixel divisions, and unread source rows remain intact. */
    for (unsigned y = d->height; y-- > 0;) {
      const uint16_t *src = output + (size_t)y * d->width;
      uint16_t *dst = output + (size_t)y * 2 * stride;
      for (unsigned x = d->width; x-- > 0;) { uint16_t v=src[x]; dst[x*2]=v; dst[x*2+1]=v; }
      memcpy(dst + stride, dst, (size_t)width * 2);
    }
    d->pixels = NULL; return 0;
  }
  unsigned dw = width, dh = (uint64_t)d->height * width / d->width;
  if (dh > height) { dh = height; dw = (uint64_t)d->width * height / d->height; }
  if (!dw || !dh) { d->pixels = NULL; return -ENOTSUP; }
  unsigned columns_needed = view_width ? view_width : dw;
  if (columns_needed > d->column_capacity) {
    uint16_t *columns = realloc(d->columns, columns_needed * sizeof(*columns));
    if (!columns) { d->pixels = NULL; return -ENOMEM; }
    d->columns = columns; d->column_capacity = columns_needed;
  }
  unsigned ox = (width - dw) / 2, oy = (height - dh) / 2;
  if (view_width) {
    /* Fuse the existing contain-to-canvas and canvas-to-view sampling. This
     * preserves both integer rounding steps without materializing the canvas. */
    for (unsigned x = 0; x < view_width; x++) {
      unsigned cx = view?view->crop_x+x*view->crop_width/view_width:x*width/view_width;
      d->columns[x] = cx < ox || cx >= ox + dw ? UINT16_MAX : (cx - ox) * d->width / dw;
    }
    unsigned previous = UINT32_MAX;
    for (unsigned y = 0; y < view_height; y++) {
      unsigned cy = view?view->crop_y+y*view->crop_height/view_height:y*height/view_height;
      uint16_t *dst = output + (size_t)y * stride;
      if (cy < oy || cy >= oy + dh) { memset(dst, 0, view_width * 2); previous = UINT32_MAX; continue; }
      unsigned sy = (cy - oy) * d->height / dh;
      const uint16_t *src = d->pixels + (size_t)sy * d->width;
      if (sy == previous) memcpy(dst, dst - stride, view_width * 2);
      else for (unsigned x = 0; x < view_width; x++)
        dst[x] = d->columns[x] == UINT16_MAX ? 0 : src[d->columns[x]];
      previous = sy;
    }
    d->pixels = NULL; return 0;
  }
  /* Keep reusable decoder-owned buffers and one column map. The old loop
   * divided once per destination pixel and cleared pixels it then overwrote. */
  for (unsigned x = 0; x < dw; x++) d->columns[x] = x * d->width / dw;
  for (unsigned y = 0; y < oy; y++) memset(output + (size_t)y * stride, 0, (size_t)width * 2);
  for (unsigned y = oy + dh; y < height; y++) memset(output + (size_t)y * stride, 0, (size_t)width * 2);
  unsigned previous = UINT32_MAX;
  for (unsigned y = 0; y < dh; y++)
    {
      unsigned sy = y * d->height / dh;
      const uint16_t *src = d->pixels + (size_t)sy * d->width;
      uint16_t *dst = output + (size_t)(y + oy) * stride + ox;
      memset(dst - ox, 0, ox * sizeof(*dst));
      memset(dst + dw, 0, (width - ox - dw) * sizeof(*dst));
      if (sy == previous) memcpy(dst, dst - stride, dw * sizeof(*dst));
      else if (dw == d->width) memcpy(dst, src, dw * sizeof(*dst));
      else for (unsigned x = 0; x < dw; x++) dst[x] = src[d->columns[x]];
      previous = sy;
    }
  d->pixels = NULL;
  return 0;
}

int qpk_mjpeg_decode_still(void *decoder, const uint8_t *input, size_t size,
                           uint16_t *output, unsigned width, unsigned height)
{
  return jpeg_decode_pixels(decoder,input,size,output,width,height,width,width,height,0,0,NULL);
}

int qpk_mjpeg_decode_still_fit(void *decoder, const uint8_t *input, size_t size,
                               uint16_t *output, unsigned width, unsigned height)
{
  return jpeg_decode_pixels(decoder,input,size,output,width,height,width,0,0,0,0,NULL);
}

int qpk_mjpeg_decode_region(void *decoder, uint8_t *input, size_t size, size_t capacity,
                           uint16_t *output, unsigned width, unsigned height, unsigned stride,
                           unsigned expected_width, unsigned expected_height)
{
  if (!decoder || !input || !output || !width || !height || stride < width ||
      stride > 4096 || height > 4096 || size > capacity) return -EINVAL;
  int ret = jpeg_tables(input,&size,capacity);
  if (ret < 0) return ret;
  return jpeg_decode_pixels(decoder,input,size,output,width,height,stride,expected_width,expected_height,0,0,NULL);
}

int qpk_mjpeg_decode_view(void *decoder, uint8_t *input, size_t size, size_t capacity,
                         uint16_t *output, unsigned width, unsigned height, unsigned stride,
                         unsigned expected_width, unsigned expected_height,
                         unsigned view_width, unsigned view_height)
{
  if (!decoder || !input || !output || !width || !height || stride < width ||
      stride > 4096 || height > 4096 || !view_width || !view_height ||
      view_width > width || view_height > height || size > capacity) return -EINVAL;
  int ret = jpeg_tables(input,&size,capacity);
  if (ret < 0) return ret;
  return jpeg_decode_pixels(decoder,input,size,output,width,height,stride,
                            expected_width,expected_height,view_width,view_height,NULL);
}
int qpk_mjpeg_decode_preview(void *decoder,uint8_t *input,size_t size,size_t capacity,
                             uint16_t *output,unsigned expected_width,unsigned expected_height,
                             const struct qpk_mjpeg_view *view)
{
  if(!view||!view->crop_width||!view->crop_height||view->crop_x>view->canvas_width||
     view->crop_y>view->canvas_height||view->crop_width>view->canvas_width-view->crop_x||
     view->crop_height>view->canvas_height-view->crop_y||view->style>=CAMERA_COLOR_STYLE_COUNT||
     !view->width||!view->height||size>capacity||!decoder||!input)return -EINVAL;
  int ret=jpeg_tables(input,&size,capacity);if(ret<0)return ret;
  return jpeg_decode_pixels(decoder,input,size,output,view->canvas_width,view->canvas_height,view->stride,
    expected_width,expected_height,view->width,view->height,view);
}

int qpk_mjpeg_decode(void *decoder, uint8_t *input, size_t size, size_t capacity,
                    uint16_t *output, unsigned width, unsigned height,
                    unsigned expected_width, unsigned expected_height)
{
  return qpk_mjpeg_decode_region(decoder,input,size,capacity,output,width,height,width,
                                 expected_width,expected_height);
}
