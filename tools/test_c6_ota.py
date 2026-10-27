#!/usr/bin/env python3
"""Exercise the real C6 image parser and no-replay OTA transfer engine."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

from prepare_c6_ota import apply


def esp_image(chip=13):
    header = bytearray(24)
    header[0:4] = bytes([0xe9, 1, 2, 0x20])
    struct.pack_into('<H', header, 12, chip)
    header[23] = 1
    data = bytes(range(256)) * 3
    image = header + struct.pack('<II', 0x40800000, len(data)) + data
    checksum = 0xef
    for b in data:
        checksum ^= b
    image += bytes(15 - len(image) % 16) + bytes([checksum])
    return bytes(image) + hashlib.sha256(image).digest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apps', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--image', type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    src = repo / 'app/espdl-quickapp/overlay/apps/system/c6probe'
    desktop = src.parent / 'desktop'
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    valid = esp_image()
    (out / 'valid.bin').write_bytes(valid)
    (out / 'wrong-chip.bin').write_bytes(esp_image(18))
    # Strict preparation must be idempotent and reject unknown templates.
    prepared = out / 'prepared/system/c6probe'
    prepared.mkdir(parents=True, exist_ok=True)
    for name in ['esp_hosted_rpc.c', 'Makefile']:
        shutil.copyfile(args.apps / 'system/c6probe' / name, prepared / name)
    a = apply(prepared.parents[1])
    b = apply(prepared.parents[1])
    assert a == b
    make = (prepared / 'Makefile').read_text()
    assert make.index('CSRCS += c6_ota.c') > make.index('CSRCS   =')
    rpc = (prepared / 'esp_hosted_rpc.c').read_text()
    assert 'if (req == NULL || g_c6_ota_attempted' in rpc
    assert rpc.count('#include "c6_ota_rpc.inc"') == 1
    (prepared / 'esp_hosted_rpc.c').write_text('unsupported RPC owner\n')
    try:
        apply(prepared.parents[1])
    except ValueError:
        pass
    else:
        raise AssertionError('unknown RPC preparation must fail')
    harness = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "c6_ota.h"

static const uint8_t *original;
static size_t original_size, consumed;
static unsigned calls, fail_call, begins, ends;
static int exchange(enum c6_ota_operation_e operation, const uint8_t *data,
                    size_t size, void *arg)
{
  assert(arg == &calls);
  calls++;
  if (operation == C6_OTA_BEGIN) {
    assert(calls == 1 && !data && !size); begins++;
  } else if (operation == C6_OTA_WRITE) {
    assert(begins == 1 && !ends && size > 0 && size <= C6_OTA_CHUNK_SIZE);
    assert(data == original + consumed && size <= original_size - consumed);
    assert(!memcmp(data, original + consumed, size));
    /* Consume even a request whose reply times out. It MUST NOT replay. */
    consumed += size;
  } else {
    assert(operation == C6_OTA_END && consumed == original_size);
    assert(!data && !size && !ends); ends++;
  }
  return calls == fail_call ? -ETIMEDOUT : 0;
}

int main(int argc, char **argv)
{
  assert(argc == 3);
  FILE *f = fopen(argv[1], "rb"); assert(f);
  assert(!fseek(f, 0, SEEK_END)); long length = ftell(f); assert(length > 64);
  rewind(f); uint8_t *image = malloc((size_t)length + 1); assert(image);
  assert(fread(image, 1, (size_t)length, f) == (size_t)length); fclose(f);
  size_t n = (size_t)length;
  assert(c6_ota_validate(image, n, argv[2]) == 0);
  assert(c6_ota_validate(NULL, n, argv[2]) == -EINVAL);
  assert(c6_ota_validate(image, n, "bad") == -EINVAL);
  assert(c6_ota_validate(image, C6_OTA_SLOT_SIZE + 1, argv[2]) == -EINVAL);
  for (size_t cut = 0; cut < 64; cut++)
    assert(c6_ota_validate(image, cut, argv[2]) < 0);
  assert(c6_ota_validate(image, n - 1, argv[2]) < 0);
  image[n] = 0; assert(c6_ota_validate(image, n + 1, argv[2]) < 0);
  const size_t corrupt[] = {0, 1, 2, 12, 13, 23, 28, n - 33, n - 1};
  for (unsigned i = 0; i < sizeof(corrupt) / sizeof(corrupt[0]); i++) {
    image[corrupt[i]] ^= 0x80;
    assert(c6_ota_validate(image, n, argv[2]) < 0);
    image[corrupt[i]] ^= 0x80;
  }
  char wrong[65]; memset(wrong, '0', 64); wrong[64] = 0;
  assert(c6_ota_validate(image, n, wrong) == -EBADMSG);
  wrong[0] = 'x'; assert(c6_ota_validate(image, n, wrong) == -EINVAL);

  /* Use all chunk/tail boundaries and fail every single RPC position. */
  original = image;
  const size_t sizes[] = {1, 191, 192, 193, 384, 385, 800};
  for (unsigned j = 0; j < sizeof(sizes) / sizeof(sizes[0]); j++) {
    original_size = sizes[j];
    unsigned total = 2 + (unsigned)((original_size + 191) / 192);
    for (fail_call = 0; fail_call <= total; fail_call++) {
      calls = begins = ends = 0; consumed = 0;
      struct c6_ota_result_s result;
      int ret = c6_ota_transfer(image, original_size, exchange, &calls, &result);
      assert(result.begin_attempted == 1);
      if (!fail_call) {
        assert(!ret && result.committed && result.commit_attempted);
        assert(result.acknowledged == original_size && calls == total && ends == 1);
      } else {
        assert(ret == -ETIMEDOUT && result.error == -ETIMEDOUT);
        assert(!result.committed && calls == fail_call);
        assert(result.commit_attempted == (fail_call == total));
        assert(ends == (fail_call == total));
        if (fail_call == 1) assert(!result.acknowledged);
        else if (fail_call < total) assert(result.acknowledged < consumed);
      }
    }
  }
  struct c6_ota_result_s result;
  assert(c6_ota_transfer(NULL, n, exchange, &calls, &result) == -EINVAL);
  assert(!result.begin_attempted && !result.commit_attempted);
  free(image);
  puts("C6_OTA_CORE_OK: real image SHA/header/checksum; bounded chunks; every RPC timeout; no replay or early commit");
  return 0;
}
'''
    (out / 'core.c').write_text(harness)
    common = ['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
              '-Werror', '-fsanitize=address,undefined',
              '-fno-omit-frame-pointer', '-I' + str(src), '-I' + str(desktop)]
    subprocess.run([*common, '-Wno-misleading-indentation', '-c',
                    str(desktop / 'qpk_espdl_sha256.c'), '-o', str(out / 'sha.o')],
                   check=True)
    subprocess.run([*common, str(out / 'core.c'), str(src / 'c6_ota.c'),
                    str(out / 'sha.o'), '-o', str(out / 'core')],
                   check=True)
    # The NSH entry point has different libc dependencies from the core and
    # RPC fixture. Compile it independently even when no release is supplied.
    subprocess.run([*common, '-Dfopen=c6_ota_test_fopen', '-c',
                    str(src / 'c6_ota_command.c'),
                    '-o', str(out / 'command.o')], check=True)
    subprocess.run([str(out / 'core'), str(out / 'valid.bin'),
                    hashlib.sha256(valid).hexdigest()], check=True, timeout=30)
    if args.image:
        image = args.image.resolve()
        data = image.read_bytes()
        assert len(data) == 1113728
        digest = hashlib.sha256(data).hexdigest()
        assert digest == '087112e8e08ada7f91ee109de1e0f81553466d22482787df7d3c41178439adaf'
        subprocess.run([str(out / 'core'), str(image), digest], check=True, timeout=30)
        sdk = args.apps / 'system/c6probe'
        # Compile against the real imported protobuf ABI/serializer.
        subprocess.run([*common, '-I' + str(sdk),
                        str(repo / 'app/espdl-quickapp/tests/c6_ota_rpc_host.c'),
                        str(src / 'c6_ota.c'), str(out / 'sha.o'),
                        str(sdk / 'esp_hosted_rpc.pb-c.c'),
                        str(sdk / 'protobuf-c.c'), '-pthread',
                        '-o', str(out / 'rpc')], check=True)
        subprocess.run([str(out / 'rpc'), str(image)], check=True, timeout=30)
        command_harness = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "c6_ota.h"
#include "c6_ota_release.h"

static const char *release_path;
static unsigned opened, applied;

FILE *c6_ota_test_fopen(const char *path, const char *mode)
{
  assert(!strcmp(path, "/sdcard/release.bin"));
  assert(!strcmp(mode, "rb"));
  opened++;
  return fopen(release_path, mode);
}

int esp_hosted_rpc_c6_ota(const uint8_t *image, size_t size,
                        struct c6_ota_result_s *result)
{
  assert(image && size == C6_OTA_RELEASE_BYTES);
  assert(!c6_ota_validate(image, size, C6_OTA_RELEASE_SHA256));
  assert(result && !result->begin_attempted);
  applied++;
  result->error = -EBUSY;
  return -EBUSY;
}

int main(int argc, char **argv)
{
  assert(argc == 2);
  release_path = argv[1];
  char *args[] = {"c6probe", "ota", "check", "/sdcard/release.bin",
                  C6_OTA_RELEASE_SHA256};
  assert(c6_ota_command(4, args) != 0);
  assert(!opened && !applied);
  args[3] = "/data/release.bin";
  assert(c6_ota_command(5, args) != 0);
  assert(!opened && !applied);
  args[3] = "/sdcard/release.bin";
  args[4] = "bad";
  assert(c6_ota_command(5, args) != 0);
  assert(opened == 1 && !applied);
  args[4] = C6_OTA_RELEASE_SHA256;
  assert(c6_ota_command(5, args) == 0);
  assert(opened == 2 && !applied);
  args[2] = "apply";
  assert(c6_ota_command(5, args) != 0);
  assert(opened == 3 && applied == 1);
  puts("C6_OTA_COMMAND_OK: command ABI; SD-only input; read-only check; apply error propagation");
  return 0;
}
'''
        (out / 'command.c').write_text(command_harness)
        subprocess.run([*common, str(out / 'command.c'),
                        str(src / 'c6_ota.c'), str(out / 'command.o'),
                        str(out / 'sha.o'), '-o', str(out / 'command')],
                       check=True)
        subprocess.run([str(out / 'command'), str(image)],
                       check=True, timeout=30)
    (out / 'RESULT.json').write_text(json.dumps({
        'host_only': True, 'real_companion_updated': False,
        'image_parser_and_failure_boundaries': True,
        'preparation_idempotent_and_unknown_owner_rejected': True,
        'command_entry_compiled': True,
        'command_read_only_check_and_apply_errors': bool(args.image),
        'approved_image_checked': bool(args.image)}, indent=2) + '\n')


if __name__ == '__main__':
    main()
