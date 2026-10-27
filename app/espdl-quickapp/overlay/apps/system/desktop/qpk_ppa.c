/* SPDX-License-Identifier: Apache-2.0 */
#include <nuttx/config.h>
#include "qpk_ppa.h"
#include <errno.h>

unsigned qpk_ppa_rgb565_scale256(unsigned width, unsigned height,
                                unsigned out_width, unsigned out_height)
{
  unsigned scale;

  if (width < 2 || width > 1280 || height < 2 || height > 720 ||
      out_width < 2 || out_width > width ||
      out_height < 2 || out_height > height)
    return 0;

  if (out_width * 256 % width || out_height * 256 % height)
    return 0;

  scale = out_width * 256 / width;
  if (scale < 16 || scale > 256 || (scale & 15) ||
      scale != out_height * 256 / height)
    return 0;

  return scale;
}

#ifdef CONFIG_ESP32P4_PPA
#include <fcntl.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <nuttx/video/p4_ppa.h>

int qpk_ppa_rgb565_scale(const uint16_t *input, unsigned width,
                         unsigned height, unsigned stride, uint16_t *output,
                         unsigned out_width, unsigned out_height,
                         unsigned out_stride)
{
  struct p4_ppa_job_s job = {0};
  uint16_t *packed = NULL;
  uint16_t *scaled = NULL;
  int fd = -1;
  int ret;
  unsigned scale = qpk_ppa_rgb565_scale256(width, height,
                                          out_width, out_height);

  if (!input || !output || stride < width || stride > 4096 ||
      out_stride < out_width || out_stride > 4096)
    return -EINVAL;

  if (!scale) return -ENOTSUP;

  job.input_size = ((size_t)width * height * 2 + 127) & ~(size_t)127;
  job.output_size = ((size_t)out_width * out_height * 2 + 127) & ~(size_t)127;
  packed = memalign(128, job.input_size);
  scaled = memalign(128, job.output_size);
  if (!packed || !scaled)
    {
      ret = -ENOMEM;
      goto done;
    }

  memset(packed, 0, job.input_size);
  memset(scaled, 0, job.output_size);
  for (unsigned y = 0; y < height; y++)
    memcpy(packed + (size_t)y * width, input + (size_t)y * stride,
           (size_t)width * 2);

  fd = open("/dev/ppa0", O_RDWR | O_CLOEXEC);
  if (fd < 0)
    {
      ret = -errno;
      goto done;
    }

  job.input = packed;
  job.output = scaled;
  job.width = width;
  job.height = height;
  job.out_width = out_width;
  job.out_height = out_height;
  job.scale256 = scale;
  ret = ioctl(fd, P4_PPA_SRM_RGB565, (unsigned long)&job);
  if (ret < 0)
    {
      ret = -errno;
      goto done;
    }

  for (unsigned y = 0; y < out_height; y++)
    memcpy(output + (size_t)y * out_stride, scaled + (size_t)y * out_width,
           (size_t)out_width * 2);
  ret = 0;

done:
  if (fd >= 0) close(fd);
  free(scaled);
  free(packed);
  return ret;
}
#else
int qpk_ppa_rgb565_scale(const uint16_t *input, unsigned width,
                         unsigned height, unsigned stride, uint16_t *output,
                         unsigned out_width, unsigned out_height,
                         unsigned out_stride)
{
  (void)input; (void)width; (void)height; (void)stride; (void)output;
  (void)out_width; (void)out_height; (void)out_stride;
  return -ENOSYS;
}
#endif
