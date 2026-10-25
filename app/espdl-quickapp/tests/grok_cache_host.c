/* SPDX-License-Identifier: Apache-2.0 */
/* Standalone lossless codec, allocation fallback and shared-budget checks. */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static atomic_bool fail_malloc, fail_calloc;
static atomic_uint malloc_calls, calloc_calls;
static void *injected_malloc(size_t size)
{
  atomic_fetch_add(&malloc_calls, 1);
  if (atomic_exchange(&fail_malloc, false)) return NULL;
  return malloc(size);
}
static void *injected_calloc(size_t count, size_t size)
{
  atomic_fetch_add(&calloc_calls, 1);
  if (atomic_exchange(&fail_calloc, false)) return NULL;
  return calloc(count, size);
}
#define malloc injected_malloc
#define calloc injected_calloc
#include "glass_grok_cache.h"
#undef malloc
#undef calloc

static unsigned roundtrips;
static uint32_t random_state = UINT32_C(0x4a6b28e1);
static uint32_t random_word(void)
{
  random_state ^= random_state << 13;
  random_state ^= random_state >> 17;
  random_state ^= random_state << 5;
  return random_state;
}
static void fill_pixels(uint8_t *pixels, size_t count, unsigned pattern)
{
  for (size_t i = 0; i < count; i++) {
    unsigned color = pattern == 0 ? 0x1234 : pattern == 1 ? i / 255 :
                     pattern == 2 ? i / 16 : random_word();
    pixels[i * 2] = color;
    pixels[i * 2 + 1] = color >> 8;
    pixels[count * 2 + i] = pattern < 3 ? 0xe7 : random_word();
  }
}
static void codec_roundtrip(size_t count, unsigned pattern)
{
  size_t raw_size = count * 3;
  uint8_t *raw = malloc(raw_size + 2), *saved = malloc(raw_size + 2);
  uint8_t *out = malloc(raw_size + 2);
  assert(raw && saved && out);
  memset(raw, 0xa5, raw_size + 2);
  fill_pixels(raw + 1, count, pattern);
  memcpy(saved, raw, raw_size + 2);
  size_t size = grok_rle_size(raw + 1, count);
  assert(size <= count * 5);
  uint8_t *encoded = malloc(size + 2);
  assert(encoded);
  memset(encoded, 0x5a, size + 2);
  grok_rle_encode(encoded + 1, raw + 1, count);
  assert(encoded[0] == 0x5a && encoded[size + 1] == 0x5a);
  memset(out, 0xa5, raw_size + 2);
  assert(grok_rle_decode(out + 1, count, encoded + 1, size));
  assert(!memcmp(raw, saved, raw_size + 2));
  assert(!memcmp(out, raw, raw_size + 2));
  if (size) {
    const size_t lengths[] = {0, 1, 2, size / 2, size - 1};
    for (unsigned i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
      memset(out, 0xa5, raw_size + 2);
      assert(!grok_rle_decode(out + 1, count, encoded + 1, lengths[i]));
      assert(out[0] == 0xa5 && out[raw_size + 1] == 0xa5);
    }
  }
  free(encoded); free(out); free(saved); free(raw); roundtrips++;
}
static void codec_checks(void)
{
  const size_t counts[] = {0, 1, 2, 254, 255, 256, 257, 511, 512, 4096, 16384};
  for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); i++)
    for (unsigned pattern = 0; pattern < 4; pattern++)
      codec_roundtrip(counts[i], pattern);
  for (unsigned i = 0; i < 256; i++) codec_roundtrip(random_word() % 4097, 3);
  uint8_t out[5], good[] = {1, 0x34, 0x12, 1, 0xff, 0};
  const uint8_t malformed[][5] = {
    {0, 0x34, 0x12, 1, 0xff}, {2, 0x34, 0x12, 1, 0xff},
    {1, 0x34, 0x12, 0, 0xff}, {1, 0x34, 0x12, 2, 0xff}
  };
  for (unsigned i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
    memset(out, 0xa5, sizeof(out));
    assert(!grok_rle_decode(out + 1, 1, malformed[i], sizeof(malformed[i])));
    assert(out[0] == 0xa5 && out[4] == 0xa5);
  }
  assert(!grok_rle_decode(out + 1, 1, good, sizeof(good)));
}
static void cache_checks(void)
{
  enum { COUNT = 4096 };
  uint8_t raw[COUNT * 3], saved[COUNT * 3], out[COUNT * 3];
  struct grok_frame_cache cache = {0};
  fill_pixels(raw, COUNT, 0); memcpy(saved, raw, sizeof(raw));
  assert(!grok_frame_cache_load(&cache, 0, out, COUNT));
  assert(!grok_frame_cache_prepare(&cache, 0));
  grok_frame_cache_clear(&cache);
  assert(!grok_frame_cache_prepare(&cache, GROK_FRAME_CACHE_MAX_FRAMES + 1));
  grok_frame_cache_clear(&cache);
  assert(grok_frame_cache_prepare(&cache, 2));
  size_t metadata = 2 * sizeof(struct grok_frame_cache_entry);
  assert(grok_frame_cache_total() == metadata);
  assert(!grok_frame_cache_prepare(&cache, 3));
  grok_frame_cache_store(&cache, 2, raw, COUNT);
  assert(grok_frame_cache_total() == metadata);
  grok_frame_cache_store(&cache, 0, raw, COUNT);
  size_t used = grok_frame_cache_total();
  assert(used == metadata + grok_rle_size(raw, COUNT));
  assert(!memcmp(raw, saved, sizeof(raw)));
  memset(raw, 0, sizeof(raw));
  assert(grok_frame_cache_load(&cache, 0, out, COUNT));
  assert(!memcmp(out, saved, sizeof(out)));
  /* Gaze can mutate a backbuffer without changing subsequent cached pixels. */
  memset(out, 0xff, sizeof(out));
  assert(grok_frame_cache_load(&cache, 0, out, COUNT));
  assert(!memcmp(out, saved, sizeof(out)));
  grok_frame_cache_store(&cache, 0, raw, COUNT);
  assert(grok_frame_cache_total() == used);
  /* Bad internal cache data falls back safely and releases only its payload. */
  cache.entries[0].data[0] = 0;
  assert(!grok_frame_cache_load(&cache, 0, out, COUNT));
  assert(grok_frame_cache_total() == metadata);
  grok_frame_cache_clear(&cache); grok_frame_cache_clear(&cache);
  assert(!grok_frame_cache_total());

  atomic_store(&fail_calloc, true);
  assert(!grok_frame_cache_prepare(&cache, 2));
  assert(!grok_frame_cache_total());
  unsigned calls = atomic_load(&calloc_calls);
  assert(!grok_frame_cache_prepare(&cache, 2));
  assert(atomic_load(&calloc_calls) == calls);
  grok_frame_cache_clear(&cache);
  assert(grok_frame_cache_prepare(&cache, 2));
  fill_pixels(raw, COUNT, 0);
  atomic_store(&fail_malloc, true);
  grok_frame_cache_store(&cache, 0, raw, COUNT);
  assert(grok_frame_cache_total() == metadata);
  assert(!grok_frame_cache_load(&cache, 0, out, COUNT));
  calls = atomic_load(&malloc_calls);
  grok_frame_cache_store(&cache, 0, raw, COUNT);
  assert(atomic_load(&malloc_calls) == calls);
  fill_pixels(raw, COUNT, 3);
  assert(grok_rle_size(raw, COUNT) >= sizeof(raw));
  grok_frame_cache_store(&cache, 1, raw, COUNT);
  assert(!grok_frame_cache_load(&cache, 1, out, COUNT));
  assert(grok_frame_cache_total() == metadata);
  grok_frame_cache_clear(&cache);
  assert(!grok_frame_cache_total());

  assert(grok_cache_reserve(GROK_FRAME_CACHE_BUDGET));
  assert(!grok_cache_reserve(1));
  assert(!grok_frame_cache_prepare(&cache, 2));
  assert(grok_frame_cache_total() == GROK_FRAME_CACHE_BUDGET);
  grok_cache_release(GROK_FRAME_CACHE_BUDGET);
  grok_frame_cache_clear(&cache);
  assert(!grok_frame_cache_total());
}

enum { WORKERS = 8, FRAMES = 32, PIXELS = 128 * 128 };
static pthread_barrier_t stored_barrier, release_barrier;
static atomic_uint stored_frames;
static void *budget_worker(void *opaque)
{
  (void)opaque;
  struct grok_frame_cache cache = {0};
  uint8_t *raw = malloc(PIXELS * 3), *out = malloc(PIXELS * 3);
  assert(raw && out); fill_pixels(raw, PIXELS, 2);
  if (grok_frame_cache_prepare(&cache, FRAMES)) {
    for (unsigned frame = 0; frame < FRAMES; frame++) {
      grok_frame_cache_store(&cache, frame, raw, PIXELS);
      assert(grok_frame_cache_total() <= GROK_FRAME_CACHE_BUDGET);
      if (grok_frame_cache_load(&cache, frame, out, PIXELS)) {
        assert(!memcmp(out, raw, PIXELS * 3));
        atomic_fetch_add(&stored_frames, 1);
      }
    }
  }
  pthread_barrier_wait(&stored_barrier);
  pthread_barrier_wait(&release_barrier);
  grok_frame_cache_clear(&cache); free(raw); free(out); return NULL;
}
static void concurrent_budget_checks(void)
{
  pthread_t workers[WORKERS];
  assert(!pthread_barrier_init(&stored_barrier, NULL, WORKERS + 1));
  assert(!pthread_barrier_init(&release_barrier, NULL, WORKERS + 1));
  for (unsigned i = 0; i < WORKERS; i++)
    assert(!pthread_create(&workers[i], NULL, budget_worker, NULL));
  pthread_barrier_wait(&stored_barrier);
  assert(grok_frame_cache_total() <= GROK_FRAME_CACHE_BUDGET);
  assert(atomic_load(&stored_frames) > 0);
  assert(atomic_load(&stored_frames) < WORKERS * FRAMES);
  pthread_barrier_wait(&release_barrier);
  for (unsigned i = 0; i < WORKERS; i++) assert(!pthread_join(workers[i], NULL));
  pthread_barrier_destroy(&stored_barrier);
  pthread_barrier_destroy(&release_barrier);
  assert(!grok_frame_cache_total());
}
int main(void)
{
  codec_checks(); cache_checks(); concurrent_budget_checks();
  printf("GROK_CACHE_HOST_OK roundtrips=%u concurrent_cached_frames=%u budget=%u; "
         "run boundaries; malformed streams; immutable raw pixels; allocation "
         "fallback; shared budget; full cleanup\n", roundtrips,
         atomic_load(&stored_frames), (unsigned)GROK_FRAME_CACHE_BUDGET);
  return 0;
}
