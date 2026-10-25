/* SPDX-License-Identifier: Apache-2.0 */
/* Host-only CPU benchmark. All model/audio I/O and initialization precede timing. */
#define _POSIX_C_SOURCE 200809L
#include "glass_custom_wake.h"
#include "glass_wake_features.h"
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#define MAX_CLIPS 64
#define MAX_REPETITIONS 101
struct clip {
  int16_t *pcm;
  size_t samples;
  float feature[GLASS_WAKE_INPUTS];
};
struct runtime {
  int (*create)(const char *, struct glass_custom_wake **);
  void (*destroy)(struct glass_custom_wake *);
  int (*feed)(struct glass_custom_wake *, const int16_t *, size_t);
  int (*extract)(const int16_t *, size_t, float *);
  float (*match)(const float *, const float *, float *);
  struct glass_custom_wake *state;
  struct clip clips[MAX_CLIPS];
  unsigned clips_used;
  float *weights;
};
static volatile float result_sink;

static void fail(const char *message)
{ fprintf(stderr, "wake runtime benchmark: %s\n", message); exit(1); }

static double cpu_seconds(void)
{
  struct timespec value;
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value)) fail("CPU clock failed");
  return value.tv_sec + value.tv_nsec * 1e-9;
}

static void *read_file(const char *path, size_t *bytes)
{
  FILE *file = fopen(path, "rb");
  if (!file) fail("cannot open input");
  if (fseek(file, 0, SEEK_END)) fail("cannot seek input");
  long size = ftell(file);
  if (size < 0 || fseek(file, 0, SEEK_SET)) fail("cannot size input");
  void *data = malloc((size_t)size + 1);
  if (!data) fail("allocation failed");
  if (fread(data, 1, (size_t)size, file) != (size_t)size || ferror(file))
    fail("cannot read input");
  fclose(file); *bytes = (size_t)size; return data;
}

static void load_inputs(struct runtime *runtime, const char *root,
                        const char *list_path)
{
  /* The public loader authenticates this exact file before any measurements. */
  if (runtime->create(root, &runtime->state)) fail("model authentication failed");
  char path[4096];
  if (snprintf(path, sizeof(path), "%s/custom/nihao_openvela.gkw", root)
      >= (int)sizeof(path)) fail("model path too long");
  size_t bytes; uint8_t *model = read_file(path, &bytes);
  if (bytes != 32 + GLASS_WAKE_WEIGHTS * sizeof(float) || memcmp(model, "GKW3", 4))
    fail("model shape mismatch");
  runtime->weights = malloc(GLASS_WAKE_WEIGHTS * sizeof(float));
  if (!runtime->weights) fail("model allocation failed");
  memcpy(runtime->weights, model + 32, GLASS_WAKE_WEIGHTS * sizeof(float));
  free(model);
  FILE *list = fopen(list_path, "r");
  if (!list) fail("cannot open PCM list");
  char *line = NULL; size_t capacity = 0; ssize_t length;
  while ((length = getline(&line, &capacity, list)) >= 0) {
    while (length && (line[length - 1] == '\n' || line[length - 1] == '\r'))
      line[--length] = 0;
    if (!length) continue;
    if (runtime->clips_used == MAX_CLIPS) fail("too many benchmark clips");
    struct clip *clip = &runtime->clips[runtime->clips_used++];
    clip->pcm = read_file(line, &bytes); clip->samples = bytes / 2;
    if (bytes % 2 || clip->samples < 400 || clip->samples > GLASS_WAKE_MAX_SAMPLES)
      fail("invalid PCM size");
    if (runtime->extract(clip->pcm, clip->samples, clip->feature))
      fail("benchmark PCM has no extractable speech");
  }
  if (ferror(list) || !runtime->clips_used) fail("invalid PCM list");
  free(line); fclose(list);
}

static double run(struct runtime *runtime, unsigned kind, uint64_t iterations)
{
  int16_t silence[320] = {0}; float feature[GLASS_WAKE_INPUTS], distance;
  double before = cpu_seconds();
  for (uint64_t i = 0; i < iterations; i++) {
    if (kind == 0) {
      if (runtime->feed(runtime->state, silence, 320)) fail("idle stream emitted event/error");
    } else {
      struct clip *clip = &runtime->clips[i % runtime->clips_used];
      if (kind == 1) {
        if (runtime->extract(clip->pcm, clip->samples, feature)) fail("extract failed");
        result_sink = feature[i % GLASS_WAKE_INPUTS];
      } else {
        result_sink = runtime->match(runtime->weights, clip->feature, &distance);
        if (!isfinite(result_sink) || !isfinite(distance)) fail("matcher returned nonfinite value");
      }
    }
  }
  return cpu_seconds() - before;
}

static int compare_double(const void *a, const void *b)
{ double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }

static void benchmark(struct runtime *runtime, unsigned kind,
                      unsigned repetitions, double target)
{
  uint64_t iterations = runtime->clips_used * 4;
  /* Calibration chooses one fixed batch size, with each clip equally represented. */
  for (unsigned attempt = 0; attempt < 12; attempt++) {
    double elapsed = run(runtime, kind, iterations);
    if (elapsed >= target) break;
    double factor = elapsed > 0 ? target / elapsed * 1.1 : 32;
    if (factor < 2) factor = 2;
    if (factor > 32) factor = 32;
    iterations *= (uint64_t)factor;
    if (iterations > 50000000) fail("benchmark calibration exceeded limit");
  }
  double samples[MAX_REPETITIONS], sorted[MAX_REPETITIONS];
  for (unsigned i = 0; i < repetitions; i++)
    sorted[i] = samples[i] = run(runtime, kind, iterations) * 1e6 / iterations;
  qsort(sorted, repetitions, sizeof(double), compare_double);
  unsigned p95 = (unsigned)ceil(.95 * repetitions) - 1;
  printf("{\"iterations_per_sample\":%llu,\"repetitions\":%u,"
         "\"median_us\":%.9g,\"p95_us\":%.9g,\"min_us\":%.9g,"
         "\"max_us\":%.9g,\"samples_us\":[",
         (unsigned long long)iterations, repetitions, sorted[repetitions / 2],
         sorted[p95], sorted[0], sorted[repetitions - 1]);
  for (unsigned i = 0; i < repetitions; i++) printf("%s%.9g", i ? "," : "", samples[i]);
  printf("]}");
}

int main(int argc, char **argv)
{
  if (argc != 6) fail("usage: runner library.so runtime-root pcm-list repetitions target-ms");
  char *end; errno = 0; unsigned long repetitions = strtoul(argv[4], &end, 10);
  if (errno || *end || repetitions < 3 || repetitions > MAX_REPETITIONS || !(repetitions & 1))
    fail("repetitions must be odd and 3..101");
  double target = strtod(argv[5], &end) / 1000;
  if (*end || !isfinite(target) || target < .005 || target > 1) fail("target must be 5..1000 ms");
  void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!library) fail(dlerror());
  struct runtime runtime = {0};
#define LOAD(member, name) do { *(void **)(&runtime.member) = dlsym(library, name); \
  if (!runtime.member) fail("missing runtime symbol: " name); } while (0)
  LOAD(create, "glass_custom_wake_create"); LOAD(destroy, "glass_custom_wake_destroy");
  LOAD(feed, "glass_custom_wake_feed"); LOAD(extract, "glass_wake_extract");
  LOAD(match, "glass_wake_ratio_distance");
  load_inputs(&runtime, argv[2], argv[3]);
  for (unsigned kind = 0; kind < 3; kind++) run(&runtime, kind, runtime.clips_used * 3);
  printf("{\"clock\":\"CLOCK_PROCESS_CPUTIME_ID\",\"initialization_timed\":false,"
         "\"io_timed\":false,\"clips\":%u,\"idle_feed_320_samples\":", runtime.clips_used);
  benchmark(&runtime, 0, (unsigned)repetitions, target);
  printf(",\"extract\":"); benchmark(&runtime, 1, (unsigned)repetitions, target);
  printf(",\"matcher\":"); benchmark(&runtime, 2, (unsigned)repetitions, target);
  printf("}\n");
  runtime.destroy(runtime.state); free(runtime.weights);
  for (unsigned i = 0; i < runtime.clips_used; i++) free(runtime.clips[i].pcm);
  dlclose(library); return 0;
}
