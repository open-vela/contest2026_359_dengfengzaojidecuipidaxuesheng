/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* This board's single ES8311 ADC occupies one of two 16-bit I2S slots;
 * the unused slot is zero. RX handoff can change the occupied slot, including
 * partway through a returned buffer. This is not a stereo downmixer.
 */
struct qpk_mono_input
{
  unsigned slot, switches;
  unsigned left_peak, right_peak, peak;
};

static inline size_t qpk_mono_input_decode(struct qpk_mono_input *state,
                                         const uint8_t *input, size_t bytes,
                                         uint8_t *output, size_t capacity,
                                         unsigned *settle)
{
  size_t written = 0;
  state->left_peak = state->right_peak = state->peak = 0;
  for (size_t i = 0; i + 4 <= bytes && written + 2 <= capacity; i += 4)
    {
      if (*settle) { --*settle; continue; }
      int left = (int16_t)(input[i] | (unsigned)input[i + 1] << 8);
      int right = (int16_t)(input[i + 2] | (unsigned)input[i + 3] << 8);
      unsigned lm = left < 0 ? -left : left;
      unsigned rm = right < 0 ? -right : right;
      if (lm > state->left_peak) state->left_peak = lm;
      if (rm > state->right_peak) state->right_peak = rm;
      if ((state->slot ? !right && left : !left && right))
        {
          state->slot ^= 1;
          ++state->switches;
        }
      unsigned magnitude = state->slot ? rm : lm;
      if (magnitude > state->peak) state->peak = magnitude;
      size_t offset = i + state->slot * 2;
      output[written++] = input[offset];
      output[written++] = input[offset + 1];
    }
  return written;
}
