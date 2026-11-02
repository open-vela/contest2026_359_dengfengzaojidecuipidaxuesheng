/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_spi_dma.inc
 * SPDX-License-Identifier: Apache-2.0
 *
 * Included after the per-port private instances in esp_spi.c.  Only ports
 * whose descriptor count is nonzero allocate DMA resources.  All transfers
 * use private, cache-line isolated internal buffers; no caller allocation or
 * rounded caller access is needed.  Polling never signals this semaphore.
 ****************************************************************************/

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#if defined(CONFIG_ESPRESSIF_SPI2_DMA) || defined(CONFIG_ESPRESSIF_SPI3_DMA)

#define SPI_DMA_BOUNCE_BYTES 512u

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool esp_spi_dma_notify(FAR struct esp_spi_priv_s *priv,
                               bool receive, bool error)
{
  irqstate_t flags;
  bool wake;

  flags = spin_lock_irqsave(&priv->dma_lock);
  priv->dma_stats.callbacks++;
  wake = priv->dma_active && (error || receive == priv->dma_wait_rx);
  if (error && priv->dma_active)
    {
      priv->dma_fault = true;
      priv->dma_stats.errors++;
    }

  spin_unlock_irqrestore(&priv->dma_lock, flags);
  if (wake)
    {
      nxsem_post(&priv->sem_isr);
    }

  return false;
}

static bool esp_spi_dma_tx_done(gdma_channel_handle_t channel,
                                FAR gdma_event_data_t *event,
                                FAR void *arg)
{
  (void)channel;
  (void)event;
  return esp_spi_dma_notify(arg, false, false);
}

static bool esp_spi_dma_rx_done(gdma_channel_handle_t channel,
                                FAR gdma_event_data_t *event,
                                FAR void *arg)
{
  (void)channel;
  return esp_spi_dma_notify(arg, true, event->flags.abnormal_eof);
}

static bool esp_spi_dma_error(gdma_channel_handle_t channel,
                              FAR gdma_event_data_t *event,
                              FAR void *arg)
{
  (void)channel;
  (void)event;
  return esp_spi_dma_notify(arg, false, true);
}

static void esp_spi_dma_close(FAR struct esp_spi_priv_s *priv)
{
  if (priv->dma_channel_rx != NULL)
    {
      gdma_stop(priv->dma_channel_rx);
      gdma_disconnect(priv->dma_channel_rx);
      gdma_del_channel(priv->dma_channel_rx);
      priv->dma_channel_rx = NULL;
    }

  if (priv->dma_channel_tx != NULL)
    {
      gdma_stop(priv->dma_channel_tx);
      gdma_disconnect(priv->dma_channel_tx);
      gdma_del_channel(priv->dma_channel_tx);
      priv->dma_channel_tx = NULL;
    }

  kmm_free(priv->dma_rxdesc);
  kmm_free(priv->dma_txdesc);
  kmm_free(priv->dma_rxbuffer);
  kmm_free(priv->dma_txbuffer);
  priv->dma_rxdesc = NULL;
  priv->dma_txdesc = NULL;
  priv->dma_rxbuffer = NULL;
  priv->dma_txbuffer = NULL;
  priv->dma_ready = false;
  priv->dma_stats.allocation_bytes = 0;
}

static int esp_spi_dma_init(FAR struct spi_dev_s *dev)
{
  FAR struct esp_spi_priv_s *priv = (FAR struct esp_spi_priv_s *)dev;
  gdma_channel_alloc_config_t config =
    {
      0
    };

  gdma_tx_event_callbacks_t tx =
    {
      .on_trans_eof = esp_spi_dma_tx_done,
      .on_descr_err = esp_spi_dma_error
    };

  gdma_rx_event_callbacks_t rx =
    {
      .on_recv_eof = esp_spi_dma_rx_done,
      .on_descr_err = esp_spi_dma_error
    };

  gdma_trigger_t trigger;
  size_t alignment = 0;
  size_t descriptors;
  int ret;

  if (priv->dma_channel == 0)
    {
      return OK;
    }

  if (esp_cache_get_alignment(MALLOC_CAP_DMA, &alignment) != ESP_OK)
    {
      return -EIO;
    }

  alignment = MAX(alignment, DMA_DESC_MEM_ALIGN_SIZE);
  if (alignment == 0 || alignment > SPI_DMA_BOUNCE_BYTES ||
      (alignment & (alignment - 1)) != 0)
    {
      return -EINVAL;
    }

  descriptors = ALIGN_UP(priv->dma_channel * sizeof(spi_dma_desc_t),
                         alignment);
  priv->dma_desc_bytes = descriptors;
  priv->dma_txdesc = kmm_memalign(alignment, descriptors);
  priv->dma_rxdesc = kmm_memalign(alignment, descriptors);
  priv->dma_txbuffer = kmm_memalign(alignment, SPI_DMA_BOUNCE_BYTES);
  priv->dma_rxbuffer = kmm_memalign(alignment, SPI_DMA_BOUNCE_BYTES);
  if (priv->dma_txdesc == NULL || priv->dma_rxdesc == NULL ||
      priv->dma_txbuffer == NULL || priv->dma_rxbuffer == NULL)
    {
      ret = -ENOMEM;
      goto fail;
    }

  if (!esp_ptr_dma_capable(priv->dma_txdesc) ||
      !esp_ptr_dma_capable(priv->dma_rxdesc) ||
      !esp_ptr_dma_capable(priv->dma_txbuffer) ||
      !esp_ptr_dma_capable(priv->dma_rxbuffer))
    {
      ret = -EFAULT;
      goto fail;
    }

  if (priv->id == SPI2_HOST)
    {
      trigger = GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_SPI, 2);
    }
#ifdef SOC_GDMA_TRIG_PERIPH_SPI3
  else if (priv->id == SPI3_HOST)
    {
      trigger = GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_SPI, 3);
    }
#endif
  else
    {
      ret = -ENODEV;
      goto fail;
    }

  ret = SPI_GDMA_NEW_CHANNEL(&config, &priv->dma_channel_tx,
                             &priv->dma_channel_rx);
  if (ret != ESP_OK)
    {
      ret = -ENOMEM;
      goto fail;
    }

  if (gdma_connect(priv->dma_channel_tx, trigger) != ESP_OK ||
      gdma_connect(priv->dma_channel_rx, trigger) != ESP_OK ||
      gdma_register_tx_event_callbacks(priv->dma_channel_tx, &tx, priv)
        != ESP_OK ||
      gdma_register_rx_event_callbacks(priv->dma_channel_rx, &rx, priv)
        != ESP_OK)
    {
      ret = -EIO;
      goto fail;
    }

  priv->dma_fault = false;
  priv->dma_active = false;
  priv->dma_ready = true;
  priv->dma_stats.allocation_bytes = 2 * descriptors +
                                     2 * SPI_DMA_BOUNCE_BYTES;
  return OK;

fail:
  esp_spi_dma_close(priv);
  return ret;
}

static int esp_spi_dma_prepare(FAR struct esp_spi_priv_s *priv,
                               bool receive, size_t bytes)
{
  FAR spi_dma_desc_t *desc;
  FAR void *buffer;

  desc = receive ? priv->dma_rxdesc : priv->dma_txdesc;
  buffer = receive ? priv->dma_rxbuffer : priv->dma_txbuffer;
  memset(desc, 0, priv->dma_desc_bytes);
  desc->dw0.size = ALIGN_UP(bytes, 4u);
  desc->dw0.length = receive ? 0 : bytes;
  desc->dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
  desc->dw0.suc_eof = receive ? 0 : 1;
  desc->buffer = buffer;
  desc->next = NULL;
  return esp_cache_msync(desc, priv->dma_desc_bytes,
                          ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

static void esp_spi_dma_exchange(FAR struct esp_spi_priv_s *priv,
                                 FAR const void *txbuffer,
                                 FAR void *rxbuffer, size_t nwords)
{
  FAR const uint8_t *tx = txbuffer;
  FAR uint8_t *rx = rxbuffer;
  size_t width = (priv->nbits + 7u) / 8u;
  size_t remaining;
  spi_hal_trans_config_t trans =
    {
      0
    };

  irqstate_t flags;
  clock_t began;
  size_t n;
  int ret = OK;

  if (!priv->dma_ready || priv->dma_fault)
    {
      return;
    }

  if (width == 0 || nwords > SIZE_MAX / width)
    {
      ret = -EOVERFLOW;
      goto fault;
    }

  remaining = nwords * width;

  trans.line_mode.data_lines = 1;
  while (remaining != 0)
    {
      n = MIN(remaining, SPI_DMA_BOUNCE_BYTES);
      memset(priv->dma_txbuffer, 0xff, SPI_DMA_BOUNCE_BYTES);
      if (tx != NULL)
        {
          memcpy(priv->dma_txbuffer, tx, n);
        }

      ret = esp_cache_msync(priv->dma_txbuffer, SPI_DMA_BOUNCE_BYTES,
                             ESP_CACHE_MSYNC_FLAG_DIR_C2M);
      if (ret != ESP_OK || esp_spi_dma_prepare(priv, false, n) != ESP_OK)
        {
          goto fault;
        }

      if (rx != NULL)
        {
          ret = esp_cache_msync(priv->dma_rxbuffer, SPI_DMA_BOUNCE_BYTES,
                                 ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                 ESP_CACHE_MSYNC_FLAG_INVALIDATE);
          if (ret != ESP_OK || esp_spi_dma_prepare(priv, true, n) != ESP_OK)
            {
              goto fault;
            }
        }

      while (nxsem_trywait(&priv->sem_isr) == OK);
      flags = spin_lock_irqsave(&priv->dma_lock);
      priv->dma_wait_rx = rx != NULL;
      priv->dma_active = true;
      spin_unlock_irqrestore(&priv->dma_lock, flags);

      trans.tx_bitlen = n * 8;
      trans.rx_bitlen = rx != NULL ? n * 8 : 0;
      trans.send_buffer = priv->dma_txbuffer;
      trans.rcv_buffer = rx != NULL ? priv->dma_rxbuffer : NULL;
      trans.cs_keep_active = remaining > n || priv->dev_cfg->cs_hold;
      spi_hal_setup_trans(priv->ctx, priv->dev_cfg, &trans);
      spi_hal_setup_device(priv->ctx, priv->dev_cfg);
      spi_ll_disable_int(priv->ctx->hw);
      spi_hal_hw_prepare_tx(priv->ctx->hw);
      if (gdma_reset(priv->dma_channel_tx) != ESP_OK ||
          gdma_start(priv->dma_channel_tx,
                       (intptr_t)priv->dma_txdesc) != ESP_OK)
        {
          goto fault;
        }

      if (rx != NULL)
        {
          spi_hal_hw_prepare_rx(priv->ctx->hw);
          if (gdma_reset(priv->dma_channel_rx) != ESP_OK ||
              gdma_start(priv->dma_channel_rx,
                           (intptr_t)priv->dma_rxdesc) != ESP_OK)
            {
              goto fault;
            }
        }

      spi_hal_enable_data_line(priv->ctx->hw, true, rx != NULL);
      spi_hal_user_start(priv->ctx);
      /* A sector completes in about 205 us at 20 MHz. Keep this ordinary
       * preemptible task running until SPI is idle, then join RX EOF before
       * touching DMA memory. This avoids a sleep/wakeup for every sector.
       * Interrupts stay enabled and both waits keep their original bounds.
       */

#ifdef CONFIG_ESPRESSIF_SPI3_DMA
      if (priv->id == SPI3_HOST)
        {
          began = clock_systime_ticks();
          while (!spi_hal_usr_is_done(priv->ctx))
            {
              if (clock_systime_ticks() - began > SEC2TICK(1))
                {
                  ret = -ETIMEDOUT;
                  goto fault;
                }
            }
        }
#endif

      ret = nxsem_tickwait_uninterruptible(&priv->sem_isr, SEC2TICK(1));
      if (ret < 0 || priv->dma_fault)
        {
          goto fault;
        }

      /* TX EOF may precede the final FIFO bits.  Do not stop the engines,
       * touch caller memory or start a polling command before SPI is idle.
       */

      began = clock_systime_ticks();
      while (!spi_hal_usr_is_done(priv->ctx))
        {
          if (clock_systime_ticks() - began > SEC2TICK(1))
            {
              ret = -ETIMEDOUT;
              goto fault;
            }
        }

      flags = spin_lock_irqsave(&priv->dma_lock);
      priv->dma_active = false;
      spin_unlock_irqrestore(&priv->dma_lock, flags);
      gdma_stop(priv->dma_channel_tx);
      gdma_stop(priv->dma_channel_rx);
      spi_ll_dma_tx_enable(priv->ctx->hw, false);
      spi_ll_dma_rx_enable(priv->ctx->hw, false);
      if (rx != NULL)
        {
          ret = esp_cache_msync(priv->dma_rxbuffer, SPI_DMA_BOUNCE_BYTES,
                                 ESP_CACHE_MSYNC_FLAG_DIR_M2C |
                                 ESP_CACHE_MSYNC_FLAG_INVALIDATE);
          if (ret != ESP_OK)
            {
              goto fault;
            }

          memcpy(rx, priv->dma_rxbuffer, n);
          rx += n;
        }

      if (tx != NULL)
        {
          tx += n;
        }

      flags = spin_lock_irqsave(&priv->dma_lock);
      priv->dma_stats.transfers++;
      priv->dma_stats.tx_bytes += n;
      priv->dma_stats.rx_bytes += rxbuffer != NULL ? n : 0;
      spin_unlock_irqrestore(&priv->dma_lock, flags);
      remaining -= n;
    }

  return;

fault:
  if (ret >= 0)
    {
      ret = -EIO;
    }

  flags = spin_lock_irqsave(&priv->dma_lock);
  priv->dma_active = false;
  priv->dma_fault = true;
  priv->dma_stats.errors++;
  priv->dma_stats.timeouts += ret == -ETIMEDOUT;
  spin_unlock_irqrestore(&priv->dma_lock, flags);
  gdma_stop(priv->dma_channel_tx);
  gdma_stop(priv->dma_channel_rx);
  PERIPH_RCC_ATOMIC()
    {
      spi_ll_reset_register(priv->id);
    }

  spierr("SPI DMA stopped, port=%u error=%d; no transfer replay\n",
         priv->id, ret);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int esp_spidma_get_stats(int port, FAR struct esp_spidma_stats_s *stats)
{
  FAR struct esp_spi_priv_s *priv;
  irqstate_t flags;

  if (stats == NULL)
    {
      return -EINVAL;
    }

  switch (port)
    {
#ifdef CONFIG_ESPRESSIF_SPI2
      case 2:
        priv = &esp_spi2_priv;
        break;
#endif
#ifdef CONFIG_ESPRESSIF_SPI3
      case 3:
        priv = &esp_spi3_priv;
        break;
#endif
      default:
        return -ENODEV;
    }

  flags = spin_lock_irqsave(&priv->dma_lock);
  *stats = priv->dma_stats;
  stats->configured = priv->dma_channel != 0;
  stats->ready = priv->dma_ready;
  stats->fault = priv->dma_fault;
  stats->frequency = priv->dev_cfg->timing_conf.real_freq;
  spin_unlock_irqrestore(&priv->dma_lock, flags);
  return OK;
}

#endif
