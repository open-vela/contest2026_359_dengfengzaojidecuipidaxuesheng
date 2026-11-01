#!/usr/bin/env python3
"""Compare actual SD receive function before/after diagnostic instrumentation.

Fault-injected SPI/ticks only.  This does not prove physical card reliability.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

from prepare_sd_read_diagnostics import BASE, DIAGNOSTIC, RELATIVE, apply, transform


HARNESS = r'''
#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define FAR
#define OK 0
#define ERROR (-1)
#define MMCSD_SPIDT_STARTBLKSNGL 0xfe
struct spi_dev_s
{
  unsigned int calls;
  unsigned int waits;
  unsigned int blocks;
  unsigned int crc;
  unsigned int ready;
  unsigned int error;
  clock_t step;
};
struct mmcsd_slot_s
{
  struct spi_dev_s *spi;
  clock_t taccess;
};
static clock_t g_now;
static unsigned int g_errors;
static char g_log[256];
static uint8_t send_byte(struct spi_dev_s *spi, uint8_t value)
{
  assert(value == 0xff);
  spi->calls++;
  g_now += spi->step;
  if (spi->blocks)
    {
      spi->crc++;
      assert(spi->crc <= 2);
      return 0xaa;
    }
  spi->waits++;
  if (spi->waits == spi->ready) return 0xfe;
  if (spi->waits == spi->error) return 0x0b;
  return 0xff;
}
static void recv_block(struct spi_dev_s *spi, uint8_t *buffer, int nbytes)
{
  assert(!spi->blocks && nbytes > 0 && nbytes <= 512);
  spi->blocks++;
  for (int i = 0; i < nbytes; i++) buffer[i] = (uint8_t)(i * 17);
}
static void error_log(const char *format, ...)
{
  va_list args;
  va_start(args, format);
  vsnprintf(g_log, sizeof(g_log), format, args);
  va_end(args);
  g_errors++;
}
#define START_TIME g_now
#define ELAPSED_TIME(start) (g_now - (start))
#define SPI_SEND(spi, value) send_byte(spi, value)
#define SPI_RECVBLOCK(spi, buffer, size) recv_block(spi, buffer, size)
#define ferr error_log
/* FUNCTIONS */

static void check(clock_t limit, clock_t step, unsigned int ready,
                  unsigned int error, int nbytes, clock_t initial)
{
  struct spi_dev_s baseline = {0};
  struct spi_dev_s diagnostic = {0};
  struct mmcsd_slot_s slot = {&baseline, limit};
  uint8_t before[512];
  uint8_t after[512];
  baseline.step = step;
  baseline.ready = ready;
  baseline.error = error;
  diagnostic = baseline;
  memset(before, 0xcc, sizeof(before));
  memset(after, 0xcc, sizeof(after));
  g_now = initial;
  g_errors = 0;
  int base_ret = baseline_recvblock(&slot, before, nbytes);
  clock_t base_time = g_now;
  unsigned int base_errors = g_errors;
  slot.spi = &diagnostic;
  g_now = initial;
  g_errors = 0;
  g_log[0] = 0;
  int diag_ret = diagnostic_recvblock(&slot, after, nbytes);
  assert(base_ret == diag_ret);
  assert(g_now == base_time && base_errors == g_errors);
  assert(!memcmp(&baseline, &diagnostic, sizeof(baseline)));
  assert(!memcmp(before, after, sizeof(before)));
  assert(diagnostic.calls < 1024);
  if (diag_ret == OK)
    {
      assert(g_errors == 0 && g_log[0] == 0);
      assert(diagnostic.blocks == 1 && diagnostic.crc == 2);
    }
  else
    {
      char expected[256];
      unsigned int token = diagnostic.waits == error ? 0x0b : 0xff;
      snprintf(expected, sizeof(expected),
               "ERROR: Did not receive data token (%02x)"
               " elapsed_ticks=%jd limit_ticks=%jd polls=%u\n",
               token, (intmax_t)(base_time - initial),
               (intmax_t)limit, diagnostic.waits);
      assert(g_errors == 1 && !strcmp(expected, g_log));
      assert(diagnostic.blocks == 0 && diagnostic.crc == 0);
    }
}
int main(void)
{
  unsigned int cases = 0;
  /* Immediate/delayed success, ff timeout, error tokens, a preemption-like
   * single SPI_SEND exceeding the limit, and FE exactly at/after the limit.
   */
  check(10, 1, 1, 0, 512, 0); cases++;
  check(10, 1, 5, 0, 512, 300); cases++;
  check(10, 1, 0, 0, 512, 0); cases++;
  check(10, 1, 0, 1, 512, 0); cases++;
  check(10, 1, 0, 5, 512, 0); cases++;
  check(10, 30, 0, 0, 512, 0); cases++;
  check(10, 1, 10, 0, 512, 0); cases++;
  check(10, 30, 1, 0, 512, 0); cases++;
  /* Original do/while always polls at least once, even with zero taccess. */
  check(0, 1, 0, 0, 512, 0); cases++;
  check(0, 1, 1, 0, 512, 0); cases++;
  for (int limit = 0; limit <= 13; limit++)
    for (int step = 1; step <= 7; step++)
      for (unsigned int ready = 0; ready <= 17; ready++)
        for (unsigned int error = 0; error <= 5; error++)
          {
            check(limit, step, ready, error, 16, 123456);
            cases++;
          }
  printf("{\"passed\":true,\"differential_cases\":%u}\n", cases);
  return 0;
}
'''


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--nuttx', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    raw = (args.nuttx / RELATIVE).read_bytes()
    source = raw.decode('utf-8')
    baseline = transform(source, False)
    diagnostic = transform(source, True)
    assert transform(diagnostic, True) == diagnostic
    assert transform(baseline, False) == baseline
    assert transform(diagnostic, False) == baseline
    assert baseline.replace(BASE, '') == diagnostic.replace(DIAGNOSTIC, '')
    (out / 'mmcsd_spi-before.c').write_bytes(baseline.encode())
    (out / 'mmcsd_spi-diagnostic.c').write_bytes(diagnostic.encode())
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        path = root / RELATIVE
        path.parent.mkdir(parents=True)
        path.write_bytes(baseline.encode())
        apply(root, True)
        assert path.read_bytes() == diagnostic.encode()
        apply(root, False)
        assert path.read_bytes() == baseline.encode()
        for bad in (baseline.replace('token == 0xff', 'token != 0xff'),
                    baseline + BASE, diagnostic.replace('polls++;', '')):
            path.write_bytes(bad.encode())
            try:
                apply(root, True)
            except RuntimeError:
                pass
            else:
                raise AssertionError('Unrecognized/partial code accepted')
            assert path.read_bytes() == bad.encode()
    functions = BASE.replace('mmcsd_recvblock(', 'baseline_recvblock(', 1)
    functions += '\n' + DIAGNOSTIC.replace(
        'mmcsd_recvblock(', 'diagnostic_recvblock(', 1)
    c_source = HARNESS.replace('/* FUNCTIONS */', functions)
    c_file = out / 'receive-differential.c'
    c_file.write_text(c_source, encoding='utf-8')
    executable = out / 'receive-differential'
    command = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', '-g',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               str(c_file), '-o', str(executable)]
    subprocess.run(command, check=True)
    result = subprocess.run([str(executable)], check=True, capture_output=True,
                            text=True)
    (out / 'result.log').write_text(result.stdout + result.stderr)
    details = json.loads(result.stdout)
    assert details['passed'] and details['differential_cases'] > 10000
    # File outside the receive function is byte-identical. In particular,
    # read/write locking, CMD12 cleanup and retry settings are not changed.
    report = {**details, 'scope': 'host SPI/tick fault injection only',
              'sanitizers': ['address', 'undefined'],
              'input_source_sha256': sha(raw),
              'baseline_source_sha256': sha(baseline.encode()),
              'diagnostic_source_sha256': sha(diagnostic.encode()),
              'prepared_tree_mutated': False,
              'outside_receive_function_identical': True,
              'normal_builder_restores_exact_original': True,
              'unknown_partial_duplicate_code_rejected_without_write': True,
              'timeout_clock_retry_write_policy_changed': False,
              'board_tested': False, 'root_cause_cured': False,
              'tool_sha256': sha(Path(__file__).read_bytes()),
              'prepare_sha256': sha(Path(__file__).with_name(
                  'prepare_sd_read_diagnostics.py').read_bytes())}
    (out / 'VERIFICATION.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
