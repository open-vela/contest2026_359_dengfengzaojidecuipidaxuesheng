/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <nuttx/config.h>
#include <stdbool.h>
#include <errno.h>

/* PWM0 is exclusively the factory LCD's J6 PWM -> J1 GPIO26 route.
 * FS USB must keep its existing GPIO26/27 display ownership guard. */
#if defined(CONFIG_PWM) && defined(CONFIG_ESPRESSIF_LEDC_TIMER0) && \
    CONFIG_ESPRESSIF_LEDC_TIMER0_CHANNELS == 1 && \
    CONFIG_ESPRESSIF_LEDC_CHANNEL0_PIN == 26 && !defined(CONFIG_PWM_MULTICHAN)
#define GLASS_PWM_BACKLIGHT 1
#endif

struct glass_backlight_status
{
  bool available;
  bool applied;
  unsigned percent;
  unsigned duty;
  int error;
};

#ifdef GLASS_PWM_BACKLIGHT
/* Apply only from the resident desktop task group (startup/LVGL callbacks).
 * Its PWM fd must never be used from a short-lived NSH task group.
 * Session brightness starts at 100%; slider movement never writes Flash. */
int glass_backlight_set(unsigned percent);
void glass_backlight_read(struct glass_backlight_status *status);
#else
static inline int glass_backlight_set(unsigned percent)
{ (void)percent; return -ENOTSUP; }
static inline void glass_backlight_read(struct glass_backlight_status *status)
{ *status=(struct glass_backlight_status){.percent=100,.error=-ENOTSUP}; }
#endif
