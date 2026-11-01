#!/usr/bin/env python3
"""Prepare bounded, lock-free P4 display shutdown for the reset-only path."""
import hashlib
from pathlib import Path


INCLUDES = '''/* ESPCLAW RESET DISPLAY INCLUDES */
#include "hal/gpio_ll.h"
#include "hal/ledc_ll.h"
#include "soc/gpio_sig_map.h"
'''

RESET_HELPER = r'''/* ESPCLAW SAFE RESET DISPLAY
 * Reset-only entry. The caller has disabled local interrupts and stopped the
 * other hart, uses an internal stack and has not disabled the cache yet.
 * Never take a driver mutex/spinlock: the stopped hart might own it.
 */

#define ESP_MIPI_DSI_RESET_WAIT 1000000u

int IRAM_ATTR esp_mipi_dsi_reset_prepare(void)
{
  FAR struct esp_mipi_dsi_priv_s *priv = &g_esp_mipi_dsi;
  FAR dw_gdma_dev_t *dev = priv->dw_hal.dev;
  const uint32_t channel_bit = 1u << ESP_MIPI_DSI_DMA_CHAN;
  const uint32_t abort_mask = 0x101u << ESP_MIPI_DSI_DMA_CHAN;
  bool drained = true;
  unsigned i;

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_LCD
  /* PWM0 is LEDC channel 0 on GPIO26. Do not call the PWM driver here:
   * its lock may belong to the stopped hart. No setting is persisted.
   */

#  if defined(CONFIG_ESPRESSIF_LEDC_TIMER0) && \
      defined(CONFIG_ESPRESSIF_LEDC_CHANNEL0_PIN) && \
      CONFIG_ESPRESSIF_LEDC_CHANNEL0_PIN == 26
  if (HP_SYS_CLKRST.soc_clk_ctrl3.reg_ledc_apb_clk_en)
    {
      ledc_ll_set_idle_level(LEDC_LL_GET_HW(), LEDC_LOW_SPEED_MODE, 0, 0);
      ledc_ll_set_sig_out_en(LEDC_LL_GET_HW(), LEDC_LOW_SPEED_MODE, 0, false);
      ledc_ll_set_duty_start(LEDC_LL_GET_HW(), LEDC_LOW_SPEED_MODE, 0);
      ledc_ll_ls_channel_update(LEDC_LL_GET_HW(), LEDC_LOW_SPEED_MODE, 0);
    }
#  endif

  /* Also cover reset before PWM setup, when the LCD uses plain GPIO output.
   * Latch low before disconnecting LEDC, then select the GPIO latch as the
   * matrix source. GPIO26 is already configured for the GPIO matrix by LCD
   * initialization. GPIO27 (panel reset) and every other pin stay untouched.
   */

  gpio_ll_set_level(&GPIO, 26, 0);
  gpio_ll_set_output_signal_matrix_source(&GPIO, 26, SIG_GPIO_OUT_IDX, false);
  gpio_ll_set_output_enable_ctrl(&GPIO, 26, false, false);
  gpio_ll_output_enable(&GPIO, 26);
#endif

  priv->dma_enabled = false;
  priv->pending_fb = NULL;

  if (priv->initialized && priv->fb_bound && dev != NULL)
    {
      /* Affect only the display channel. A CSI channel may still be allocated
       * and is handled later by the generic system reset, without freeing its
       * descriptors or framebuffers while a DMA engine can still own them.
       */

      dw_gdma_ll_channel_enable_intr_generation(dev, ESP_MIPI_DSI_DMA_CHAN,
                                                UINT32_MAX, false);
      dw_gdma_ll_channel_enable_intr_propagation(dev, ESP_MIPI_DSI_DMA_CHAN,
                                                 UINT32_MAX, false);
      dw_gdma_ll_channel_enable(dev, ESP_MIPI_DSI_DMA_CHAN, false);
      drained = false;
      for (i = 0; i < ESP_MIPI_DSI_RESET_WAIT; i++)
        {
          if ((dev->chen0.val & channel_bit) == 0)
            {
              drained = true;
              break;
            }
        }

      if (!drained)
        {
          /* The SDK abort helper waits forever. Bound our fallback so the
           * already-armed RTC watchdog can recover even a hung AXI master.
           * CHEN1 is a masked write: do not read/modify/write other channels.
           */

          dev->chen1.val = abort_mask;
          for (i = 0; i < ESP_MIPI_DSI_RESET_WAIT; i++)
            {
              if ((dev->chen1.val & abort_mask) == 0)
                {
                  drained = true;
                  break;
                }
            }
        }

      dw_gdma_ll_channel_clear_intr(dev, ESP_MIPI_DSI_DMA_CHAN, UINT32_MAX);
    }

  if (priv->initialized)
    {
      if (priv->hal.bridge != NULL)
        {
          mipi_dsi_brg_ll_enable_dpi_output(priv->hal.bridge, false);
        }

      if (priv->hal.host != NULL)
        {
          mipi_dsi_host_ll_enable_video_mode(priv->hal.host, false);
        }
    }

  priv->video_running = false;
  return drained ? OK : -ETIMEDOUT;
}

'''

DECLARATION = '''/* Reset-only shutdown: other hart stopped, IRQs off, internal stack,
 * cache still enabled. Blanks GPIO26, bounds display DMA drain/abort and
 * never frees a framebuffer or persists brightness. Returns -ETIMEDOUT if
 * the display channel did not quiesce; the caller must still reset.
 */
int esp_mipi_dsi_reset_prepare(void);
'''


def apply(nuttx: Path):
    root = Path(nuttx)
    common = root / 'arch/risc-v/src/common/espressif'
    source = common / 'esp_mipi_dsi.c'
    header = common / 'esp_mipi_dsi.h'
    text = source.read_text()
    if '/* ESPCLAW RESET DISPLAY INCLUDES */' not in text:
        anchor = '#include "esp_mipi_dsi.h"\n'
        if text.count(anchor) != 1:
            raise RuntimeError('Unknown P4 DSI include anchor')
        text = text.replace(anchor, anchor + INCLUDES, 1)
    elif text.count(INCLUDES) != 1:
        raise RuntimeError('Unexpected reset display include change')
    if '/* ESPCLAW SAFE RESET DISPLAY' not in text:
        anchor = 'int esp_mipi_dsi_swap_framebuffer(FAR void *fb, uint32_t timeout_ms)\n'
        if text.count(anchor) != 1:
            raise RuntimeError('Unknown P4 DSI reset helper anchor')
        text = text.replace(anchor, RESET_HELPER + anchor, 1)
    elif text.count(RESET_HELPER) != 1:
        raise RuntimeError('Unexpected reset display helper change')
    source_text = text
    text = header.read_text()
    if 'int esp_mipi_dsi_reset_prepare(void);' not in text:
        anchor = 'int esp_mipi_dsi_video_stop(void);\n'
        if text.count(anchor) != 1:
            raise RuntimeError('Unknown P4 DSI reset declaration anchor')
        text = text.replace(anchor, anchor + '\n' + DECLARATION, 1)
    elif text.count(DECLARATION) != 1:
        raise RuntimeError('Unexpected reset display declaration change')
    header_text = text
    # Validate both files before changing either one in the prepared tree.
    if source.read_text() != source_text:
        source.write_text(source_text)
    if header.read_text() != header_text:
        header.write_text(header_text)
    return {
        'enabled': True,
        'entry': 'esp_mipi_dsi_reset_prepare',
        'requires': 'other hart stopped, local IRQs off, internal stack, cache on',
        'backlight_gpio': 26,
        'dma_channel': 0,
        'max_poll_iterations_per_phase': 1000000,
        'runtime_tested': False,
        'prepared_sha256': {
            str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in [source, header]
        },
        'tool_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    }
