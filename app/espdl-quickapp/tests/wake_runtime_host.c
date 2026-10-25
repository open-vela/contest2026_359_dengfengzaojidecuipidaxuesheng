/* SPDX-License-Identifier: Apache-2.0 */
/* Host-only numerical equivalence checks against the original arithmetic. */
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Keep the private bounded matcher visible without a production test API. */
#include "glass_wake_features.c"

static uint32_t random_state = 0x68032f41u;
static unsigned rms_checks, dtw_checks, bounded_checks, bank_checks, pruned;

static uint32_t next_random(void)
{
  random_state ^= random_state << 13;
  random_state ^= random_state >> 17;
  random_state ^= random_state << 5;
  return random_state;
}

static float random_feature(void)
{
  return ((int32_t)(next_random() % 65537u) - 32768) / 8192.0f;
}

static uint32_t float_bits(float value)
{
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

static void same_float(float actual, float expected)
{
  if (float_bits(actual) != float_bits(expected))
    {
      fprintf(stderr, "float mismatch: actual=%a (0x%08x), expected=%a (0x%08x)\n",
              (double)actual, float_bits(actual),
              (double)expected, float_bits(expected));
      abort();
    }
}

static float old_pcm_rms(const int16_t *pcm, size_t count)
{
  double sum = 0;
  for (size_t i = 0; i < count; i++)
    sum += (double)pcm[i] * pcm[i];
  return count ? sqrtf(sum / count) : 0;
}

static void check_rms(const int16_t *pcm, size_t count)
{
  same_float(glass_wake_pcm_rms(pcm, count), old_pcm_rms(pcm, count));
  rms_checks++;
}

static void test_rms(void)
{
  int16_t *pcm = malloc(GLASS_WAKE_MAX_SAMPLES * sizeof(*pcm));
  assert(pcm);
  check_rms(NULL, 0);

  /* Every representable int16 value through an entire production block. */
  for (int value = INT16_MIN; value <= INT16_MAX; value++)
    {
      for (unsigned i = 0; i < 320; i++)
        pcm[i] = (int16_t)value;
      check_rms(pcm, 320);
    }

  const size_t lengths[] = {1, 2, 3, 319, 320, 321, 639, 640, 2559,
                            2560, 2561, 4096, 63999, 64000};
  for (unsigned pattern = 0; pattern < 6; pattern++)
    {
      for (size_t i = 0; i < GLASS_WAKE_MAX_SAMPLES; i++)
        {
          switch (pattern)
            {
              case 0: pcm[i] = INT16_MIN; break;
              case 1: pcm[i] = INT16_MAX; break;
              case 2: pcm[i] = i & 1 ? INT16_MIN : INT16_MAX; break;
              case 3: pcm[i] = (int16_t)((int)(i % 65536) + INT16_MIN); break;
              case 4: pcm[i] = i % 319 ? 0 : INT16_MIN; break;
              default: pcm[i] = (int16_t)((int)(next_random() & 65535u) + INT16_MIN);
            }
        }
      for (unsigned i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++)
        check_rms(pcm, lengths[i]);
    }

  for (unsigned round = 0; round < 2048; round++)
    {
      size_t count = round < 2000 ? 320 : next_random() % 64001u;
      for (size_t i = 0; i < count; i++)
        pcm[i] = (int16_t)((int)(next_random() & 65535u) + INT16_MIN);
      check_rms(pcm, count);
    }
  free(pcm);
}

/* Frozen full-search oracle: no cutoff or calls into the optimized path. */
static float old_dtw(const float *a, const float *b)
{
  float previous[33], current[33];
  for (unsigned i = 0; i < 33; i++) previous[i] = 1e20f;
  previous[0] = 0;
  for (int i = 1; i <= 32; i++)
    {
      for (unsigned j = 0; j < 33; j++) current[j] = 1e20f;
      int first = i > 8 ? i - 8 : 1;
      int last = i + 8 < 32 ? i + 8 : 32;
      for (int j = first; j <= last; j++)
        {
          float cost = 0;
          for (unsigned k = 0; k < 8; k++)
            {
              float d = a[(i - 1) * 13 + k] - b[(j - 1) * 13 + k];
              cost += d * d * (k < 4 ? 2 : 1);
            }
          current[j] = cost / 12 +
              fminf(previous[j - 1], fminf(previous[j], current[j - 1]));
        }
      memcpy(previous, current, sizeof(previous));
    }
  return previous[32] / 32;
}

static float old_ratio_distance(const float *templates, const float *feature,
                                float *positive_distance)
{
  float best[2][3] = {{1e20f, 1e20f, 1e20f}, {1e20f, 1e20f, 1e20f}};
  for (unsigned i = 0; i < GLASS_WAKE_TEMPLATES; i++)
    {
      unsigned group = i < GLASS_WAKE_POSITIVE ? 0 : 1;
      float distance = old_dtw(templates + i * GLASS_WAKE_INPUTS, feature);
      for (unsigned j = 0; j < 3; j++)
        if (distance < best[group][j])
          {
            for (unsigned k = 2; k > j; k--) best[group][k] = best[group][k - 1];
            best[group][j] = distance;
            break;
          }
    }
  float p = best[0][0] + best[0][1] + best[0][2];
  float n = best[1][0] + best[1][1] + best[1][2];
  *positive_distance = p / 3;
  return p / fmaxf(n, 1e-9f);
}

static void check_dtw(const float *a, const float *b)
{
  float expected = old_dtw(a, b);
  same_float(glass_wake_dtw(a, b), expected);
  dtw_checks++;

  const float cutoffs[] = {0, 0x1p-149f, .01f, .25f, 1,
                          nextafterf(expected, 0), expected,
                          nextafterf(expected, INFINITY),
                          1e20f / 32, 1e20f};
  for (unsigned i = 0; i < sizeof(cutoffs) / sizeof(cutoffs[0]); i++)
    {
      float cutoff = cutoffs[i];
      float actual = glass_wake_dtw_bounded(a, b, cutoff);
      if (expected < cutoff || cutoff >= 1e20f / 32)
        same_float(actual, expected);
      else
        {
          /* A loser may return a lower bound, but must never enter top 3. */
          assert(actual >= cutoff);
          if (actual < expected) pruned++;
        }
      bounded_checks++;
    }
}

static void test_dtw(void)
{
  float a[GLASS_WAKE_INPUTS], b[GLASS_WAKE_INPUTS];
  for (unsigned round = 0; round < 80; round++)
    {
      for (unsigned i = 0; i < GLASS_WAKE_INPUTS; i++)
        {
          a[i] = round < 64 ? random_feature() : 0;
          b[i] = round < 64 ? random_feature() : (float)(round - 64) / 4;
        }
      check_dtw(a, b);
    }

  const float extremes[] = {0, 0x1p-74f, -0x1p-74f, 1e10f, -1e10f,
                            FLT_MAX, -FLT_MAX};
  for (unsigned t = 0; t < sizeof(extremes) / sizeof(extremes[0]); t++)
    {
      for (unsigned i = 0; i < GLASS_WAKE_INPUTS; i++)
        {
          a[i] = extremes[t];
          b[i] = 0;
        }
      check_dtw(a, b);
    }
  assert(pruned > 0);
}

static void check_bank(const float *bank, const float *feature)
{
  float expected_distance, actual_distance;
  float expected = old_ratio_distance(bank, feature, &expected_distance);
  float actual = glass_wake_ratio_distance(bank, feature, &actual_distance);
  same_float(actual, expected);
  same_float(actual_distance, expected_distance);
  same_float(glass_wake_ratio_distance(bank, feature, NULL), expected);
  same_float(glass_wake_ratio(bank, feature), expected);
  bank_checks++;
}

static void set_template(float *bank, unsigned index, float value)
{
  for (unsigned k = 0; k < GLASS_WAKE_INPUTS; k++)
    bank[index * GLASS_WAKE_INPUTS + k] = value;
}

static void test_banks(void)
{
  float *bank = malloc(GLASS_WAKE_WEIGHTS * sizeof(*bank));
  float feature[GLASS_WAKE_INPUTS] = {0};
  assert(bank);

  /* All ties, duplicate nearest neighbours, and the original .25 ratio. */
  const float constants[][2] = {{0, 0}, {1, 1}, {1, 2}, {.1f, .1f},
                                {0x1p-74f, 0x1p-73f}, {1e20f, -1e20f}};
  for (unsigned c = 0; c < sizeof(constants) / sizeof(constants[0]); c++)
    {
      for (unsigned t = 0; t < GLASS_WAKE_TEMPLATES; t++)
        set_template(bank, t, constants[c][t < GLASS_WAKE_POSITIVE ? 0 : 1]);
      check_bank(bank, feature);
    }

  /* The nearest 3 arrive first, last, or amid many rejected templates. */
  const unsigned starts[][2] = {{0, GLASS_WAKE_POSITIVE},
      {GLASS_WAKE_POSITIVE - 3, GLASS_WAKE_TEMPLATES - 3}, {23, 117}};
  for (unsigned order = 0; order < sizeof(starts) / sizeof(starts[0]); order++)
    {
      for (unsigned t = 0; t < GLASS_WAKE_TEMPLATES; t++)
        set_template(bank, t, t < GLASS_WAKE_POSITIVE ? 3 : 4);
      for (unsigned i = 0; i < 3; i++)
        {
          set_template(bank, starts[order][0] + i, .1f * (i + 1));
          set_template(bank, starts[order][1] + i, .6f + .1f * i);
        }
      check_bank(bank, feature);
    }

  /* Structured shuffled populations and independent bounded random features. */
  for (unsigned round = 0; round < 40; round++)
    {
      for (unsigned k = 0; k < GLASS_WAKE_INPUTS; k++)
        feature[k] = random_feature();
      for (unsigned t = 0; t < GLASS_WAKE_TEMPLATES; t++)
        {
          float offset = random_feature();
          for (unsigned k = 0; k < GLASS_WAKE_INPUTS; k++)
            bank[t * GLASS_WAKE_INPUTS + k] = round & 1 ?
                random_feature() : feature[k] + offset;
        }
      check_bank(bank, feature);
    }
  free(bank);
}

int main(void)
{
  test_rms();
  test_dtw();
  test_banks();
  printf("WAKE_RUNTIME_HOST_OK rms=%u dtw=%u bounded=%u banks=%u pruned=%u; "
         "bitwise RMS/full DTW/top3 ratio/positive distance; ASan/UBSan; host only\n",
         rms_checks, dtw_checks, bounded_checks, bank_checks, pruned);
  return 0;
}
