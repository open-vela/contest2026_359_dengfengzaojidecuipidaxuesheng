/* SPDX-License-Identifier: Apache-2.0 */
#ifndef __INCLUDE_NUTTX_TIMERS_P4_LCD_PWM_H
#define __INCLUDE_NUTTX_TIMERS_P4_LCD_PWM_H
#include <nuttx/config.h>
#include <nuttx/fs/ioctl.h>
#include <stdint.h>
#define P4_LCD_PWM_GETSTATE _PWMIOC(0x40)
#if defined(CONFIG_ARCH_CHIP_ESP32P4) && defined(CONFIG_ESPRESSIF_LEDC_TIMER0) && \
    CONFIG_ESPRESSIF_LEDC_TIMER0_CHANNELS == 1 && \
    CONFIG_ESPRESSIF_LEDC_CHANNEL0_PIN == 26 && \
    CONFIG_ESPRESSIF_LEDC_TIMER0_RESOLUTION == 9 && !defined(CONFIG_PWM_MULTICHAN)
#  define P4_LCD_PWM 1
#endif
struct p4_lcd_pwm_state_s
{
  uint32_t frequency;
  uint32_t source_hz;
  uint32_t divider;
  uint32_t resolution;
  uint32_t duty_ticks;
  uint32_t samples;
  uint32_t high_samples;
  uint32_t transitions;
  uint32_t elapsed_us;
};
#endif
