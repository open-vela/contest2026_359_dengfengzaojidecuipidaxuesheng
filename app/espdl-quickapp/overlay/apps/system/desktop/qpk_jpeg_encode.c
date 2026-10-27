/* SPDX-License-Identifier: Apache-2.0 */
#include <nuttx/config.h>
#include "qpk_jpeg_hw.h"
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#ifdef CONFIG_ESP32P4_JPEG
#include <nuttx/video/p4_jpeg.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "qpk_tjpgd.h"

int qpk_jpeg_hw_encode565(const uint16_t *input, unsigned width,
                          unsigned height, unsigned stride, unsigned quality,
                          uint8_t **output, size_t *size)
{
  struct p4_jpeg_encode_s job = {0};
  uint16_t *padded = NULL;
  uint8_t *encoded = NULL;
  int ret = -EINVAL;
  if (!output || !size) return ret;
  *output = NULL; *size = 0;
  if (!input || !width || !height || width > 4096 || height > 4096 ||
      stride < width || stride > 4096 || !quality || quality > 100) return ret;
  unsigned pw = (width + 15) & ~15u, ph = (height + 15) & ~15u;
  size_t pixels = (size_t)pw * ph;
  if (pixels > 4 * 1024 * 1024) return -EFBIG;
  int fd = open("/dev/jpeg0", O_RDWR | O_CLOEXEC);
  if (fd < 0) return -errno;
  padded = malloc(pixels * 2);
  if (!padded) { ret = -ENOMEM; goto done; }
  for (unsigned y = 0; y < ph; y++)
    {
      const uint16_t *src = input + (size_t)(y < height ? y : height - 1) * stride;
      uint16_t *dst = padded + (size_t)y * pw;
      memcpy(dst, src, width * 2);
      for (unsigned x = width; x < pw; x++) dst[x] = src[width - 1];
    }
  job.output_capacity = (pixels * 6 + 4096 + 127) & ~(size_t)127;
  if (job.output_capacity > 4 * 1024 * 1024) job.output_capacity = 4 * 1024 * 1024;
  encoded = memalign(128, job.output_capacity);
  if (!encoded) { ret = -ENOMEM; goto done; }
  job.input = (uint8_t *)padded; job.input_size = pixels * 2;
  job.output = encoded;
  job.width = width; job.height = height;
  job.format = P4_JPEG_INPUT_RGB565;
  job.sampling = P4_JPEG_SAMPLE_420;
  job.quality = quality;
  ret = ioctl(fd, P4_JPEG_ENCODE, (unsigned long)&job);
  if (ret < 0) { ret = -errno; goto done; }
  *output = encoded; *size = job.output_size; encoded = NULL;
done:
  free(encoded); free(padded); close(fd);
  return ret;
}

struct encode_sw_s
{
  JDEC decoder;
  uint8_t workspace[16384];
  const uint8_t *input;
  size_t size, position;
  uint16_t *output;
  unsigned width, height;
};

static size_t encode_sw_input(JDEC *jd, uint8_t *buffer, size_t count)
{
  struct encode_sw_s *s = jd->device;
  if (count > s->size - s->position) count = s->size - s->position;
  if (buffer) memcpy(buffer, s->input + s->position, count);
  s->position += count;
  return count;
}

static int encode_sw_output(JDEC *jd, void *bitmap, JRECT *r)
{
  struct encode_sw_s *s = jd->device;
  if (r->right >= s->width || r->bottom >= s->height) return 0;
  const uint16_t *source = bitmap;
  size_t n = r->right - r->left + 1;
  for (unsigned y = r->top; y <= r->bottom; y++)
    {
      memcpy(s->output + (size_t)y * s->width + r->left, source, n * 2);
      source += n;
    }
  return 1;
}

static int encode_sw_decode(const uint8_t *data, size_t size,
                             uint16_t *output, unsigned width, unsigned height)
{
  struct encode_sw_s *s = calloc(1, sizeof(*s));
  if (!s) return -ENOMEM;
  s->input = data; s->size = size; s->output = output;
  s->width = width; s->height = height;
  int ret = qpk_jd_prepare(&s->decoder, encode_sw_input, s->workspace,
                         sizeof(s->workspace), s) == JDR_OK &&
            s->decoder.width == width && s->decoder.height == height &&
            qpk_jd_decomp(&s->decoder, encode_sw_output, 0) == JDR_OK ? 0 : -EBADMSG;
  free(s);
  return ret;
}

static unsigned encode_channel(uint16_t pixel, unsigned c)
{
  return c == 0 ? (pixel >> 11) * 255 / 31 :
         c == 1 ? ((pixel >> 5) & 63) * 255 / 63 : (pixel & 31) * 255 / 31;
}

static void encode_test_rgb(unsigned x, unsigned y, bool gray, bool solid,
                             bool texture,
                             uint8_t rgb[3])
{
  static const uint8_t colors[4][3] = {{208,48,48},{48,208,48},{48,48,208},{192,176,64}};
  if (texture)
    {
      rgb[0] = (x * 17 + y * 3) & 255;
      rgb[1] = (x * 5 + y * 19) & 255;
      rgb[2] = (x * 11 + y * 7) & 255;
      if (gray) memset(rgb, (x * 7 + y * 3) & 255, 3);
    }
  else if (gray) memset(rgb, 32 + ((x / 8 + y / 8) % 4) * 56, 3);
  else if (solid) { rgb[0] = 80; rgb[1] = 140; rgb[2] = 200; }
  else memcpy(rgb, colors[(x / 16 + 2 * (y / 16)) % 4], 3);
}

int qpk_jpeg_encode_selftest(void)
{
  int fd = open("/dev/jpeg0", O_RDWR | O_CLOEXEC);
  if (fd < 0) { printf("JPEG_ENCODE open=%d\n", errno); return -errno; }
  unsigned failures = 0, cases = 0;
  /* Same engine, formats and sampling alternate on every iteration. */
  for (unsigned test = 0; test < 19; test++)
    {
      unsigned format = test < 12 ? (test / 3) % 2 :
                        test < 14 ? P4_JPEG_INPUT_GRAY8 : P4_JPEG_INPUT_RGB565;
      unsigned sampling = format == P4_JPEG_INPUT_GRAY8 ? P4_JPEG_SAMPLE_GRAY : test % 3;
      unsigned width = test == 15 ? 1024 : test == 14 ? 30 : test == 13 ? 128 : 32;
      unsigned height = test == 15 ? 600 : test == 14 ? 24 : 32;
      if (test >= 14) sampling = P4_JPEG_SAMPLE_420;
      if (test >= 16)
        {
          format = test - 16;
          sampling = format == P4_JPEG_INPUT_GRAY8 ? P4_JPEG_SAMPLE_GRAY : P4_JPEG_SAMPLE_444;
          width = format == P4_JPEG_INPUT_GRAY8 ? 128 : 256;
          height = format == P4_JPEG_INPUT_GRAY8 ? 32 : 256;
        }
      unsigned quality = test < 6 ? 50 : 90;
      unsigned mx = sampling == P4_JPEG_SAMPLE_420 || sampling == P4_JPEG_SAMPLE_422 ? 16 : 8;
      unsigned my = sampling == P4_JPEG_SAMPLE_420 ? 16 : 8;
      unsigned pw = (width + mx - 1) & ~(mx - 1), ph = (height + my - 1) & ~(my - 1);
      unsigned bytes = format == P4_JPEG_INPUT_RGB565 ? 2 : format == P4_JPEG_INPUT_BGR888 ? 3 : 1;
      size_t capacity = ((size_t)pw * ph * 6 + 4096 + 127) & ~(size_t)127;
      if (capacity > 4 * 1024 * 1024) capacity = 4 * 1024 * 1024;
      uint8_t *input = malloc((size_t)pw * ph * bytes);
      uint8_t *output = memalign(128, capacity);
      uint16_t *decoded = malloc((size_t)width * height * 2);
      if (!input || !output || !decoded) { free(input); free(output); free(decoded); failures++; break; }
      for (unsigned y = 0; y < ph; y++)
        for (unsigned x = 0; x < pw; x++)
          {
            uint8_t rgb[3];
            encode_test_rgb(x < width ? x : width - 1, y < height ? y : height - 1,
                            bytes == 1, test == 15, test >= 16, rgb);
            size_t at = ((size_t)y * pw + x) * bytes;
            if (bytes == 1) input[at] = rgb[0];
            else if (bytes == 3) { input[at] = rgb[2]; input[at+1] = rgb[1]; input[at+2] = rgb[0]; }
            else ((uint16_t *)input)[(size_t)y * pw + x] =
                   (uint16_t)((rgb[0] >> 3) << 11 | (rgb[1] >> 2) << 5 | rgb[2] >> 3);
          }
      memset(output, 0xa5, capacity);
      struct p4_jpeg_encode_s job = {0};
      job.input = input; job.input_size = (size_t)pw * ph * bytes;
      job.output = output; job.output_capacity = capacity;
      job.width = width; job.height = height;
      job.format = format; job.sampling = sampling; job.quality = quality;
      int ret = ioctl(fd, P4_JPEG_ENCODE, (unsigned long)&job);
      if (ret < 0) ret = -errno;
      size_t count = 0; uint64_t sum = 0; unsigned max = 0;
      bool guard = job.output_size <= capacity - 128;
      for (unsigned n = 0; n < 128; n++) if (output[capacity - 128 + n] != 0xa5) guard = false;
      int sw = ret ? ret : encode_sw_decode(output, job.output_size, decoded, width, height);
      if (!sw)
        for (unsigned y = 0; y < height; y++)
          for (unsigned x = 0; x < width; x++)
            {
              if (bytes != 1 && test < 15 &&
                  ((x > 1 && x < width - 2 && (x % 16 < 2 || x % 16 > 13)) ||
                   (y > 1 && y < height - 2 && (y % 16 < 2 || y % 16 > 13)))) continue;
              uint8_t rgb[3]; encode_test_rgb(x, y, bytes == 1, test == 15, test >= 16, rgb);
              uint16_t expected = (uint16_t)((rgb[0] >> 3) << 11 | (rgb[1] >> 2) << 5 | rgb[2] >> 3);
              for (unsigned c = 0; c < 3; c++)
                {
                  unsigned a = encode_channel(decoded[(size_t)y * width + x], c);
                  unsigned b = encode_channel(expected, c);
                  unsigned error = a > b ? a - b : b - a;
                  if (error > max) max = error;
                  sum += error; count++;
                }
            }
      bool ok = !ret && !sw && guard && count && max <= (test >= 16 ? 64u : 32u) &&
                sum <= count * (test >= 16 ? 12u : 8u);
      printf("JPEG_ENCODE case=%u fmt=%u sample=%u quality=%u size=%ux%u bytes=%lu result=%d sw=%d max=%u mean_x100=%lu guard=%u %s\n",
             test,format,sampling,quality,width,height,(unsigned long)job.output_size,ret,sw,max,
             (unsigned long)(count ? sum * 100 / count : 0),guard,ok ? "PASS" : "FAIL");
      if (!ok) { failures++; if (!sw) printf("JPEG_ENCODE first_pixel=%04x\n", decoded[0]); }
      cases++;
      if (test == 14 && ok)
        {
          uint8_t *wrapped = NULL; size_t wrapped_size = 0;
          uint16_t *reference = malloc((size_t)width * height * 2);
          int helper = qpk_jpeg_hw_encode565((uint16_t *)input,width,height,pw,
                                            quality,&wrapped,&wrapped_size);
          int decoded_helper = helper || !reference ? -EIO :
            encode_sw_decode(wrapped,wrapped_size,reference,width,height);
          bool same = !decoded_helper && !memcmp(decoded,reference,(size_t)width*height*2);
          printf("JPEG_ENCODE helper result=%d sw=%d bytes=%lu %s\n",helper,
                 decoded_helper,(unsigned long)wrapped_size,same ? "PASS" : "FAIL");
          if (!same) failures++;
          free(reference); free(wrapped);
        }
      /* Deliberately exhaust a bounded entropy buffer, then the next case
       * must succeed with the same codec/DMA owner and no partial receipt.
       */
      if (test == 0)
        {
          job.output_capacity = 768; job.quality = 100; job.output_size = 123;
          for (size_t n = 0; n < job.input_size; n++) input[n] = (n * 71 + n / 13) & 255;
          int shortret = ioctl(fd, P4_JPEG_ENCODE, (unsigned long)&job);
          if (shortret < 0) shortret = -errno;
          bool bounded = shortret == -ENOSPC && job.output_size == 0;
          printf("JPEG_ENCODE short_output result=%d size=%lu %s\n",shortret,
                 (unsigned long)job.output_size,bounded ? "PASS" : "FAIL");
          if (!bounded) failures++;
        }
      free(decoded); free(output); free(input);
    }
  close(fd);
  printf("JPEG_ENCODE cases=%u failures=%u\n", cases, failures);
  return failures ? -EIO : 0;
}

int qpk_jpeg_file_selftest(const char *path)
{
  if (!path || strncmp(path,"/sdcard/photos/",15)) return -EINVAL;
  int fd = open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
  if (fd < 0) return -errno;
  struct stat st;
  int ret = fstat(fd,&st) ? -errno : 0;
  if (!ret && (!S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 4*1024*1024)) ret = -EFBIG;
  uint8_t *data = ret ? NULL : malloc(st.st_size);
  if (!ret && !data) ret = -ENOMEM;
  size_t count = 0;
  while (!ret && count < (size_t)st.st_size)
    {
      ssize_t n = read(fd,data+count,st.st_size-count);
      if (n <= 0) { ret = n < 0 ? -errno : -EIO; break; }
      count += n;
    }
  close(fd);
  struct encode_sw_s *s = ret ? NULL : calloc(1,sizeof(*s));
  if (!ret && !s) ret = -ENOMEM;
  unsigned width = 0,height = 0;
  if (!ret)
    {
      s->input = data;s->size = count;
      if (qpk_jd_prepare(&s->decoder,encode_sw_input,s->workspace,sizeof(s->workspace),s) != JDR_OK) ret = -EBADMSG;
      else { width=s->decoder.width;height=s->decoder.height; }
      if (!width || !height || width>4096 || height>4096 || (size_t)width*height>4*1024*1024) ret = -EFBIG;
    }
  free(s);
  uint16_t *pixels = ret ? NULL : malloc((size_t)width*height*2);
  if (!ret && !pixels) ret = -ENOMEM;
  if (!ret) ret = encode_sw_decode(data,count,pixels,width,height);
  uint32_t hash = 2166136261u;
  if (!ret)
    for (size_t i=0;i<(size_t)width*height*2;i++) hash=(hash^((uint8_t *)pixels)[i])*16777619u;
  printf("JPEG_FILE result=%d bytes=%lu width=%u height=%u pixel_hash=%08lx\n",
         ret,(unsigned long)count,width,height,(unsigned long)(ret ? 0 : hash));
  free(pixels);free(data);
  return ret;
}
#else
int qpk_jpeg_hw_encode565(const uint16_t *input, unsigned width,
                          unsigned height, unsigned stride, unsigned quality,
                          uint8_t **output, size_t *size)
{
  (void)input; (void)width; (void)height; (void)stride; (void)quality;
  if (output) *output = NULL;
  if (size) *size = 0;
  return -ENOSYS;
}
int qpk_jpeg_encode_selftest(void) { return -ENOSYS; }
int qpk_jpeg_file_selftest(const char *path) { (void)path; return -ENOSYS; }
#endif
