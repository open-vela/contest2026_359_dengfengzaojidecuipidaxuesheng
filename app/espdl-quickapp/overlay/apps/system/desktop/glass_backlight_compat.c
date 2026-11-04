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
  printf("BACKLIGHT available=%u applied=%u pin=26 frequency=50000 percent=%u duty=%u error=%d\n",
         state.available,state.applied,state.percent,state.duty,state.error);
  printf("BACKLIGHT_SIGNAL result=%d hz=%u source_hz=%u div=%u bits=%u duty_ticks=%u samples=%u high=%u edges=%u us=%u\n",
         state.signal_error,state.signal.frequency,state.signal.source_hz,state.signal.divider,state.signal.resolution,
         state.signal.duty_ticks,state.signal.samples,state.signal.high_samples,state.signal.transitions,state.signal.elapsed_us);
  return state.applied && !state.error ? 0 : -1;
}
