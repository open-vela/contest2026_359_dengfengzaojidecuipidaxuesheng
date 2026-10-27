/****************************************************************************
 * arch/risc-v/src/esp32p4/esp32p4_jpeg_parse.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include "esp32p4_jpeg_internal.h"
#include "esp32p4_jpeg_tables.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint16_t jpeg_be16(FAR const uint8_t *p)
{
  return ((uint16_t)p[0] << 8) | p[1];
}

static int jpeg_quant(FAR const uint8_t *p, size_t n,
                      FAR struct p4_jpeg_header_s *h)
{
  unsigned id;
  unsigned i;

  if (n == 0)
    {
      return -EBADMSG;
    }

  while (n != 0)
    {
      if (n < 65)
        {
          return -EBADMSG;
        }

      id = *p++;
      if ((id >> 4) != 0 || (id & 15) >= 3)
        {
          return -ENOTSUP;
        }

      for (i = 0; i < 64; i++)
        {
          if (p[i] == 0)
            {
              return -EBADMSG;
            }

          h->quant[id][g_jpeg_zigzag_arr[i]] = p[i];
        }

      h->quant_mask |= 1u << id;
      p += 64;
      n -= 65;
    }

  return OK;
}

static int jpeg_huffman(FAR const uint8_t *p, size_t n,
                        FAR struct p4_jpeg_header_s *h)
{
  uint8_t seen[32];
  unsigned type;
  unsigned id;
  unsigned total;
  unsigned slots;
  unsigned i;
  unsigned symbol;

  if (n == 0)
    {
      return -EBADMSG;
    }

  while (n != 0)
    {
      if (n < 17)
        {
          return -EBADMSG;
        }

      type = p[0] >> 4;
      id = p[0] & 15;
      if (type > 1 || id > 1)
        {
          return -ENOTSUP;
        }

      total = 0;
      slots = 1;
      for (i = 1; i <= 16; i++)
        {
          slots *= 2;
          if (p[i] > slots)
            {
              return -EBADMSG;
            }

          slots -= p[i];
          total += p[i];
        }

      /* JPEG reserves the all-one code; an exhausted tree is invalid. */

      if (slots == 0 || total == 0 || total > (type ? 162 : 12) ||
          total > n - 17)
        {
          return -EBADMSG;
        }

      memset(seen, 0, sizeof(seen));
      for (i = 0; i < total; i++)
        {
          symbol = p[17 + i];
          if ((!type && symbol > 11) ||
              (type && ((symbol & 15) > 10 ||
               ((symbol & 15) == 0 && symbol != 0 && symbol != 0xf0))) ||
              (seen[symbol >> 3] & (1u << (symbol & 7))))
            {
              return -EBADMSG;
            }

          seen[symbol >> 3] |= 1u << (symbol & 7);
        }

      memcpy(h->bits[type][id], p + 1, 16);
      memset(h->values[type][id], 0, 256);
      memcpy(h->values[type][id], p + 17, total);
      if (type)
        {
          h->ac_mask |= 1u << id;
        }
      else
        {
          h->dc_mask |= 1u << id;
        }

      p += 17 + total;
      n -= 17 + total;
    }

  return OK;
}

static int jpeg_frame(FAR const uint8_t *p, size_t n,
                      FAR struct p4_jpeg_header_s *h)
{
  unsigned i;
  unsigned j;
  unsigned mx;
  unsigned my;

  if (h->components != 0 || n < 6)
    {
      return -EBADMSG;
    }

  if (p[0] != 8 || (p[5] != 1 && p[5] != 3))
    {
      return -ENOTSUP;
    }

  h->height = jpeg_be16(p + 1);
  h->width = jpeg_be16(p + 3);
  h->components = p[5];
  if (n != 6 + 3u * h->components || h->height == 0 || h->width == 0)
    {
      return -EBADMSG;
    }

  if (h->width > P4_JPEG_MAX_DIMENSION ||
      h->height > P4_JPEG_MAX_DIMENSION ||
      (uint32_t)h->width * h->height > P4_JPEG_MAX_PIXELS ||
      ((uint32_t)h->width * h->height & 7) != 0)
    {
      return -ENOTSUP;
    }

  for (i = 0; i < h->components; i++)
    {
      h->identifier[i] = p[6 + 3 * i];
      h->horizontal[i] = p[7 + 3 * i] >> 4;
      h->vertical[i] = p[7 + 3 * i] & 15;
      h->quant_id[i] = p[8 + 3 * i];
      if (h->quant_id[i] >= 3)
        {
          return -ENOTSUP;
        }

      for (j = 0; j < i; j++)
        {
          if (h->identifier[j] == h->identifier[i])
            {
              return -EBADMSG;
            }
        }

      if ((i != 0 || h->components == 1) &&
          (h->horizontal[i] != 1 || h->vertical[i] != 1))
        {
          return -ENOTSUP;
        }
    }

  mx = h->horizontal[0];
  my = h->vertical[0];
  if (!((mx == 1 && my == 1) || (mx == 2 && my == 1) ||
        (mx == 2 && my == 2)))
    {
      return -ENOTSUP;
    }

  mx *= 8;
  my *= 8;
  h->padded_width = (h->width + mx - 1) / mx * mx;
  h->padded_height = (h->height + my - 1) / my * my;
  if ((uint32_t)h->padded_width * h->padded_height > P4_JPEG_MAX_PIXELS)
    {
      return -ENOTSUP;
    }

  return OK;
}

static void jpeg_defaults(FAR struct p4_jpeg_header_s *h)
{
  memcpy(h->bits[0][0], g_jpeg_luminance_dc_coefficients, 16);
  memcpy(h->bits[0][1], g_jpeg_chrominance_dc_coefficients, 16);
  memcpy(h->bits[1][0], g_jpeg_luminance_ac_coefficients, 16);
  memcpy(h->bits[1][1], g_jpeg_chrominance_ac_coefficients, 16);
  memcpy(h->values[0][0], g_jpeg_luminance_dc_values, 12);
  memcpy(h->values[0][1], g_jpeg_chrominance_dc_values, 12);
  memcpy(h->values[1][0], g_jpeg_luminance_ac_values, 162);
  memcpy(h->values[1][1], g_jpeg_chrominance_ac_values, 162);
  h->dc_mask = 3;
  h->ac_mask = 3;
}

static int jpeg_scan(FAR const uint8_t *p, size_t n,
                     FAR struct p4_jpeg_header_s *h)
{
  unsigned i;

  if (h->components == 0 || n != 4u + 2u * h->components ||
      p[0] != h->components)
    {
      return -ENOTSUP;
    }

  if (p[n - 3] != 0 || p[n - 2] != 63 || p[n - 1] != 0)
    {
      return -ENOTSUP;
    }

  if (h->dc_mask == 0 && h->ac_mask == 0)
    {
      jpeg_defaults(h);
    }

  for (i = 0; i < h->components; i++)
    {
      if (p[1 + 2 * i] != h->identifier[i])
        {
          return -ENOTSUP;
        }

      h->dc_id[i] = p[2 + 2 * i] >> 4;
      h->ac_id[i] = p[2 + 2 * i] & 15;
      if (h->dc_id[i] > 1 || h->ac_id[i] > 1)
        {
          return -ENOTSUP;
        }

      if (!(h->quant_mask & (1u << h->quant_id[i])) ||
          !(h->dc_mask & (1u << h->dc_id[i])) ||
          !(h->ac_mask & (1u << h->ac_id[i])))
        {
          return -EBADMSG;
        }
    }

  if (h->components == 3 && h->adobe_transform >= 0 &&
      h->adobe_transform != 1)
    {
      return -ENOTSUP;
    }

  return OK;
}

static int jpeg_entropy(FAR const uint8_t *data, size_t length, size_t pos,
                        FAR struct p4_jpeg_header_s *h)
{
  unsigned restart = 0;
  uint8_t marker;

  while (pos < length)
    {
      if (data[pos++] != 0xff)
        {
          continue;
        }

      while (pos < length && data[pos] == 0xff)
        {
          pos++;
        }

      if (pos == length)
        {
          return -EBADMSG;
        }

      marker = data[pos++];
      if (marker == 0)
        {
          continue;
        }

      if (marker == 0xd9)
        {
          h->scan_length = pos - h->scan_offset;
          return OK;
        }

      if (marker >= 0xd0 && marker <= 0xd7)
        {
          if (h->restart_interval == 0 || marker != 0xd0 + restart)
            {
              return -EBADMSG;
            }

          restart = (restart + 1) & 7;
          continue;
        }

      return -ENOTSUP;
    }

  return -EBADMSG;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int esp32p4_jpeg_parse(FAR const uint8_t *data, size_t length,
                      FAR struct p4_jpeg_header_s *h)
{
  size_t pos = 2;
  size_t segment;
  size_t marker_pos;
  uint8_t marker;
  FAR const uint8_t *p;
  size_t n;
  int ret;

  if (data == NULL || h == NULL || length < 4 ||
      length > P4_JPEG_MAX_STREAM)
    {
      return -EINVAL;
    }

  memset(h, 0, sizeof(*h));
  h->adobe_transform = -1;
  if (data[0] != 0xff || data[1] != 0xd8)
    {
      return -EBADMSG;
    }

  while (pos < length)
    {
      if (data[pos++] != 0xff)
        {
          return -EBADMSG;
        }

      while (pos < length && data[pos] == 0xff)
        {
          pos++;
        }

      if (pos == length)
        {
          return -EBADMSG;
        }

      marker = data[pos++];
      marker_pos = pos - 2;
      if (length - pos < 2)
        {
          return -EBADMSG;
        }

      segment = jpeg_be16(data + pos);
      if (segment < 2 || segment > length - pos)
        {
          return -EBADMSG;
        }

      p = data + pos + 2;
      n = segment - 2;
      pos += segment;
      switch (marker)
        {
          case 0xdb:
            ret = jpeg_quant(p, n, h);
            break;

          case 0xc4:
            ret = jpeg_huffman(p, n, h);
            break;

          case 0xc0:
            ret = jpeg_frame(p, n, h);
            break;

          case 0xdd:
            if (n != 2)
              {
                return -EBADMSG;
              }

            h->restart_interval = jpeg_be16(p);
            ret = OK;
            break;

          case 0xda:
            ret = jpeg_scan(p, n, h);
            if (ret < 0)
              {
                return ret;
              }

            h->scan_offset = marker_pos;
            return jpeg_entropy(data, length, pos, h);

          default:
            if ((marker >= 0xe0 && marker <= 0xef) || marker == 0xfe)
              {
                if (marker == 0xee && n >= 12 && !memcmp(p, "Adobe", 5))
                  {
                    if (p[11] > 2)
                      {
                        return -ENOTSUP;
                      }

                    h->adobe_transform = p[11];
                  }

                ret = OK;
              }
            else
              {
                return -ENOTSUP;
              }

            break;
        }

      if (ret < 0)
        {
          return ret;
        }
    }

  return -EBADMSG;
}
