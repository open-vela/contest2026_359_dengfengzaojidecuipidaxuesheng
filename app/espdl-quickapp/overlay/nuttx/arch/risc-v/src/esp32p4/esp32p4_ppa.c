/****************************************************************************
 * arch/risc-v/src/esp32p4/esp32p4_ppa.c
 *
 * SPDX-License-Identifier: Apache-2.0
 * Synchronous PPA SRM prototype for the v3.x P4 flat build.
 * Register/descriptor sequences follow Espressif upper_hal_ppa/ppa_srm.c
 * and upper_hal_dma/dma2d.c (Apache-2.0). No FreeRTOS dependency.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#ifdef CONFIG_ESP32P4_PPA
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>
#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/fs/fs.h>
#include <nuttx/kmalloc.h>
#include <nuttx/irq.h>
#include <nuttx/mutex.h>
#include <nuttx/signal.h>
#include <nuttx/spinlock.h>
#include <nuttx/semaphore.h>
#include <nuttx/video/p4_ppa.h>
#include "esp_cache.h"
#include "esp_memory_utils.h"
#include "hal/ppa_ll.h"
#include "hal/dma2d_ll.h"
#include "esp32p4_dma2d.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct p4_ppa_dev_s
{
  mutex_t lock;
  FAR dma2d_descriptor_t *tx;
  FAR dma2d_descriptor_t *rx;
  bool initialized;
  sem_t done;
  spinlock_t irq_lock;
  uint32_t status;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void ppa_reset(void)
{
  irqstate_t flags = enter_critical_section();
  int __DECLARE_RCC_ATOMIC_ENV = 0;
  dma2d_ll_enable_bus_clock(0, true);
  dma2d_ll_reset_register(0);
  ppa_ll_enable_bus_clock(true);
  ppa_ll_reset_register();
  leave_critical_section(flags);
  dma2d_ll_hw_enable(DMA2D_LL_GET_HW(0), true);
}

static void ppa_interrupt(FAR void *arg, uint32_t status)
{
  FAR struct p4_ppa_dev_s *dev = arg;
  irqstate_t flags = spin_lock_irqsave(&dev->irq_lock);
  if (status & (DMA2D_LL_EVENT_RX_SUC_EOF | DMA2D_LL_EVENT_RX_DESC_ERROR))
    {
      dev->status = status;
      nxsem_post(&dev->done);
    }

  spin_unlock_irqrestore(&dev->irq_lock, flags);
}

static bool ppa_ram(FAR const void *p, size_t n)
{
  uintptr_t first = (uintptr_t)p;
  if (!p || n == 0 || first > UINTPTR_MAX - n ||
      (first & 127) || (n & 127))
    {
      return false;
    }

  FAR const void *last = (FAR const void *)(first + n - 1);
  return (esp_ptr_internal(p) && esp_ptr_internal(last)) ||
         (esp_ptr_external_ram(p) && esp_ptr_external_ram(last));
}

static int ppa_run(FAR struct p4_ppa_dev_s *dev,
                   FAR const struct p4_ppa_job_s *j,
                   ppa_srm_color_mode_t input_format)
{
  FAR dma2d_dev_t *dma = DMA2D_LL_GET_HW(0);
  FAR ppa_dev_t *ppa = PPA_LL_GET_HW;
  uint32_t ow;
  uint32_t oh;
  uint32_t bh;
  uint32_t bv;
  uint32_t status;
  int ret;

  if (!j || j->width < 2 || j->width > 1280 ||
      (input_format == PPA_SRM_COLOR_MODE_YUV422_UYVY && (j->width & 1)) ||
      j->height < 2 || j->height > 720 || j->out_width < 2 ||
      j->out_width > 1280 || j->out_height < 2 ||
      j->out_height > 720 || j->scale256 < 16 || j->scale256 > 256 ||
      (j->scale256 & 15) ||
      !ppa_ram(j->input, j->input_size) ||
      !ppa_ram(j->output, j->output_size) ||
      j->input_size < (size_t)j->width * j->height * 2 ||
      j->output_size < (size_t)j->out_width * j->out_height * 2)
    {
      return -EINVAL;
    }

  ow = j->width * j->scale256 / 256;
  oh = j->height * j->scale256 / 256;
  if (!ow || !oh || ow + j->out_x > j->out_width ||
      oh + j->out_y > j->out_height ||
      ((uintptr_t)j->input < (uintptr_t)j->output + j->output_size &&
       (uintptr_t)j->output < (uintptr_t)j->input + j->input_size))
    {
      return -EINVAL;
    }

  if (!dev->initialized)
    {
      ppa_reset();
      dev->initialized = true;
    }

  ppa_ll_srm_reset(ppa);
  ppa->int_ena.val = 0;
  ppa->int_clr.val = UINT32_MAX;
  memset(dev->tx, 0, 128);
  memset(dev->rx, 0, 128);
  dev->tx->vb_size = j->height;
  dev->tx->hb_length = j->width;
  dev->tx->va_size = j->height;
  dev->tx->ha_length = j->width;
  dev->tx->dma2d_en = 1;
  dev->tx->suc_eof = 1;
  dev->tx->owner = 1;
  dev->tx->pbyte = DMA2D_DESCRIPTOR_PBYTE_2B0_PER_PIXEL;
  dev->tx->buffer = (FAR void *)j->input;
  dev->rx->vb_size = 2;
  dev->rx->hb_length = 2;
  dev->rx->va_size = j->out_height;
  dev->rx->ha_length = j->out_width;
  dev->rx->dma2d_en = 1;
  dev->rx->suc_eof = 1;
  dev->rx->owner = 1;
  dev->rx->pbyte = DMA2D_DESCRIPTOR_PBYTE_2B0_PER_PIXEL;
  dev->rx->x = j->out_x;
  dev->rx->y = j->out_y;
  dev->rx->buffer = j->output;

  /* Preserve letterbox contents before invalidating the DMA destination. */

  ret = esp_cache_msync((FAR void *)j->input, j->input_size,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  if (ret == 0)
    {
      ret = esp_cache_msync(j->output, j->output_size,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }

  if (ret != 0)
    {
      return -EIO;
    }

  ret = esp_cache_msync(dev->tx, 128, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  if (ret == 0)
    {
      ret = esp_cache_msync(dev->rx, 128, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }

  if (ret != 0)
    {
      return -EIO;
    }
  dma2d_ll_tx_stop(dma, 0);
  dma2d_ll_rx_stop(dma, 0);
  dma2d_ll_tx_reset_channel(dma, 0);
  dma2d_ll_rx_reset_channel(dma, 0);
  dma2d_ll_tx_connect_to_periph(dma, 0, DMA2D_TRIG_PERIPH_PPA_SRM,
                               SOC_DMA2D_TRIG_PERIPH_PPA_SRM_TX);
  dma2d_ll_rx_connect_to_periph(dma, 0, DMA2D_TRIG_PERIPH_PPA_SRM,
                               SOC_DMA2D_TRIG_PERIPH_PPA_SRM_RX);
  dma2d_ll_tx_enable_reorder(dma, 0, false);
  dma2d_ll_rx_enable_reorder(dma, 0, false);
  dma2d_ll_tx_enable_dscr_port(dma, 0, true);
  dma2d_ll_rx_enable_dscr_port(dma, 0, true);
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
  dma2d_ll_rx_set_macro_block_size(dma, 0, DMA2D_MACRO_BLOCK_SIZE_NONE);
  dma2d_ll_tx_configure_color_space_conv(dma, 0, DMA2D_CSC_TX_NONE);
  dma2d_ll_rx_configure_color_space_conv(dma, 0, DMA2D_CSC_RX_NONE);
  dma2d_ll_tx_enable_interrupt(dma, 0, UINT32_MAX, false);
  dma2d_ll_rx_enable_interrupt(dma, 0, UINT32_MAX, false);
  dma2d_ll_tx_clear_interrupt_status(dma, 0, UINT32_MAX);
  dma2d_ll_rx_clear_interrupt_status(dma, 0, UINT32_MAX);

  ppa_ll_srm_set_mb_size(ppa, PPA_LL_SRM_MB_SIZE_16_16);
  ppa_ll_srm_get_dma_dscr_port_mode_block_size(ppa,
    input_format, PPA_LL_SRM_MB_SIZE_16_16, &bh, &bv);
  dma2d_ll_tx_set_dscr_port_block_size(dma, 0, bh, bv);
  ppa_ll_srm_set_rx_color_mode(ppa, input_format);
  ppa_ll_srm_set_rx_yuv_range(ppa, PPA_COLOR_RANGE_LIMIT);
  ppa_ll_srm_set_rx_yuv2rgb_std(ppa, PPA_COLOR_CONV_STD_RGB_YUV_BT601);
  ppa_ll_srm_enable_rx_byte_swap(ppa, false);
  ppa_ll_srm_enable_rx_rgb_swap(ppa, false);
  ppa_ll_srm_configure_rx_alpha(ppa, PPA_ALPHA_NO_CHANGE, 255);
  ppa_ll_srm_set_tx_color_mode(ppa, PPA_SRM_COLOR_MODE_RGB565);
  ppa_ll_srm_set_rotation_angle(ppa, PPA_SRM_ROTATION_ANGLE_0);
  ppa_ll_srm_set_scaling_x(ppa, j->scale256 / 256,
                          (j->scale256 % 256) / 16);
  ppa_ll_srm_set_scaling_y(ppa, j->scale256 / 256,
                          (j->scale256 % 256) / 16);
  ppa_ll_srm_enable_mirror_x(ppa, false);
  ppa_ll_srm_enable_mirror_y(ppa, false);

  /* DIG-734: last macroblock may be smaller than the DMA FIFO. */

  uint32_t wl = ow % 64 ? ow % 64 : 64;
  uint32_t hl = j->height % 16 ? j->height % 16 : 16;
  hl = hl * j->scale256 / 256;
  ppa_ll_srm_bypass_mb_order(ppa,
    (ow > 64 || j->height > 16) && wl * hl * 16 < 1536);
  dma2d_ll_tx_set_desc_addr(dma, 0, (uintptr_t)dev->tx);
  dma2d_ll_rx_set_desc_addr(dma, 0, (uintptr_t)dev->rx);
  irqstate_t flags = spin_lock_irqsave(&dev->irq_lock);
  while (nxsem_trywait(&dev->done) == OK)
    {
    }

  dev->status = 0;
  dma2d_ll_rx_clear_interrupt_status(dma, 0, UINT32_MAX);
  dma2d_ll_rx_enable_interrupt(dma, 0,
    DMA2D_LL_EVENT_RX_SUC_EOF | DMA2D_LL_EVENT_RX_DESC_ERROR, true);
  spin_unlock_irqrestore(&dev->irq_lock, flags);
  dma2d_ll_tx_start(dma, 0);
  dma2d_ll_rx_start(dma, 0);
  ppa_ll_srm_start(ppa);

  /* IRQ completion avoids waiting for a scheduler tick on each picture.
   * Stop/reset before returning timeout so no DMA can outlive the buffers.
   */

  ret = nxsem_tickwait_uninterruptible(&dev->done, MSEC2TICK(250));
  flags = spin_lock_irqsave(&dev->irq_lock);
  dma2d_ll_rx_enable_interrupt(dma, 0, UINT32_MAX, false);
  status = dev->status;
  dma2d_ll_rx_clear_interrupt_status(dma, 0, UINT32_MAX);
  spin_unlock_irqrestore(&dev->irq_lock, flags);
  if (ret == OK && (status & DMA2D_LL_EVENT_RX_SUC_EOF) &&
      !(status & DMA2D_LL_EVENT_RX_DESC_ERROR))
    {
      dma2d_ll_tx_stop(dma, 0);
      dma2d_ll_rx_stop(dma, 0);
      ret = esp_cache_msync(j->output, j->output_size,
                            ESP_CACHE_MSYNC_FLAG_DIR_M2C);
      return ret == 0 ? 0 : -EIO;
    }

  if (ret == OK)
    {
      ret = -EIO;
    }

  syslog(LOG_ERR, "PPA abort=%d rx=%08lx tx=%08lx ppa=%08lx param=%08lx\n",
         ret, (unsigned long)dma->in_channel[0].in_int_raw.val,
         (unsigned long)dma->out_channel[0].out_int_raw.val,
         (unsigned long)ppa->int_raw.val,
         (unsigned long)ppa->sr_param_err_st.val);
  ppa_reset();
  return ret;
}

static int ppa_ioctl(FAR struct file *filep, int cmd, unsigned long arg)
{
  FAR struct p4_ppa_dev_s *dev = filep->f_inode->i_private;
  struct p4_ppa_job_s job;
  ppa_srm_color_mode_t input_format;
  int ret;
  if (cmd == P4_PPA_SRM)
    {
      input_format = PPA_SRM_COLOR_MODE_YUV422_UYVY;
    }
  else if (cmd == P4_PPA_SRM_RGB565)
    {
      input_format = PPA_SRM_COLOR_MODE_RGB565;
    }
  else
    {
      return -ENOTTY;
    }

  if (arg == 0)
    {
      return -EINVAL;
    }

  /* This board uses a FLAT build. Snapshot the bounded job once, so fields
   * validated by ppa_run cannot change while it waits for the shared engine.
   * The caller must still own both RAM buffers until ioctl returns.
   */

  job = *(FAR const struct p4_ppa_job_s *)arg;
  ret = nxmutex_lock(&dev->lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp32p4_dma2d_acquire(ppa_interrupt, dev);
  if (ret == OK)
    {
      ret = ppa_run(dev, &job, input_format);
      esp32p4_dma2d_release();
    }

  nxmutex_unlock(&dev->lock);
  return ret;
}

static const struct file_operations g_ppa_ops =
{
  .ioctl = ppa_ioctl
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int esp32p4_ppa_register(FAR const char *path)
{
  FAR struct p4_ppa_dev_s *dev = kmm_zalloc(sizeof(*dev));
  int ret;
  if (!dev)
    {
      return -ENOMEM;
    }

  nxmutex_init(&dev->lock);
  nxsem_init(&dev->done, 0, 0);
  dev->irq_lock = (spinlock_t)SP_UNLOCKED;
  dev->tx = kmm_memalign(128, 128);
  dev->rx = kmm_memalign(128, 128);
  if (!dev->tx || !dev->rx)
    {
      ret = -ENOMEM;
    }
  else
    {
      ret = register_driver(path, &g_ppa_ops, 0660, dev);
    }

  if (ret < 0)
    {
      kmm_free(dev->tx);
      kmm_free(dev->rx);
      nxsem_destroy(&dev->done);
      nxmutex_destroy(&dev->lock);
      kmm_free(dev);
    }
  return ret;
}
#endif
