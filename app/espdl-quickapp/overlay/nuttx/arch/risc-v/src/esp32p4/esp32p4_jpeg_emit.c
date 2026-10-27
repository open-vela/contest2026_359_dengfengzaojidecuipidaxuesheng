/****************************************************************************
 * arch/risc-v/src/esp32p4/esp32p4_jpeg_emit.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bounded baseline/JFIF header emission. Standard tables and quality scaling
 * follow Espressif upper_hal_jpeg (Apache-2.0) and ISO/IEC 10918-1 Annex K.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <errno.h>
#include <stdbool.h>
#include "esp32p4_jpeg_emit.h"
#include "esp32p4_jpeg_tables.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct jpeg_writer_s
{
  FAR uint8_t *buffer;
  size_t capacity;
  size_t pos;
  int error;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void jpeg_byte(FAR struct jpeg_writer_s *w, unsigned value)
{
  if (w->pos < w->capacity)
    {
      w->buffer[w->pos++] = value;
    }
  else
    {
      w->error = -ENOSPC;
    }
}

static void jpeg_word(FAR struct jpeg_writer_s *w, unsigned value)
{
  jpeg_byte(w, value >> 8);
  jpeg_byte(w, value);
}

static void jpeg_marker(FAR struct jpeg_writer_s *w, unsigned marker)
{
  jpeg_byte(w, 255);
  jpeg_byte(w, marker);
}

static void jpeg_huffman(FAR struct jpeg_writer_s *w, unsigned id,
                          FAR const uint8_t *bits,
                          FAR const uint8_t *values)
{
  unsigned count = 0;
  unsigned i;

  for (i = 0; i < 16; i++)
    {
      count += bits[i];
    }

  jpeg_marker(w, 0xc4);
  jpeg_word(w, count + 19);
  jpeg_byte(w, id);
  for (i = 0; i < 16; i++)
    {
      jpeg_byte(w, bits[i]);
    }

  for (i = 0; i < count; i++)
    {
      jpeg_byte(w, values[i]);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int esp32p4_jpeg_emit(FAR const struct p4_jpeg_encode_s *job,
                     FAR uint32_t quant[2][64], FAR size_t *length)
{
  struct jpeg_writer_s w;
  unsigned components;
  unsigned scale;
  unsigned sample;
  unsigned padding;
  unsigned i;
  unsigned id;

  if (length == NULL)
    {
      return -EINVAL;
    }

  *length = 0;
  if (job == NULL || quant == NULL || job->output == NULL ||
      job->width == 0 || job->height == 0 || job->width > 4096 ||
      job->height > 4096 || job->quality == 0 || job->quality > 100 ||
      (unsigned)job->sampling > P4_JPEG_SAMPLE_GRAY)
    {
      return -EINVAL;
    }

  w.buffer = job->output;
  w.capacity = job->output_capacity;
  w.pos = 0;
  w.error = 0;
  components = job->sampling == P4_JPEG_SAMPLE_GRAY ? 1 : 3;
  scale = job->quality < 50 ? 5000 / job->quality :
          200 - 2 * job->quality;
  sample = job->sampling == P4_JPEG_SAMPLE_420 ? 0x22 :
           job->sampling == P4_JPEG_SAMPLE_422 ? 0x21 : 0x11;
  jpeg_marker(&w, 0xd8);
  jpeg_marker(&w, 0xe0);
  jpeg_word(&w, 16);
  jpeg_byte(&w, 'J');
  jpeg_byte(&w, 'F');
  jpeg_byte(&w, 'I');
  jpeg_byte(&w, 'F');
  jpeg_byte(&w, 0);
  jpeg_word(&w, 0x0101);
  jpeg_byte(&w, 0);
  jpeg_word(&w, 1);
  jpeg_word(&w, 1);
  jpeg_word(&w, 0);

  for (id = 0; id < 2; id++)
    {
      FAR const uint8_t *basic = id ?
        g_jpeg_chrominance_quantization_table :
        g_jpeg_luminance_quantization_table;

      for (i = 0; i < 64; i++)
        {
          unsigned value = (basic[i] * scale + 50) / 100;
          quant[id][i] = value < 1 ? 1 : value > 255 ? 255 : value;
        }

      if (id == 1 && components == 1)
        {
          continue;
        }

      jpeg_marker(&w, 0xdb);
      jpeg_word(&w, 67);
      jpeg_byte(&w, id);
      for (i = 0; i < 64; i++)
        {
          jpeg_byte(&w, quant[id][g_jpeg_zigzag_arr[i]]);
        }
    }

  jpeg_marker(&w, 0xc0);
  jpeg_word(&w, 8 + 3 * components);
  jpeg_byte(&w, 8);
  jpeg_word(&w, job->height);
  jpeg_word(&w, job->width);
  jpeg_byte(&w, components);
  for (i = 0; i < components; i++)
    {
      jpeg_byte(&w, i + 1);
      jpeg_byte(&w, i == 0 ? sample : 0x11);
      jpeg_byte(&w, i == 0 ? 0 : 1);
    }

  jpeg_huffman(&w, 0, g_jpeg_luminance_dc_coefficients,
                g_jpeg_luminance_dc_values);
  jpeg_huffman(&w, 0x10, g_jpeg_luminance_ac_coefficients,
                g_jpeg_luminance_ac_values);
  if (components == 3)
    {
      jpeg_huffman(&w, 1, g_jpeg_chrominance_dc_coefficients,
                    g_jpeg_chrominance_dc_values);
      jpeg_huffman(&w, 0x11, g_jpeg_chrominance_ac_coefficients,
                    g_jpeg_chrominance_ac_values);
    }

  /* Keep the CPU-written header on separate cache lines from DMA entropy. */

  padding = (128 - ((w.pos + 4 + 8 + 2 * components) & 127)) & 127;
  jpeg_marker(&w, 0xfe);
  jpeg_word(&w, padding + 2);
  for (i = 0; i < padding; i++)
    {
      jpeg_byte(&w, 0);
    }

  jpeg_marker(&w, 0xda);
  jpeg_word(&w, 6 + 2 * components);
  jpeg_byte(&w, components);
  for (i = 0; i < components; i++)
    {
      jpeg_byte(&w, i + 1);
      jpeg_byte(&w, i == 0 ? 0 : 0x11);
    }

  jpeg_byte(&w, 0);
  jpeg_byte(&w, 63);
  jpeg_byte(&w, 0);
  if (w.error != 0)
    {
      return w.error;
    }

  *length = w.pos;
  return OK;
}
