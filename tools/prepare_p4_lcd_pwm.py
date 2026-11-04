#!/usr/bin/env python3
"""Prepare GPIO26 LEDC LCD high-frequency PWM; other routes retain their ABI."""
from pathlib import Path
import hashlib

SUPPORT = r"""#ifdef P4_LCD_PWM
extern int64_t esp_timer_get_time(void);

static uint32_t p4_lcd_pwm_duty(uint32_t duty)
{
  /* NuttX PWM uses unsigned 16.16, including exactly 1.0 = 65536.
   * Nine-bit LEDC supports the constant-high endpoint at 512 ticks.
   */

  return (duty * 512u + 32768u) >> 16;
}

static int p4_lcd_pwm_state(struct pwm_lowerhalf_s *dev, unsigned long arg)
{
  struct esp_ledc_s *priv = (struct esp_ledc_s *)dev;
  struct p4_lcd_pwm_state_s *state = (struct p4_lcd_pwm_state_s *)arg;
  ledc_slow_clk_sel_t source;
  int64_t started;
  bool previous;
  bool level;
  int ret;
  int i;

  if (priv->timer != 0 || priv->channels != 1 ||
      priv->chans[0].pin != 26)
    {
      return -ENOTTY;
    }

  if (state == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&g_ledc_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (p_ledc_obj == NULL || !(g_ledc_open_timers & 1u))
    {
      nxmutex_unlock(&g_ledc_lock);
      return -ENODEV;
    }

  memset(state, 0, sizeof(*state));
  ledc_hal_get_slow_clk_sel(&p_ledc_obj->ledc_hal, &source);
  ret = esp_clk_tree_src_get_freq_hz((soc_module_clk_t)source,
                                    LEDC_CLK_SRC_FREQ_PRECISION,
                                    &state->source_hz);
  if (ret != 0)
    {
      nxmutex_unlock(&g_ledc_lock);
      return -EIO;
    }

  ledc_hal_get_clock_divider(&p_ledc_obj->ledc_hal, 0,
                            &state->divider);
  ledc_hal_get_duty_resolution(&p_ledc_obj->ledc_hal, 0,
                              &state->resolution);
  if (state->divider == 0 || state->resolution != 9)
    {
      nxmutex_unlock(&g_ledc_lock);
      return -EIO;
    }

  /* Duty updates latch asynchronously. Do not sample the preceding output
   * while the first timer/clock change is still being published in hardware.
   * Wait with interrupts enabled and a bounded deadline in thread context.
   */

  for (i = 0; i < 5000; i++)
    {
      ledc_hal_get_duty(&p_ledc_obj->ledc_hal, priv->chans[0].num,
                        &state->duty_ticks);
      if (state->duty_ticks == priv->chans[0].duty)
        {
          break;
        }

      up_udelay(1);
    }

  if (i == 5000)
    {
      nxmutex_unlock(&g_ledc_lock);
      return -ETIMEDOUT;
    }

  state->frequency = ((uint64_t)state->source_hz <<
                      LEDC_LL_FRACTIONAL_BITS) /
                     (state->divider * (1u << state->resolution));
  previous = esp_gpioread(26);
  started = esp_timer_get_time();
  for (i = 0; i < 2000; i++)
    {
      level = esp_gpioread(26);
      state->high_samples += level;
      state->transitions += level != previous;
      previous = level;
      up_udelay(1);
    }

  state->elapsed_us = esp_timer_get_time() - started;
  state->samples = 2000;
  nxmutex_unlock(&g_ledc_lock);
  return OK;
}
#endif
"""

def apply(nuttx: Path):
    path=nuttx/'arch/risc-v/src/common/espressif/esp_ledc.c'
    text=path.read_text()
    if 'static int p4_lcd_pwm_state(' not in text:
        anchor='#include <nuttx/mutex.h>'
        assert text.count(anchor)==1
        text=text.replace(anchor,anchor+'\n#include <nuttx/timers/p4_lcd_pwm.h>')
        anchor='static int pwm_ioctl(struct pwm_lowerhalf_s *dev, int cmd,\n                     unsigned long arg)\n{'
        assert text.count(anchor)==1
        text=text.replace(anchor,SUPPORT+'\n'+anchor+"\n#ifdef P4_LCD_PWM\n  if (cmd == P4_LCD_PWM_GETSTATE)\n    {\n      return p4_lcd_pwm_state(dev, arg);\n    }\n#endif\n")
        anchor='static int pwm_start(struct pwm_lowerhalf_s *dev,\n                     const struct pwm_info_s *info)\n{'
        assert text.count(anchor)==1
        # This forward declaration is only needed by the GPIO26 start path.
        text=text.replace(anchor,'#ifdef P4_LCD_PWM\nstatic uint32_t p4_lcd_pwm_duty(uint32_t duty);\n#endif\n\n'+anchor)
        anchor='  /* Update timer with given PWM timer frequency */'
        assert text.count(anchor)==1
        text=text.replace(anchor,"#ifdef P4_LCD_PWM\n  if (priv->timer == 0 && info->duty > 65536u)\n    {\n      return -EINVAL;\n    }\n#endif\n\n"+anchor)
        anchor='      duty = ledc_duty_bin_conversion(info[i].duty,\n                                      priv->duty_resolution);'
        assert text.count(anchor)==1
        text=text.replace(anchor,"#ifdef P4_LCD_PWM\n      if (priv->timer == 0)\n        {\n          duty = p4_lcd_pwm_duty(info->duty);\n        }\n      else\n#endif\n        {\n"+anchor.replace("      duty", "          duty").replace("                                      priv", "                                          priv")+"\n        }")
        anchor='      esp_configgpio(priv->chans[i].pin, OUTPUT | PULLUP);'
        assert text.count(anchor)==1
        text=text.replace(anchor,"#ifdef P4_LCD_PWM\n      if (priv->timer == 0)\n        {\n          esp_configgpio(priv->chans[i].pin, OUTPUT | INPUT | PULLUP);\n        }\n      else\n#endif\n        {\n"+anchor.replace("      esp_configgpio", "          esp_configgpio")+"\n        }")
    # Refresh the managed implementation when reusing a prepared workspace.
    start=text.index('#ifdef P4_LCD_PWM\nextern int64_t esp_timer_get_time')
    end=text.index('static int pwm_ioctl(',start)
    text=text[:start].rstrip()+'\n\n'+SUPPORT+'\n'+text[end:]
    path.write_text(text)
    return {'implementation':'GPIO26 LEDC 50kHz/9bit with physical input probe',
            'enabled':True,'frequency_hz':50000,'resolution_bits':9,
            'prepared_sha256':{str(path.relative_to(nuttx)):hashlib.sha256(path.read_bytes()).hexdigest()},
            'tool_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
