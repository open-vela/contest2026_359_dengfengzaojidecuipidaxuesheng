/****************************************************************************
 * arch/risc-v/src/esp32p4/esp32p4_dma2d.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#if defined(CONFIG_ESP32P4_PPA) || defined(CONFIG_ESP32P4_JPEG)

#include <errno.h>
#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>
#include <nuttx/spinlock.h>

#include "hal/dma2d_ll.h"
#include "soc/interrupts.h"
#include "esp_irq.h"
#include "esp32p4_dma2d.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct esp32p4_dma2d_s
{
  mutex_t lock;
  spinlock_t irq_lock;
  esp32p4_dma2d_callback_t callback;
  FAR void *arg;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* One physical reset domain and RX0 interrupt. The shared manager lives for
 * the board lifetime; clients retain their own device and request state.
 */

static mutex_t g_dma2d_initlock = NXMUTEX_INITIALIZER;
static FAR struct esp32p4_dma2d_s *g_dma2d;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void dma2d_reset(void)
{
  irqstate_t flags;
  int __DECLARE_RCC_ATOMIC_ENV = 0;

  flags = enter_critical_section();
  dma2d_ll_enable_bus_clock(0, true);
  dma2d_ll_reset_register(0);
  leave_critical_section(flags);
  dma2d_ll_hw_enable(DMA2D_LL_GET_HW(0), true);
}

static int dma2d_interrupt(int irq, FAR void *context, FAR void *arg)
{
  FAR struct esp32p4_dma2d_s *priv = arg;
  FAR dma2d_dev_t *dma = DMA2D_LL_GET_HW(0);
  irqstate_t flags;
  uint32_t status;

  flags = spin_lock_irqsave(&priv->irq_lock);
  status = dma->in_channel[0].in_int_raw.val;
  dma2d_ll_rx_clear_interrupt_status(dma, 0, status);
  if (status != 0 && priv->callback != NULL)
    {
      /* Keep the lock through dispatch: releasing a lease cannot race an
       * already selected callback on the other CPU.
       */

      priv->callback(priv->arg, status);
    }

  spin_unlock_irqrestore(&priv->irq_lock, flags);
  return OK;
}

static int dma2d_initialize(void)
{
  FAR struct esp32p4_dma2d_s *priv;
  FAR dma2d_dev_t *dma = DMA2D_LL_GET_HW(0);
  int ret;

  ret = nxmutex_lock(&g_dma2d_initlock);
  if (ret < 0)
    {
      return ret;
    }

  if (g_dma2d != NULL)
    {
      nxmutex_unlock(&g_dma2d_initlock);
      return OK;
    }

  priv = kmm_zalloc(sizeof(*priv));
  if (priv == NULL)
    {
      nxmutex_unlock(&g_dma2d_initlock);
      return -ENOMEM;
    }

  nxmutex_init(&priv->lock);
  priv->irq_lock = (spinlock_t)SP_UNLOCKED;

  dma2d_reset();
  dma2d_ll_rx_enable_interrupt(dma, 0, UINT32_MAX, false);
  dma2d_ll_tx_enable_interrupt(dma, 0, UINT32_MAX, false);
  dma2d_ll_rx_clear_interrupt_status(dma, 0, UINT32_MAX);
  dma2d_ll_tx_clear_interrupt_status(dma, 0, UINT32_MAX);

  ret = esp_setup_irq(ETS_DMA2D_IN_CH0_INTR_SOURCE,
                      ESP_IRQ_PRIORITY_DEFAULT, ESP_IRQ_TRIGGER_LEVEL,
                      dma2d_interrupt, priv);
  if (ret < 0)
    {
      nxmutex_destroy(&priv->lock);
      kmm_free(priv);
      nxmutex_unlock(&g_dma2d_initlock);
      return ret;
    }

  g_dma2d = priv;
  up_enable_irq(ESP_SOURCE2IRQ(ETS_DMA2D_IN_CH0_INTR_SOURCE));
  nxmutex_unlock(&g_dma2d_initlock);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int esp32p4_dma2d_acquire(esp32p4_dma2d_callback_t callback, FAR void *arg)
{
  FAR struct esp32p4_dma2d_s *priv;
  irqstate_t flags;
  int ret;

  if (callback == NULL || arg == NULL)
    {
      return -EINVAL;
    }

  ret = dma2d_initialize();
  if (ret < 0)
    {
      return ret;
    }

  priv = g_dma2d;
  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  flags = spin_lock_irqsave(&priv->irq_lock);
  /* Channel reset alone need not clear reorder/CSC configuration. Every
   * owner starts from a full controller reset while no other DMA2D user
   * can be active. Peripheral setup must follow this lease acquisition.
   */

  dma2d_reset();
  priv->arg = arg;
  priv->callback = callback;
  spin_unlock_irqrestore(&priv->irq_lock, flags);
  return OK;
}

void esp32p4_dma2d_stop(void)
{
  FAR struct esp32p4_dma2d_s *priv = g_dma2d;
  FAR dma2d_dev_t *dma = DMA2D_LL_GET_HW(0);
  irqstate_t flags;

  flags = spin_lock_irqsave(&priv->irq_lock);
  dma2d_ll_rx_enable_interrupt(dma, 0, UINT32_MAX, false);
  dma2d_ll_tx_enable_interrupt(dma, 0, UINT32_MAX, false);
  dma2d_ll_tx_stop(dma, 0);
  dma2d_ll_rx_stop(dma, 0);
  dma2d_ll_rx_clear_interrupt_status(dma, 0, UINT32_MAX);
  dma2d_ll_tx_clear_interrupt_status(dma, 0, UINT32_MAX);
  dma2d_reset();
  priv->callback = NULL;
  priv->arg = NULL;
  spin_unlock_irqrestore(&priv->irq_lock, flags);
}

void esp32p4_dma2d_release(void)
{
  FAR struct esp32p4_dma2d_s *priv = g_dma2d;

  esp32p4_dma2d_stop();
  nxmutex_unlock(&priv->lock);
}

#endif /* CONFIG_ESP32P4_PPA || CONFIG_ESP32P4_JPEG */
