#!/usr/bin/env python3
"""Test production ES8311 mono extraction with long, slot-changing PCM."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
header = root / 'app/espdl-quickapp/overlay/apps/system/desktop/qpk_mono_input.h'
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
fixture = out / 'recorder_input.c'
fixture.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "qpk_mono_input.h"

static void put(uint8_t *p, int value)
{ p[0] = value; p[1] = (unsigned)value >> 8; }
static int sample(unsigned frame)
{
  if (frame % 37 == 0) return 0;
  if (frame % 1009 == 0) return -32768;
  if (frame % 1013 == 0) return 32767;
  return (int)((frame * 7919u) % 60001) - 30000;
}
static void long_capture(unsigned mode)
{
  uint8_t raw[4032], saved[4032], guarded[2018];
  struct qpk_mono_input state = {0};
  unsigned settle = 1024, consumed = 0, produced = 0;
  /* Both transitions are inside a DMA buffer; first is two samples in. */
  const unsigned first = 1008 * 905 + 2, second = 1008 * 1430 + 17;
  const unsigned frames = 120 * 16000 + 1024;
  while (consumed < frames)
    {
      unsigned count = frames - consumed;
      if (count > 1008) count = 1008;
      for (unsigned j = 0; j < count; ++j)
        {
          unsigned frame = consumed + j;
          unsigned right = mode == 1 && frame >= first && frame < second;
          int value = sample(frame);
          put(raw + j * 4, mode == 2 || !right ? value : 0);
          put(raw + j * 4 + 2, mode == 2 || right ? value : 0);
        }
      memcpy(saved, raw, sizeof(raw));
      memset(guarded, 0xa5, sizeof(guarded));
      size_t bytes = qpk_mono_input_decode(&state, raw, count * 4,
                                         guarded + 1, 2016, &settle);
      assert(bytes <= 2016 && bytes % 2 == 0);
      assert(guarded[0] == 0xa5 && guarded[2017] == 0xa5);
      assert(!memcmp(saved, raw, sizeof(raw)));
      unsigned expected_peak = 0;
      for (size_t j = 0; j < bytes; j += 2)
        {
          int expected = sample(1024 + produced++);
          int actual = (int16_t)(guarded[j + 1] | (unsigned)guarded[j + 2] << 8);
          assert(actual == expected);
          unsigned magnitude = expected < 0 ? -expected : expected;
          if (magnitude > expected_peak) expected_peak = magnitude;
        }
      assert(state.peak == expected_peak);
      consumed += count;
    }
  assert(produced == 120 * 16000 && !settle && state.slot == 0);
  assert(state.switches == (mode == 1 ? 2u : 0u));
}
static void boundaries(void)
{
  uint8_t input[21] = {0}, output[19];
  struct qpk_mono_input state = {0};
  unsigned settle = 0;
  memset(output, 0xa5, sizeof(output));
  assert(qpk_mono_input_decode(&state, input + 1, 19, output + 1, 7, &settle) == 6);
  assert(!state.switches && !state.peak && output[0] == 0xa5 && output[7] == 0xa5);
  put(input + 1, -32768); put(input + 5 + 2, 32767);
  /* An unaligned input and an odd output capacity, including a mid-buffer
   * occupied-slot change and a trailing incomplete frame. */
  assert(qpk_mono_input_decode(&state, input + 1, 11, output + 1, 5, &settle) == 4);
  assert(state.peak == 32768 && state.left_peak == 32768 && state.right_peak == 32767);
  assert(state.slot == 1 && state.switches == 1);
  assert(output[1] == 0 && output[2] == 128 && output[3] == 255 && output[4] == 127);
  memset(input, 0, sizeof(input));
  assert(qpk_mono_input_decode(&state, input, 20, output, 10, &settle) == 10);
  assert(state.slot == 1 && state.switches == 1 && !state.peak);
  settle = 3;
  assert(qpk_mono_input_decode(&state, input, 8, output, 10, &settle) == 0 && settle == 1);
  assert(qpk_mono_input_decode(&state, input, 20, output, 3, &settle) == 2 && !settle);
  output[0] = 0xa5; settle = 1;
  assert(qpk_mono_input_decode(&state, input, 20, output, 1, &settle) == 0);
  assert(settle == 1 && output[0] == 0xa5);
}
int main(void)
{
  long_capture(0); long_capture(1); long_capture(2); boundaries();
  puts("RECORDER_INPUT_OK: 3 x 120s bit-exact PCM, mid-buffer slot changes, zero crossings, silence, duplicate slots, signed extrema, settling and bounds");
}
''')
binary = out / 'recorder_input'
subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I', str(header.parent), str(fixture), '-o', str(binary)], check=True)
result = subprocess.run([str(binary)], check=True, text=True, capture_output=True)
print(result.stdout, end='')
(out / 'RESULT.json').write_text(json.dumps({
    'passed': True, 'source_sha256': hashlib.sha256(header.read_bytes()).hexdigest(),
    'sanitizers': ['address', 'undefined'], 'result': result.stdout.strip()
}, indent=2) + '\n')
