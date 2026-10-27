/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <nuttx/video/p4_jpeg.h>
#include "qpk_jpeg_hw.h"

static int opens, closes, mode, decodes;
int jpeg_test_open(const char *path, int flags, ...)
{
  assert(!strcmp(path, "/dev/jpeg0"));
  assert(flags & O_CLOEXEC);
  if (mode == 3) { errno = ENOENT; return -1; }
  opens++;
  return 7;
}
int jpeg_test_close(int fd) { assert(fd == 7); closes++; return 0; }
int jpeg_test_ioctl(int fd, unsigned long cmd, ...)
{
  va_list ap;
  va_start(ap, cmd);
  struct p4_jpeg_decode_s *job = (void *)va_arg(ap, unsigned long);
  va_end(ap);
  assert(fd == 7);
  assert(job->info.input_size == 3 && job->info.input[1] == 9);
  job->info.width = 30;
  job->info.height = 24;
  job->info.padded_width = 32;
  job->info.padded_height = 32;
  job->info.components = mode == 2 ? 1 : 3;
  if (cmd == P4_JPEG_INFO) return 0;
  assert(cmd == P4_JPEG_DECODE);
  assert(!((uintptr_t)job->output & 127));
  assert(job->output_capacity == 32 * 32 * 2);
  decodes++;
  job->stride = 64;
  for (unsigned y = 0; y < 32; y++)
    for (unsigned x = 0; x < 32; x++)
      ((uint16_t *)job->output)[y * 32 + x] = y * 100 + x;
  if (mode == 1) { errno = EIO; return -1; }
  return 0;
}
int main(void)
{
  const uint8_t input[] = {4, 9, 2};
  uint16_t output[40 * 26];
  for (unsigned m = 0; m < 4; m++)
    {
      mode = m;
      memset(output, 0x5a, sizeof(output));
      int ret = qpk_jpeg_hw_decode(input, sizeof(input), output, 30, 24, 40);
      assert(ret == (m == 0 ? 0 : m == 1 ? -EIO : m == 2 ? -ENOTSUP : -ENOENT));
      for (unsigned y = 0; y < 26; y++)
        for (unsigned x = 0; x < 40; x++)
          assert(output[y * 40 + x] ==
                 (m == 0 && y < 24 && x < 30 ? y * 100 + x : 0x5a5a));
      assert(opens == closes);
    }
  assert(decodes == 2);
  assert(qpk_jpeg_hw_decode(input, SIZE_MAX, output, 30, 24, 40) == -EINVAL);
  assert(qpk_jpeg_hw_decode(input, 3, output, 30, 24, 29) == -EINVAL);
  puts("JPEG_ADAPTER_OK: padded-row crop; failed DMA leaves output intact; unsupported/absent device fallback; no FD leaks");
}
