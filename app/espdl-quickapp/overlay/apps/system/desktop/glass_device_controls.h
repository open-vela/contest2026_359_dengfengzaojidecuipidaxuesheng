/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
/* One session-wide 0..100 speaker gain, also used by music controls. */
unsigned glass_speaker_volume(void);
int glass_speaker_volume_set(unsigned percent);
void music_pcm_status(void);
int glass_screen_brightness_set(unsigned percent);
int glass_screen_brightness_status(void);
