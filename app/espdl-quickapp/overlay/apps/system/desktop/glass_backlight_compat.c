/* SPDX-License-Identifier: Apache-2.0 */
/* Existing UI probes enter through the permanent desktop task group. */
#include <stdio.h>
#include "glass_device_controls.h"
#include "glass_backlight.h"

int glass_screen_brightness_set(unsigned percent)
{
  return glass_backlight_set(percent);
}

int glass_screen_brightness_status(void)
{
  struct glass_backlight_status state;
  glass_backlight_read(&state);
  printf("BACKLIGHT available=%u applied=%u pin=26 frequency=5000 percent=%u duty=%u error=%d\n",
         state.available,state.applied,state.percent,state.duty,state.error);
  return state.applied && !state.error ? 0 : -1;
}
