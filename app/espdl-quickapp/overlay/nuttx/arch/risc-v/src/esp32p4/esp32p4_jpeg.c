/****************************************************************************
 * arch/risc-v/src/esp32p4/esp32p4_jpeg.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Native synchronous JPEG decode. Register/descriptor sequencing follows
 * Espressif esp-hal-3rdparty upper_hal_jpeg and upper_hal_dma (Apache-2.0).
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_ESP32P4_JPEG

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <syslog.h>
#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/fs/fs.h>
#include <nuttx/irq.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#include <nuttx/spinlock.h>
#include <nuttx/video/p4_jpeg.h>

#include "esp_cache.h"
#include "esp_memory_utils.h"
#include "esp_irq.h"
#include "hal/dma2d_ll.h"
#include "hal/jpeg_hal.h"
#include "hal/jpeg_ll.h"
#include "soc/dma2d_channel.h"
#include "soc/interrupts.h"
#include "esp32p4_dma2d.h"
#include "esp32p4_jpeg_internal.h"
#include "esp32p4_jpeg_emit.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct p4_jpeg_dev_s
{
  mutex_t lock;
  spinlock_t irq_lock;
  sem_t done;
  jpeg_hal_context_t hal;
  FAR struct p4_jpeg_header_s *header;
  FAR uint32_t *scratch;
  FAR dma2d_descriptor_t *tx;
  FAR dma2d_descriptor_t *rx;
  uint32_t jpeg_status;
  uint32_t dma_status;
  uint32_t event_mask;
  bool active;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* One physical codec/IRQ, shared by all opens of the registered device. */

static mutex_t g_jpeg_initlock = NXMUTEX_INITIALIZER;
static FAR struct p4_jpeg_dev_s *g_jpeg;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool jpeg_ram(FAR const void *buffer, size_t size)
{
  uintptr_t start = (uintptr_t)buffer;
  FAR const void *last;

  if (buffer == NULL || size == 0 || start > UINTPTR_MAX - size)
    {
      return false;
    }

  last = (FAR const void *)(start + size - 1);
  return (esp_ptr_internal(buffer) && esp_ptr_internal(last)) ||
         (esp_ptr_external_ram(buffer) && esp_ptr_external_ram(last));
}

static void jpeg_dma_interrupt(FAR void *arg, uint32_t status)
{
  FAR struct p4_jpeg_dev_s *dev = arg;
  irqstate_t flags = spin_lock_irqsave(&dev->irq_lock);

  if (dev->active)
    {
      dev->dma_status |= status;
      nxsem_post(&dev->done);
    }

  spin_unlock_irqrestore(&dev->irq_lock, flags);
}

static int jpeg_interrupt(int irq, FAR void *context, FAR void *arg)
{
  FAR struct p4_jpeg_dev_s *dev = arg;
  irqstate_t flags = spin_lock_irqsave(&dev->irq_lock);
  uint32_t status = jpeg_ll_get_intr_status(dev->hal.dev);

  jpeg_ll_clear_intr_mask(dev->hal.dev, status);
  if (dev->active && (status & dev->event_mask) != 0)
    {
      dev->jpeg_status |= status & dev->event_mask;
      nxsem_post(&dev->done);
    }

  spin_unlock_irqrestore(&dev->irq_lock, flags);
  return OK;
}

static void jpeg_clock_init(FAR struct p4_jpeg_dev_s *dev)
{
  irqstate_t flags;
  int __DECLARE_RCC_ATOMIC_ENV = 0;

  flags = enter_critical_section();
  jpeg_ll_enable_bus_clock(true);
  jpeg_ll_reset_module_register();
  leave_critical_section(flags);
  jpeg_hal_init(&dev->hal);
  jpeg_ll_disable_intr_mask(dev->hal.dev, UINT32_MAX);
  jpeg_ll_clear_intr_mask(dev->hal.dev, UINT32_MAX);
}

static void jpeg_info(FAR struct p4_jpeg_info_s *info,
                      FAR const struct p4_jpeg_header_s *h)
{
  info->width = h->width;
  info->height = h->height;
  info->padded_width = h->padded_width;
  info->padded_height = h->padded_height;
  info->components = h->components;
}

static void jpeg_tables(FAR struct p4_jpeg_dev_s *dev)
{
  FAR struct p4_jpeg_header_s *h = dev->header;
  unsigned i;
  unsigned id;

  /* A new lease also resets the codec registers: format changes and an
   * aborted scan must not inherit the previous output/FIFO state.
   */

  jpeg_clock_init(dev);
  jpeg_ll_set_codec_mode(dev->hal.dev, JPEG_CODEC_DECODER);
  jpeg_ll_set_picture_height(dev->hal.dev, 0);
  jpeg_ll_set_picture_width(dev->hal.dev, 0);
  for (i = 0; i < 3; i++)
    {
      if (h->quant_mask & (1u << i))
        {
          dqt_func[i](dev->hal.dev, h->quant[i]);
        }
    }

  jpeg_ll_set_picture_height(dev->hal.dev, h->padded_height);
  jpeg_ll_set_picture_width(dev->hal.dev, h->padded_width);
  jpeg_ll_set_decode_component_num(dev->hal.dev, h->components);
  for (i = 0; i < h->components; i++)
    {
      sof_func[i](dev->hal.dev, h->identifier[i], h->horizontal[i],
                  h->vertical[i], h->quant_id[i]);
    }

  for (i = 0; i < 2; i++)
    {
      for (id = 0; id < 2; id++)
        {
          if ((i ? h->ac_mask : h->dc_mask) & (1u << id))
            {
              dht_func[i][id](&dev->hal, h->bits[i][id],
                              h->values[i][id], dev->scratch);
            }
        }
    }

  jpeg_ll_set_restart_interval(dev->hal.dev, h->restart_interval);
}

static void jpeg_dma_config(FAR struct p4_jpeg_dev_s *dev,
                            FAR const struct p4_jpeg_decode_s *job,
                            unsigned bytes)
{
  FAR struct p4_jpeg_header_s *h = dev->header;
  FAR dma2d_dev_t *dma = DMA2D_LL_GET_HW(0);
  unsigned sample = h->horizontal[0] == 1 ? 0 :
                    h->vertical[0] == 1 ? 1 : 2;
  unsigned hb565[3] =
  {
    32, 64, 48
  };

  unsigned hb888[3] =
  {
    40, 32, 32
  };

  unsigned hb = bytes == 1 ? 96 : bytes == 2 ?
                hb565[sample] : hb888[sample];

  /* The SDK's throughput-oriented block width can exceed a small image.
   * A reorder block must fit its frame. Padded width is already MCU-aligned.
   */

  if (hb > h->padded_width)
    {
      hb = h->padded_width;
    }

  memset(dev->tx, 0, 128);
  memset(dev->rx, 0, 128);
  dev->tx->hb_length = h->scan_length & 0x3fff;
  dev->tx->vb_size = h->scan_length & 0x3fff;
  dev->tx->ha_length = h->scan_length >> 14;
  dev->tx->va_size = h->scan_length >> 14;
  dev->tx->pbyte = DMA2D_DESCRIPTOR_PBYTE_1B0_PER_PIXEL;
  dev->tx->owner = 1;
  dev->tx->buffer = (FAR void *)(job->info.input + h->scan_offset);
  dev->rx->dma2d_en = 1;
  dev->rx->mode = DMA2D_DESCRIPTOR_BLOCK_RW_MODE_MULTIPLE;
  dev->rx->vb_size = h->vertical[0] * 8;
  dev->rx->hb_length = hb;
  dev->rx->va_size = h->padded_height;
  dev->rx->ha_length = h->padded_width;
  dev->rx->pbyte = bytes == 1 ? DMA2D_DESCRIPTOR_PBYTE_1B0_PER_PIXEL :
                   bytes == 2 ? DMA2D_DESCRIPTOR_PBYTE_2B0_PER_PIXEL :
                   DMA2D_DESCRIPTOR_PBYTE_3B0_PER_PIXEL;
  dev->rx->owner = 1;
  dev->rx->buffer = job->output;

  dma2d_ll_tx_connect_to_periph(dma, 0, DMA2D_TRIG_PERIPH_JPEG_DECODER,
                               SOC_DMA2D_TRIG_PERIPH_JPEG_TX);
  dma2d_ll_rx_connect_to_periph(dma, 0, DMA2D_TRIG_PERIPH_JPEG_DECODER,
                               SOC_DMA2D_TRIG_PERIPH_JPEG_RX);
  dma2d_ll_tx_enable_reorder(dma, 0, false);
  dma2d_ll_rx_enable_reorder(dma, 0, true);
  dma2d_ll_tx_enable_dscr_port(dma, 0, false);
  dma2d_ll_rx_enable_dscr_port(dma, 0, false);
  dma2d_ll_tx_enable_owner_check(dma, 0, false);
  dma2d_ll_rx_enable_owner_check(dma, 0, false);
  dma2d_ll_tx_enable_auto_write_back(dma, 0, false);
  dma2d_ll_rx_set_auto_return_owner(dma, 0, 0);
  dma2d_ll_tx_enable_eof_mode(dma, 0, true);
  dma2d_ll_tx_enable_descriptor_burst(dma, 0, true);
  dma2d_ll_rx_enable_descriptor_burst(dma, 0, true);
  dma2d_ll_tx_set_data_burst_length(dma, 0, DMA2D_DATA_BURST_LENGTH_128);
  dma2d_ll_rx_set_data_burst_length(dma, 0, DMA2D_DATA_BURST_LENGTH_128);
  dma2d_ll_tx_enable_page_bound_wrap(dma, 0, true);
  dma2d_ll_rx_enable_page_bound_wrap(dma, 0, true);
  dma2d_ll_tx_set_macro_block_size(dma, 0, DMA2D_MACRO_BLOCK_SIZE_NONE);
  dma2d_ll_rx_set_macro_block_size(dma, 0, sample == 0 ?
    DMA2D_MACRO_BLOCK_SIZE_8_8 : sample == 1 ?
    DMA2D_MACRO_BLOCK_SIZE_8_16 : DMA2D_MACRO_BLOCK_SIZE_16_16);
  dma2d_ll_tx_configure_color_space_conv(dma, 0, DMA2D_CSC_TX_NONE);
  dma2d_ll_rx_configure_color_space_conv(dma, 0, bytes == 1 ?
    DMA2D_CSC_RX_NONE : bytes == 2 ? DMA2D_CSC_RX_YUV420_TO_RGB565_601 :
    DMA2D_CSC_RX_YUV420_TO_RGB888_601);
  dma2d_ll_rx_set_csc_post_scramble(dma, 0, bytes == 2 ?
    DMA2D_SCRAMBLE_ORDER_BYTE2_1_0 : bytes == 3 ?
    DMA2D_SCRAMBLE_ORDER_BYTE0_1_2 : DMA2D_SCRAMBLE_ORDER_BYTE2_1_0);
  if (bytes != 1)
    {
      /* JPEG YCbCr uses full-range samples. The HAL's television BT.601
       * matrix expands limited-range Y (16..235), which clips JPEG colours.
       * Coefficients are Q8, in Y/Cb/Cr/constant order.
       */

      typeof(dma->in_channel[0].in_color_param_group) csc;

      memset(&csc, 0, sizeof(csc));
      csc.param_h.a = 256;
      csc.param_h.b = 0;
      csc.param_h.c = 359;
      csc.param_h.d = -45952;
      csc.param_m.a = 256;
      csc.param_m.b = -88;
      csc.param_m.c = -183;
      csc.param_m.d = 34688;
      csc.param_l.a = 256;
      csc.param_l.b = 454;
      csc.param_l.c = 0;
      csc.param_l.d = -58112;
      dma->in_channel[0].in_color_param_group.param_h.val[0] =
        csc.param_h.val[0];
      dma->in_channel[0].in_color_param_group.param_h.val[1] =
        csc.param_h.val[1];
      dma->in_channel[0].in_color_param_group.param_m.val[0] =
        csc.param_m.val[0];
      dma->in_channel[0].in_color_param_group.param_m.val[1] =
        csc.param_m.val[1];
      dma->in_channel[0].in_color_param_group.param_l.val[0] =
        csc.param_l.val[0];
      dma->in_channel[0].in_color_param_group.param_l.val[1] =
        csc.param_l.val[1];
    }

  dma2d_ll_tx_set_desc_addr(dma, 0, (uintptr_t)dev->tx);
  dma2d_ll_rx_set_desc_addr(dma, 0, (uintptr_t)dev->rx);
}

static int jpeg_decode(FAR struct p4_jpeg_dev_s *dev,
                        FAR struct p4_jpeg_decode_s *job)
{
  FAR struct p4_jpeg_header_s *h = dev->header;
  FAR dma2d_dev_t *dma = DMA2D_LL_GET_HW(0);
  size_t required;
  size_t aligned;
  unsigned bytes;
  unsigned timeout;
  uint32_t jpeg_status;
  uint32_t dma_status;
  irqstate_t flags;
  clock_t ticks;
  int ret;

  job->output_size = 0;
  job->stride = 0;
  if (job->format > P4_JPEG_GRAY8 || job->format < P4_JPEG_RGB565 ||
      job->timeout_ms > 5000)
    {
      return -EINVAL;
    }

  bytes = job->format == P4_JPEG_RGB565 ? 2 :
          job->format == P4_JPEG_RGB888 ? 3 : 1;
  if ((h->components == 1) != (bytes == 1))
    {
      return -ENOTSUP;
    }

  required = (size_t)h->padded_width * h->padded_height * bytes;
  aligned = (required + 127) & ~(size_t)127;
  if (!jpeg_ram(job->output, job->output_capacity) ||
      ((uintptr_t)job->output & 127) != 0 ||
      (job->output_capacity & 127) != 0 ||
      job->output_capacity < aligned ||
      ((uintptr_t)job->info.input < (uintptr_t)job->output + aligned &&
       (uintptr_t)job->output <
       (uintptr_t)job->info.input + job->info.input_size))
    {
      return -EINVAL;
    }

  ret = esp32p4_dma2d_acquire(jpeg_dma_interrupt, dev);
  if (ret < 0)
    {
      return ret;
    }

  jpeg_tables(dev);
  jpeg_dma_config(dev, job, bytes);
  ret = esp_cache_msync((FAR void *)job->info.input, job->info.input_size,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                        ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  if (ret == 0)
    {
      ret = esp_cache_msync(job->output, job->output_capacity,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }

  if (ret == 0)
    {
      ret = esp_cache_msync(dev->tx, 128, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }

  if (ret == 0)
    {
      ret = esp_cache_msync(dev->rx, 128, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }

  if (ret != 0)
    {
      jpeg_ll_soft_rst(dev->hal.dev);
      esp32p4_dma2d_release();
      return -EIO;
    }

  flags = spin_lock_irqsave(&dev->irq_lock);
  while (nxsem_trywait(&dev->done) == OK)
    {
    }

  dev->jpeg_status = 0;
  dev->dma_status = 0;
  dev->active = true;
  dev->event_mask = JPEG_LL_DECODER_EVENT_INTR;
  jpeg_ll_clear_intr_mask(dev->hal.dev, UINT32_MAX);
  jpeg_ll_enable_intr_mask(dev->hal.dev, JPEG_LL_DECODER_EVENT_INTR);
  dma2d_ll_rx_enable_interrupt(dma, 0,
    DMA2D_LL_EVENT_RX_SUC_EOF | DMA2D_LL_EVENT_RX_DESC_ERROR, true);
  spin_unlock_irqrestore(&dev->irq_lock, flags);
  dma2d_ll_tx_start(dma, 0);
  dma2d_ll_rx_start(dma, 0);
  jpeg_ll_process_start(dev->hal.dev);
  timeout = job->timeout_ms ? job->timeout_ms : 250;
  ticks = MSEC2TICK(timeout);
  ret = nxsem_tickwait_uninterruptible(&dev->done, ticks ? ticks : 1);

  flags = spin_lock_irqsave(&dev->irq_lock);
  jpeg_ll_disable_intr_mask(dev->hal.dev, UINT32_MAX);
  jpeg_status = dev->jpeg_status |
                (jpeg_ll_get_intr_status(dev->hal.dev) &
                 JPEG_LL_DECODER_EVENT_INTR);
  dma_status = dev->dma_status | dma->in_channel[0].in_int_raw.val;
  dev->active = false;
  jpeg_ll_soft_rst(dev->hal.dev);
  spin_unlock_irqrestore(&dev->irq_lock, flags);

  /* No device spinlock while draining the shared interrupt callback. Keep
   * the lease until the DMA reset and output cache completion are finished.
   */

  esp32p4_dma2d_stop();
  if (ret == OK && jpeg_status == 0 &&
      (dma_status & DMA2D_LL_EVENT_RX_SUC_EOF) != 0 &&
      (dma_status & DMA2D_LL_EVENT_RX_DESC_ERROR) == 0)
    {
      ret = esp_cache_msync(job->output, job->output_capacity,
                            ESP_CACHE_MSYNC_FLAG_DIR_M2C) == 0 ? OK : -EIO;
    }
  else if (ret == OK)
    {
      ret = -EIO;
    }

  esp32p4_dma2d_release();
  if (ret == OK)
    {
      job->stride = h->padded_width * bytes;
      job->output_size = required;
    }
  else
    {
      syslog(LOG_ERR, "JPEG decode=%d codec=%08lx dma=%08lx\n", ret,
             (unsigned long)jpeg_status, (unsigned long)dma_status);
    }

  return ret;
}

static void jpeg_encode_dma(FAR struct p4_jpeg_dev_s *dev,
                            FAR const struct p4_jpeg_encode_s *job,
                            unsigned width, unsigned height,
                            unsigned bytes, size_t header)
{
  FAR dma2d_dev_t *dma = DMA2D_LL_GET_HW(0);
  size_t capacity = job->output_capacity - header;
  unsigned mx = job->sampling == P4_JPEG_SAMPLE_422 ||
                job->sampling == P4_JPEG_SAMPLE_420 ? 16 : 8;
  unsigned my = job->sampling == P4_JPEG_SAMPLE_420 ? 16 : 8;
  unsigned hb = bytes == 1 ? 128 : bytes == 2 ?
                (job->sampling == P4_JPEG_SAMPLE_420 ? 48 : 64) :
                (job->sampling == P4_JPEG_SAMPLE_444 ? 40 : 32);

  if (hb > width)
    {
      hb = width;
    }

  memset(dev->tx, 0, 128);
  memset(dev->rx, 0, 128);
  dev->tx->dma2d_en = 1;
  dev->tx->mode = DMA2D_DESCRIPTOR_BLOCK_RW_MODE_MULTIPLE;
  dev->tx->vb_size = my;
  dev->tx->hb_length = hb;
  dev->tx->va_size = height;
  dev->tx->ha_length = width;
  dev->tx->pbyte = bytes == 1 ? DMA2D_DESCRIPTOR_PBYTE_1B0_PER_PIXEL :
                   bytes == 2 ? DMA2D_DESCRIPTOR_PBYTE_2B0_PER_PIXEL :
                   DMA2D_DESCRIPTOR_PBYTE_3B0_PER_PIXEL;
  dev->tx->owner = 1;
  dev->tx->buffer = (FAR void *)job->input;
  dev->rx->mode = DMA2D_DESCRIPTOR_BLOCK_RW_MODE_MULTIPLE;
  dev->rx->vb_size = capacity & 0x3fff;
  dev->rx->va_size = capacity >> 14;
  dev->rx->pbyte = DMA2D_DESCRIPTOR_PBYTE_1B0_PER_PIXEL;
  dev->rx->owner = 1;
  dev->rx->buffer = job->output + header;

  dma2d_ll_tx_connect_to_periph(dma, 0, DMA2D_TRIG_PERIPH_JPEG_ENCODER,
                               SOC_DMA2D_TRIG_PERIPH_JPEG_TX);
  dma2d_ll_rx_connect_to_periph(dma, 0, DMA2D_TRIG_PERIPH_JPEG_ENCODER,
                               SOC_DMA2D_TRIG_PERIPH_JPEG_RX);
  dma2d_ll_tx_enable_reorder(dma, 0, true);
  dma2d_ll_rx_enable_reorder(dma, 0, false);
  dma2d_ll_tx_enable_dscr_port(dma, 0, false);
  dma2d_ll_rx_enable_dscr_port(dma, 0, false);
  dma2d_ll_tx_enable_owner_check(dma, 0, false);
  dma2d_ll_rx_enable_owner_check(dma, 0, false);
  dma2d_ll_tx_enable_auto_write_back(dma, 0, false);
  dma2d_ll_rx_set_auto_return_owner(dma, 0, 0);
  dma2d_ll_tx_enable_eof_mode(dma, 0, true);
  dma2d_ll_tx_enable_descriptor_burst(dma, 0, true);
  dma2d_ll_rx_enable_descriptor_burst(dma, 0, true);
  dma2d_ll_tx_set_data_burst_length(dma, 0, DMA2D_DATA_BURST_LENGTH_128);
  dma2d_ll_rx_set_data_burst_length(dma, 0, DMA2D_DATA_BURST_LENGTH_128);
  dma2d_ll_tx_enable_page_bound_wrap(dma, 0, true);
  dma2d_ll_rx_enable_page_bound_wrap(dma, 0, true);
  dma2d_ll_tx_set_macro_block_size(dma, 0, mx == 8 ?
    DMA2D_MACRO_BLOCK_SIZE_8_8 : my == 8 ? DMA2D_MACRO_BLOCK_SIZE_8_16 :
    DMA2D_MACRO_BLOCK_SIZE_16_16);
  dma2d_ll_rx_set_macro_block_size(dma, 0, DMA2D_MACRO_BLOCK_SIZE_NONE);
  dma2d_ll_tx_configure_color_space_conv(dma, 0, DMA2D_CSC_TX_NONE);
  dma2d_ll_rx_configure_color_space_conv(dma, 0, DMA2D_CSC_RX_NONE);
  dma2d_ll_tx_set_desc_addr(dma, 0, (uintptr_t)dev->tx);
  dma2d_ll_rx_set_desc_addr(dma, 0, (uintptr_t)dev->rx);
}

static int jpeg_encode(FAR struct p4_jpeg_dev_s *dev,
                        FAR struct p4_jpeg_encode_s *job)
{
  FAR dma2d_dev_t *dma = DMA2D_LL_GET_HW(0);
  unsigned bytes;
  unsigned mx;
  unsigned my;
  unsigned width;
  unsigned height;
  size_t input_size;
  size_t header;
  size_t capacity;
  size_t entropy;
  uint32_t jpeg_status;
  uint32_t dma_status;
  irqstate_t flags;
  clock_t ticks;
  int ret;

  job->output_size = 0;
  if (job->width == 0 || job->height == 0 || job->width > 4096 ||
      job->height > 4096 || (unsigned)job->format > P4_JPEG_INPUT_GRAY8 ||
      (unsigned)job->sampling > P4_JPEG_SAMPLE_GRAY ||
      job->quality == 0 || job->quality > 100 || job->timeout_ms > 5000 ||
      ((job->format == P4_JPEG_INPUT_GRAY8) !=
       (job->sampling == P4_JPEG_SAMPLE_GRAY)))
    {
      return -EINVAL;
    }

  bytes = job->format == P4_JPEG_INPUT_GRAY8 ? 1 :
          job->format == P4_JPEG_INPUT_RGB565 ? 2 : 3;
  mx = job->sampling == P4_JPEG_SAMPLE_422 ||
       job->sampling == P4_JPEG_SAMPLE_420 ? 16 : 8;
  my = job->sampling == P4_JPEG_SAMPLE_420 ? 16 : 8;
  width = (job->width + mx - 1) & ~(mx - 1);
  height = (job->height + my - 1) & ~(my - 1);
  input_size = (size_t)width * height * bytes;
  if ((size_t)width * height > P4_JPEG_MAX_PIXELS ||
      job->input_size < input_size ||
      !jpeg_ram(job->input, job->input_size) ||
      !jpeg_ram(job->output, job->output_capacity) ||
      ((uintptr_t)job->output & 127) != 0 ||
      (job->output_capacity & 127) != 0 ||
      job->output_capacity > P4_JPEG_MAX_STREAM ||
      ((uintptr_t)job->input <
       (uintptr_t)job->output + job->output_capacity &&
       (uintptr_t)job->output < (uintptr_t)job->input + job->input_size))
    {
      return -EINVAL;
    }

  ret = esp32p4_jpeg_emit(job, dev->header->quant, &header);
  if (ret < 0)
    {
      return ret;
    }

  if (job->output_capacity < header + 128)
    {
      return -ENOSPC;
    }

  capacity = job->output_capacity - header;
  ret = esp32p4_dma2d_acquire(jpeg_dma_interrupt, dev);
  if (ret < 0)
    {
      return ret;
    }

  jpeg_clock_init(dev);
  jpeg_ll_set_codec_mode(dev->hal.dev, JPEG_CODEC_ENCODER);
  jpeg_ll_config_picture_pixel_format(dev->hal.dev, bytes == 1 ?
    JPEG_ENC_SRC_GRAY : bytes == 2 ? JPEG_ENC_SRC_RGB565 :
    JPEG_ENC_SRC_RGB888);
  if (bytes != 1)
    {
      jpeg_ll_sample_mode_select(dev->hal.dev,
        job->sampling == P4_JPEG_SAMPLE_420 ? JPEG_SAMPLE_MODE_YUV420 :
        job->sampling == P4_JPEG_SAMPLE_422 ? JPEG_SAMPLE_MODE_YUV422 :
        JPEG_SAMPLE_MODE_YUV444);
    }

  jpeg_ll_set_picture_width(dev->hal.dev, width);
  jpeg_ll_set_picture_height(dev->hal.dev, height);
  jpeg_ll_pixel_reverse(dev->hal.dev, false);
  jpeg_ll_add_tail(dev->hal.dev, true);
  jpeg_ll_enable_ff_check(dev->hal.dev, true);
  jpeg_ll_set_qnr_presition(dev->hal.dev, 0);
  jpeg_hal_set_quantization_coefficient(&dev->hal, dev->header->quant[0],
                                        dev->header->quant[1]);
  jpeg_encode_dma(dev, job, width, height, bytes, header);

  ret = esp_cache_msync((FAR void *)job->input, input_size,
    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  if (ret == 0)
    {
      ret = esp_cache_msync(job->output, job->output_capacity,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }

  if (ret == 0)
    {
      ret = esp_cache_msync(dev->tx, 128, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }

  if (ret == 0)
    {
      ret = esp_cache_msync(dev->rx, 128, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }

  if (ret != 0)
    {
      jpeg_ll_soft_rst(dev->hal.dev);
      esp32p4_dma2d_release();
      return -EIO;
    }

  flags = spin_lock_irqsave(&dev->irq_lock);
  while (nxsem_trywait(&dev->done) == OK)
    {
    }

  dev->jpeg_status = 0;
  dev->dma_status = 0;
  dev->event_mask = JPEG_LL_ENCODER_EVENT_INTR;
  dev->active = true;
  jpeg_ll_clear_intr_mask(dev->hal.dev, UINT32_MAX);
  jpeg_ll_enable_intr_mask(dev->hal.dev, dev->event_mask);
  dma2d_ll_rx_enable_interrupt(dma, 0, DMA2D_LL_EVENT_RX_SUC_EOF |
    DMA2D_LL_EVENT_RX_DESC_ERROR | DMA2D_LL_EVENT_RX_DESC_EMPTY, true);
  spin_unlock_irqrestore(&dev->irq_lock, flags);
  dma2d_ll_tx_start(dma, 0);
  dma2d_ll_rx_start(dma, 0);
  jpeg_ll_process_start(dev->hal.dev);
  ticks = MSEC2TICK(job->timeout_ms ? job->timeout_ms : 250);
  ret = nxsem_tickwait_uninterruptible(&dev->done, ticks ? ticks : 1);
  flags = spin_lock_irqsave(&dev->irq_lock);
  jpeg_ll_disable_intr_mask(dev->hal.dev, UINT32_MAX);
  jpeg_status = dev->jpeg_status |
    (jpeg_ll_get_intr_status(dev->hal.dev) & dev->event_mask);
  dma_status = dev->dma_status | dma->in_channel[0].in_int_raw.val;
  dev->active = false;
  jpeg_ll_soft_rst(dev->hal.dev);
  spin_unlock_irqrestore(&dev->irq_lock, flags);
  esp32p4_dma2d_stop();

  if (ret == OK && (dma_status & DMA2D_LL_EVENT_RX_DESC_EMPTY) != 0)
    {
      ret = -ENOSPC;
    }
  else if (ret == OK && (jpeg_status != 0 ||
           (dma_status & DMA2D_LL_EVENT_RX_SUC_EOF) == 0 ||
           (dma_status & DMA2D_LL_EVENT_RX_DESC_ERROR) != 0))
    {
      ret = -EIO;
    }

  if (ret == OK)
    {
      ret = esp_cache_msync(dev->rx, 128,
                           ESP_CACHE_MSYNC_FLAG_DIR_M2C) == 0 &&
            esp_cache_msync(job->output, job->output_capacity,
                           ESP_CACHE_MSYNC_FLAG_DIR_M2C) == 0 ? OK : -EIO;
    }

  if (ret == OK)
    {
      entropy = ((size_t)dev->rx->ha_length << 14) | dev->rx->hb_length;
      if (entropy < 2 || entropy > capacity ||
          job->output[header + entropy - 2] != 0xff ||
          job->output[header + entropy - 1] != 0xd9)
        {
          ret = -EIO;
        }
      else
        {
          job->output_size = header + entropy;
        }
    }

  esp32p4_dma2d_release();
  if (ret < 0)
    {
      syslog(LOG_ERR, "JPEG encode=%d codec=%08lx dma=%08lx\n", ret,
             (unsigned long)jpeg_status, (unsigned long)dma_status);
    }

  return ret;
}

static int jpeg_ioctl(FAR struct file *filep, int cmd, unsigned long arg)
{
  FAR struct p4_jpeg_dev_s *dev = filep->f_inode->i_private;
  FAR struct p4_jpeg_info_s *info = (FAR struct p4_jpeg_info_s *)arg;
  int ret;

  if (cmd == P4_JPEG_ENCODE)
    {
      if (arg == 0)
        {
          return -EINVAL;
        }

      ((FAR struct p4_jpeg_encode_s *)arg)->output_size = 0;
      ret = nxmutex_lock(&dev->lock);
      if (ret == OK)
        {
          ret = jpeg_encode(dev, (FAR struct p4_jpeg_encode_s *)arg);
          nxmutex_unlock(&dev->lock);
        }

      return ret;
    }

  if (cmd != P4_JPEG_INFO && cmd != P4_JPEG_DECODE)
    {
      return -ENOTTY;
    }

  if (info == NULL)
    {
      return -EINVAL;
    }

  info->width = 0;
  info->height = 0;
  info->padded_width = 0;
  info->padded_height = 0;
  info->components = 0;
  if (cmd == P4_JPEG_DECODE)
    {
      FAR struct p4_jpeg_decode_s *job =
        (FAR struct p4_jpeg_decode_s *)arg;
      job->output_size = 0;
      job->stride = 0;
    }

  if (info->input_size > P4_JPEG_MAX_STREAM ||
      !jpeg_ram(info->input, info->input_size))
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&dev->lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp32p4_jpeg_parse(info->input, info->input_size, dev->header);
  if (ret == OK)
    {
      jpeg_info(info, dev->header);
      if (cmd == P4_JPEG_DECODE)
        {
          ret = jpeg_decode(dev, (FAR struct p4_jpeg_decode_s *)arg);
        }
    }

  nxmutex_unlock(&dev->lock);
  return ret;
}

static const struct file_operations g_jpeg_ops =
{
  .ioctl = jpeg_ioctl
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int esp32p4_jpeg_register(FAR const char *path)
{
  FAR struct p4_jpeg_dev_s *dev;
  bool clocked = false;
  irqstate_t flags;
  int cpuint = -1;
  int ret;
  int __DECLARE_RCC_ATOMIC_ENV = 0;

  if (path == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&g_jpeg_initlock);
  if (ret < 0)
    {
      return ret;
    }

  if (g_jpeg != NULL)
    {
      nxmutex_unlock(&g_jpeg_initlock);
      return -EEXIST;
    }

  dev = kmm_zalloc(sizeof(*dev));
  if (dev == NULL)
    {
      nxmutex_unlock(&g_jpeg_initlock);
      return -ENOMEM;
    }

  nxmutex_init(&dev->lock);
  nxsem_init(&dev->done, 0, 0);
  dev->irq_lock = (spinlock_t)SP_UNLOCKED;
  dev->header = kumm_zalloc(sizeof(*dev->header));
  dev->scratch = kumm_zalloc(256 * sizeof(*dev->scratch));
  dev->tx = kmm_memalign(128, 128);
  dev->rx = kmm_memalign(128, 128);
  ret = -ENOMEM;
  if (dev->header != NULL && dev->scratch != NULL &&
      dev->tx != NULL && dev->rx != NULL)
    {
      jpeg_clock_init(dev);
      clocked = true;
      ret = esp_setup_irq(ETS_JPEG_INTR_SOURCE, ESP_IRQ_PRIORITY_DEFAULT,
                          ESP_IRQ_TRIGGER_LEVEL, jpeg_interrupt, dev);
      if (ret >= 0)
        {
          cpuint = ret;
          ret = nxmutex_lock(&dev->lock);
          if (ret == OK)
            {
              ret = register_driver(path, &g_jpeg_ops, 0660, dev);
              if (ret == OK)
                {
                  g_jpeg = dev;
                  up_enable_irq(ESP_SOURCE2IRQ(ETS_JPEG_INTR_SOURCE));
                }

              nxmutex_unlock(&dev->lock);
            }
        }
    }

  if (ret < 0)
    {
      if (cpuint >= 0)
        {
          esp_teardown_irq(ETS_JPEG_INTR_SOURCE, cpuint);
          irq_detach(ESP_SOURCE2IRQ(ETS_JPEG_INTR_SOURCE));
        }

      if (clocked)
        {
          jpeg_ll_disable_intr_mask(dev->hal.dev, UINT32_MAX);
          jpeg_ll_soft_rst(dev->hal.dev);
          flags = enter_critical_section();
          jpeg_ll_enable_bus_clock(false);
          leave_critical_section(flags);
        }

      kumm_free(dev->header);
      kumm_free(dev->scratch);
      kmm_free(dev->tx);
      kmm_free(dev->rx);
      nxsem_destroy(&dev->done);
      nxmutex_destroy(&dev->lock);
      kmm_free(dev);
    }

  nxmutex_unlock(&g_jpeg_initlock);
  return ret;
}

#endif /* CONFIG_ESP32P4_JPEG */
