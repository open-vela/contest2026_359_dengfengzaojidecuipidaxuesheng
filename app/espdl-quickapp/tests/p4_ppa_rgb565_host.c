/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nuttx/video/p4_ppa.h>
#include <nuttx/video/p4_jpeg.h>
#include "qpk_ppa.h"
#include "qpk_jpeg_hw.h"
#include "qpk_mjpeg.h"
#include "qpk_jpeg_testdata.h"

static unsigned opens, closes, scales, decodes;
static int failure;
static uint16_t *caller;

int ppa_test_open(const char *path, int flags, ...)
{
  assert(flags & O_CLOEXEC);
  if (!strcmp(path, "/dev/jpeg0"))
    {
      if (failure == 3) { errno = ENOENT; return -1; }
      opens++;
      return 7;
    }

  assert(!strcmp(path, "/dev/ppa0"));
  if (failure == 2) { errno = ENOENT; return -1; }
  opens++;
  return 9;
}

int ppa_test_close(int fd)
{
  assert(fd == 7 || fd == 9);
  closes++;
  return 0;
}

int ppa_test_ioctl(int fd, unsigned long cmd, ...)
{
  va_list args;
  va_start(args, cmd);
  void *argument = (void *)va_arg(args, unsigned long);
  va_end(args);
  if (fd == 7)
    {
      struct p4_jpeg_decode_s *job = argument;
      job->info.width = job->info.height = 32;
      job->info.padded_width = job->info.padded_height = 32;
      job->info.components = 3;
      if (cmd == P4_JPEG_INFO) return 0;
      assert(cmd == P4_JPEG_DECODE && job->output_capacity == 2048);
      assert(job->format == P4_JPEG_RGB565);
      decodes++;
      job->stride = 64;
      for (unsigned y = 0; y < 32; y++)
        for (unsigned x = 0; x < 32; x++)
          ((uint16_t *)job->output)[y * 32 + x] = y * 100 + x;
      return 0;
    }

  assert(fd == 9 && cmd == P4_PPA_SRM_RGB565);
  struct p4_ppa_job_s *job = argument;
  assert(!((uintptr_t)job->input & 127) && !((uintptr_t)job->output & 127));
  assert(!(job->input_size & 127) && !(job->output_size & 127));
  assert(job->output != caller && job->input != caller);
  scales++;
  /* A deterministic transport peer validates row packing/commit, not the
   * physical PPA interpolation algorithm or its colour accuracy.
   */
  for (unsigned y = 0; y < job->out_height; y++)
    for (unsigned x = 0; x < job->out_width; x++)
      ((uint16_t *)job->output)[y * job->out_width + x] =
        ((const uint16_t *)job->input)[(size_t)(y * 256 / job->scale256) *
                                      job->width + x * 256 / job->scale256];
  if (failure == 1) { errno = EIO; return -1; }
  return 0;
}

static void adapter(void)
{
  uint16_t input[36 * 34], output[20 * 18];
  for (unsigned y = 0; y < 34; y++)
    for (unsigned x = 0; x < 36; x++) input[y * 36 + x] = y * 100 + x;
  caller = output;
  for (failure = 0; failure <= 2; failure++)
    {
      memset(output, 0x5a, sizeof(output));
      int ret = qpk_ppa_rgb565_scale(input, 32, 32, 36, output, 16, 16, 20);
      assert(ret == (failure == 0 ? 0 : failure == 1 ? -EIO : -ENOENT));
      for (unsigned y = 0; y < 18; y++)
        for (unsigned x = 0; x < 20; x++)
          assert(output[y * 20 + x] ==
            (!failure && y < 16 && x < 16 ? y * 200 + x * 2 : 0x5a5a));
      assert(opens == closes);
    }

  unsigned previous = opens;
  assert(!qpk_ppa_rgb565_scale256(32, 32, 17, 16));
  assert(!qpk_ppa_rgb565_scale256(32, 32, 64, 64));
  assert(!qpk_ppa_rgb565_scale256(UINT32_MAX, 32, 16, 16));
  assert(qpk_ppa_rgb565_scale256(31, 33, 31, 33) == 256);
  assert(qpk_ppa_rgb565_scale(NULL, 32, 32, 32, output, 16, 16, 20) == -EINVAL);
  assert(qpk_ppa_rgb565_scale(input, 32, 32, 31, output, 16, 16, 20) == -EINVAL);
  assert(qpk_ppa_rgb565_scale(input, 32, 32, 36, output, 17, 16, 20) == -ENOTSUP);
  assert(opens == previous);
  failure = 0;
  caller = input;
  assert(!qpk_ppa_rgb565_scale(input, 32, 32, 36, input, 16, 16, 36));
  for (unsigned y = 0; y < 16; y++)
    for (unsigned x = 0; x < 16; x++) assert(input[y * 36 + x] == y * 200 + x * 2);
}

static void jpeg_pipeline(void)
{
  uint16_t output[20 * 26], software[20 * 26];
  uint8_t jpeg[sizeof(jpeg_444_data) + 512];
  memcpy(jpeg, jpeg_444_data, sizeof(jpeg_444_data));
  void *decoder = qpk_mjpeg_create();
  assert(decoder);
  caller = output;
  failure = 0;
  memset(output, 0x5a, sizeof(output));
  unsigned before = scales;
  assert(!qpk_mjpeg_decode_region(decoder, jpeg, sizeof(jpeg_444_data),
          sizeof(jpeg), output, 16, 24, 20, 32, 32));
  assert(scales == before + 1);
  for (unsigned y = 0; y < 26; y++)
    for (unsigned x = 0; x < 20; x++)
      assert(output[y * 20 + x] ==
        (y >= 24 || x >= 16 ? 0x5a5a : y < 4 || y >= 20 ? 0 :
         (y - 4) * 200 + x * 2));

  memset(output, 0x5a, sizeof(output));
  before = scales;
  assert(!qpk_mjpeg_decode(decoder, jpeg, sizeof(jpeg_444_data), sizeof(jpeg),
                           output, 16, 16, 32, 32));
  assert(scales == before + 1);
  for (unsigned i = 0; i < sizeof(output) / sizeof(output[0]); i++)
    assert(output[i] == (i < 256 ? (i / 16) * 200 + (i % 16) * 2 : 0x5a5a));

  /* Live reduced previews retain their software sampling policy. */
  before = opens;
  assert(!qpk_mjpeg_decode_view(decoder, jpeg, sizeof(jpeg_444_data),
          sizeof(jpeg), output, 32, 32, 32, 32, 32, 16, 16));
  assert(opens == before);

  failure = 3; caller = software;
  memset(software, 0x5a, sizeof(software));
  assert(!qpk_mjpeg_decode_region(decoder, jpeg, sizeof(jpeg_444_data),
          sizeof(jpeg), software, 16, 24, 20, 32, 32));
  caller = output;
  for (failure = 1; failure <= 2; failure++)
    {
      memset(output, 0x5a, sizeof(output));
      assert(!qpk_mjpeg_decode_region(decoder, jpeg, sizeof(jpeg_444_data),
              sizeof(jpeg), output, 16, 24, 20, 32, 32));
      assert(!memcmp(output, software, sizeof(output)));
      assert(opens == closes);
    }

  qpk_mjpeg_destroy(decoder);
}

int main(void)
{
  adapter();
  jpeg_pipeline();
  assert(decodes && scales && opens == closes);
  puts("PPA_RGB565_APP_OK: exact ratios, padded/aliased rows, private DMA commit, failure output unchanged, real JPEG fit/letterbox, real software fallback, no FD leaks");
  return 0;
}
