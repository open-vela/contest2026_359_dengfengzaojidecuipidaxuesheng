/* SPDX-License-Identifier: Apache-2.0 */
#include <nuttx/config.h>
#include "qpk_jpeg_hw.h"
#include "qpk_ppa.h"

#ifdef CONFIG_ESP32P4_JPEG
#include <nuttx/video/p4_jpeg.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "qpk_jpeg_testdata.h"

static int jpeg_call(int fd, int cmd, void *job)
{
  int ret = ioctl(fd, cmd, (unsigned long)job);
  return ret < 0 ? -errno : ret;
}

int qpk_jpeg_hw_decode_scaled(const uint8_t *input, size_t size,
                              uint16_t *output, unsigned width,
                              unsigned height, unsigned out_width,
                              unsigned out_height, unsigned stride)
{
  struct p4_jpeg_decode_s job = {0};
  uint8_t *copy = NULL;
  void *pixels = NULL;
  int fd = -1;
  int ret = -EINVAL;

  if (!input || !size || size > 4 * 1024 * 1024 || !output ||
      !width || width > 4096 || !height || height > 4096 ||
      !out_width || !out_height || out_width > width || out_height > height ||
      stride < out_width || stride > 4096) return ret;
  bool scaled = out_width != width || out_height != height;
  if (scaled)
    {
#ifdef CONFIG_ESP32P4_PPA
      if (!qpk_ppa_rgb565_scale256(width, height, out_width, out_height))
        return -ENOTSUP;
#else
      return -ENOTSUP;
#endif
    }
  fd = open("/dev/jpeg0", O_RDWR | O_CLOEXEC);
  if (fd < 0) return -errno;
  /* Read-only input can reside in Flash. Always give DMA its own RAM copy. */
  copy = malloc(size);
  if (!copy) { ret = -ENOMEM; goto done; }
  memcpy(copy, input, size);
  job.info.input = copy;
  job.info.input_size = size;
  ret = jpeg_call(fd, P4_JPEG_INFO, &job.info);
  if (ret < 0) goto done;
  if (job.info.width != width || job.info.height != height ||
      job.info.components != 3) { ret = -ENOTSUP; goto done; }
  job.output_capacity = ((size_t)job.info.padded_width *
                         job.info.padded_height * 2 + 127) & ~(size_t)127;
  pixels = memalign(128, job.output_capacity);
  if (!pixels) { ret = -ENOMEM; goto done; }
  job.output = pixels;
  job.format = P4_JPEG_RGB565;
  ret = jpeg_call(fd, P4_JPEG_DECODE, &job);
  if (ret < 0) goto done;
  if (scaled)
    {
      /* JPEG ioctl released its DMA2D lease before PPA acquires the same
       * manager. No nested hardware ownership or caller-buffer DMA.
       */
#ifdef CONFIG_ESP32P4_PPA
      ret = qpk_ppa_rgb565_scale(pixels, width, height, job.stride / 2,
                                output, out_width, out_height, stride);
#endif
    }
  else
    {
      for (unsigned y = 0; y < height; y++)
        memcpy(output + (size_t)y * stride, (uint8_t *)pixels +
               (size_t)y * job.stride, (size_t)width * 2);
    }
done:
  free(pixels);
  free(copy);
  close(fd);
  return ret;
}

int qpk_jpeg_hw_decode(const uint8_t *input, size_t size, uint16_t *output,
                       unsigned width, unsigned height, unsigned stride)
{
  return qpk_jpeg_hw_decode_scaled(input, size, output, width, height,
                                  width, height, stride);
}

/* Built-in deterministic images and libjpeg reference pixels. No camera,
 * display, file writes, network or user configuration is involved.
 */
int qpk_jpeg_hw_selftest(void)
{
  int fd = open("/dev/jpeg0", O_RDWR | O_CLOEXEC);
  unsigned failures = 0;
  if (fd < 0) { printf("JPEG_TEST open=%d\n", errno); return -errno; }
  {
    /* Keep complete headers/SOS but remove entropy. The parser accepts the
     * bounded container; hardware must fail or time out without retaining
     * the DMA lease. Every valid decode below also checks recovery.
     */
    uint8_t *input = malloc(sizeof(jpeg_444_data));
    uint8_t *output = memalign(128, 32 * 32 * 2);
    if (!input || !output) failures++;
    else
      {
        memcpy(input, jpeg_444_data, sizeof(jpeg_444_data));
        size_t p = 2;
        while (p + 4 < sizeof(jpeg_444_data) && input[p + 1] != 0xda)
          p += 2 + ((unsigned)input[p + 2] << 8) + input[p + 3];
        p += 2 + ((unsigned)input[p + 2] << 8) + input[p + 3];
        input[p++] = 0xff;
        input[p++] = 0xd9;
        struct p4_jpeg_decode_s job = {0};
        job.info.input = input;
        job.info.input_size = p;
        job.output = output;
        job.output_capacity = 32 * 32 * 2;
        job.format = P4_JPEG_RGB565;
        job.timeout_ms = 25;
        int ret = jpeg_call(fd, P4_JPEG_DECODE, &job);
        bool ok = (ret == -ETIMEDOUT || ret == -EIO) &&
                  job.output_size == 0 && job.stride == 0;
        printf("JPEG_TEST empty_entropy result=%d %s\n", ret, ok ? "PASS" : "FAIL");
        if (!ok) failures++;
      }
    free(input);
    free(output);
  }
  for (unsigned i = 0; i < sizeof(g_jpeg_tests) / sizeof(g_jpeg_tests[0]); i++)
    {
      const struct jpeg_test_s *test = &g_jpeg_tests[i];
      uint8_t *input = malloc(test->size);
      if (!input) { failures++; break; }
      memcpy(input, test->data, test->size);
      for (unsigned f = 0; f < (test->gray ? 1u : 2u); f++)
        {
          struct p4_jpeg_decode_s job = {0};
          unsigned bytes = test->gray ? 1 : f ? 3 : 2;
          unsigned max = 0;
          uint64_t sum = 0;
          size_t count = 0;
          job.info.input = input;
          job.info.input_size = test->size;
          int ret = jpeg_call(fd, P4_JPEG_INFO, &job.info);
          if (ret < 0) { failures++; printf("JPEG_TEST %s info=%d\n", test->name, ret); continue; }
          job.output_capacity = ((size_t)job.info.padded_width *
                                 job.info.padded_height * bytes + 127) & ~(size_t)127;
          uint8_t *allocation = memalign(128, job.output_capacity + 128);
          if (!allocation) { failures++; break; }
          memset(allocation, 0xa5, job.output_capacity + 128);
          size_t data_capacity = job.output_capacity;
          job.output_capacity += 128;
          job.output = allocation;
          job.format = test->gray ? P4_JPEG_GRAY8 : f ? P4_JPEG_RGB888 : P4_JPEG_RGB565;
          ret = jpeg_call(fd, P4_JPEG_DECODE, &job);
          bool guard = true;
          for (unsigned n = 0; n < 128; n++)
            if (allocation[data_capacity + n] != 0xa5) guard = false;
          if (!ret)
            for (unsigned y = 0; y < test->height; y++)
              for (unsigned x = 0; x < test->width; x++)
                for (unsigned c = 0; c < (test->gray ? 1u : 3u); c++)
                  {
                    /* Compare tile interiors: libjpeg's chroma interpolation
                     * differs from the DMA engine at colour boundaries.
                     */
                    if (!test->gray && !test->solid &&
                        ((x > 1 && x < test->width - 2 && (x % 16 < 2 || x % 16 > 13)) ||
                         (y > 1 && y < test->height - 2 && (y % 16 < 2 || y % 16 > 13)))) continue;
                    count++;
                    unsigned actual;
                    const uint8_t *pixel = allocation + y * job.stride + x * bytes;
                    if (bytes == 2)
                      {
                        uint16_t v = pixel[0] | (uint16_t)pixel[1] << 8;
                        actual = c == 0 ? ((v >> 11) & 31) * 255 / 31 :
                                 c == 1 ? ((v >> 5) & 63) * 255 / 63 : (v & 31) * 255 / 31;
                      }
                    else actual = pixel[c];
                    unsigned expected = test->reference[test->solid ? c :
                      ((size_t)y * test->width + x) * (test->gray ? 1 : 3) + c];
                    unsigned error = actual > expected ? actual - expected : expected - actual;
                    if (error > max) max = error;
                    sum += error;
                  }
          /* Different legal chroma upsamplers differ near edges. Flat colour
           * tiles keep the reference useful for channel order and MCU layout.
           */
          bool ok = !ret && count && guard && max <= (bytes == 1 ? 3u : 24u) &&
                    sum <= count * (bytes == 1 ? 1u : 6u) &&
                    job.info.width == test->width && job.info.height == test->height;
          printf("JPEG_TEST %s bytes=%u result=%d max=%u mean_x100=%lu guard=%u %s\n",
                 test->name, bytes, ret, max, (unsigned long)(count ? sum * 100 / count : 0),
                 guard, ok ? "PASS" : "FAIL");
          if (!ok && !ret)
            printf("JPEG_TEST sample=%02x,%02x,%02x,%02x expected=%u,%u,%u\n",
                   allocation[0], allocation[1], allocation[2], allocation[3],
                   test->reference[0], test->reference[1], test->reference[2]);
          if (!ok) failures++;
          free(allocation);
        }
      free(input);
    }
  close(fd);
  printf("JPEG_TEST failures=%u\n", failures);
  return failures ? -EIO : 0;
}
#else
#include <errno.h>
int qpk_jpeg_hw_decode(const uint8_t *input, size_t size, uint16_t *output,
                       unsigned width, unsigned height, unsigned stride)
{
  (void)input; (void)size; (void)output;
  (void)width; (void)height; (void)stride;
  return -ENOSYS;
}
int qpk_jpeg_hw_selftest(void) { return -ENOSYS; }
int qpk_jpeg_hw_decode_scaled(const uint8_t *input, size_t size,
                              uint16_t *output, unsigned width,
                              unsigned height, unsigned out_width,
                              unsigned out_height, unsigned stride)
{
  (void)input; (void)size; (void)output; (void)width; (void)height;
  (void)out_width; (void)out_height; (void)stride;
  return -ENOSYS;
}
#endif
