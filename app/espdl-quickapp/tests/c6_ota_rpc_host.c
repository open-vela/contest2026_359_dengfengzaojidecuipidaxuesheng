/* SPDX-License-Identifier: Apache-2.0 */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_hosted_rpc.pb-c.h"
#include "c6_ota.h"
#include "c6_ota_release.h"

static unsigned g_rpc;
static pthread_mutex_t g_rpc_transaction_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_c6_ota_attempted;
static bool trial_pending;
static unsigned calls;
static unsigned malformed;
static size_t acknowledged;
static const uint8_t *expected_image;
static size_t expected_size;

bool c6_wifi_trial_pending(void)
{
  return trial_pending;
}

static unsigned c6_rpc_next_uid(unsigned *sequence)
{
  return ++*sequence;
}

static Rpc *rpc_transact_locked(const Rpc *req, const char *label)
{
  uint8_t wire[256];
  RpcRespOTABegin begin = RPC__RESP__OTABEGIN__INIT;
  RpcRespOTAWrite write = RPC__RESP__OTAWRITE__INIT;
  RpcRespOTAEnd end = RPC__RESP__OTAEND__INIT;
  Rpc response = RPC__INIT;
  size_t length = rpc__get_packed_size(req);
  Rpc *decoded;

  assert(!strcmp(label, "C6OTA"));
  assert(length <= sizeof(wire));
  assert(rpc__pack(req, wire) == length);
  decoded = rpc__unpack(NULL, length, wire);
  assert(decoded && decoded->uid == req->uid && decoded->uid != 0);
  assert(decoded->msg_type == RPC_TYPE__Req);
  calls++;
  response.msg_type = RPC_TYPE__Resp;
  response.msg_id = decoded->msg_id + 256;
  response.uid = decoded->uid;
  if (decoded->msg_id == RPC_ID__Req_OTABegin)
    {
      assert(decoded->payload_case == RPC__PAYLOAD_REQ_OTA_BEGIN);
      response.payload_case = RPC__PAYLOAD_RESP_OTA_BEGIN;
      response.resp_ota_begin = &begin;
    }
  else if (decoded->msg_id == RPC_ID__Req_OTAWrite)
    {
      assert(decoded->payload_case == RPC__PAYLOAD_REQ_OTA_WRITE);
      assert(decoded->req_ota_write);
      size_t n = decoded->req_ota_write->ota_data.len;
      assert(n > 0 && n <= 192 && n <= expected_size - acknowledged);
      assert(!memcmp(decoded->req_ota_write->ota_data.data,
                     expected_image + acknowledged, n));
      acknowledged += n;
      response.payload_case = RPC__PAYLOAD_RESP_OTA_WRITE;
      response.resp_ota_write = &write;
    }
  else
    {
      assert(decoded->msg_id == RPC_ID__Req_OTAEnd);
      assert(decoded->payload_case == RPC__PAYLOAD_REQ_OTA_END);
      assert(acknowledged == expected_size);
      response.payload_case = RPC__PAYLOAD_RESP_OTA_END;
      response.resp_ota_end = &end;
    }

  rpc__free_unpacked(decoded, NULL);
  if (malformed == 1) return NULL;
  if (malformed == 2) response.msg_type = RPC_TYPE__Event;
  if (malformed == 3) response.msg_id++;
  if (malformed == 4) response.payload_case = RPC__PAYLOAD__NOT_SET;
  if (malformed == 5) begin.resp = write.resp = end.resp = 0x102;
  length = rpc__get_packed_size(&response);
  assert(length <= sizeof(wire));
  assert(rpc__pack(&response, wire) == length);
  return rpc__unpack(NULL, length, wire);
}

#include "c6_ota_rpc.inc"

int main(int argc, char **argv)
{
  assert(argc == 2);
  FILE *f = fopen(argv[1], "rb");
  assert(f);
  uint8_t *image = malloc(C6_OTA_RELEASE_BYTES);
  assert(image);
  assert(fread(image, 1, C6_OTA_RELEASE_BYTES, f) == C6_OTA_RELEASE_BYTES);
  assert(fgetc(f) == EOF && !ferror(f));
  fclose(f);
  expected_image = image;
  expected_size = C6_OTA_RELEASE_BYTES;
  struct c6_ota_result_s result;

  assert(esp_hosted_rpc_c6_ota(image, 1, &result) == -EPERM);
  assert(!calls && !g_c6_ota_attempted);
  image[100] ^= 1;
  assert(esp_hosted_rpc_c6_ota(image, expected_size, &result) < 0);
  assert(!calls && !g_c6_ota_attempted);
  image[100] ^= 1;
  assert(!pthread_mutex_lock(&g_rpc_transaction_lock));
  assert(esp_hosted_rpc_c6_ota(image, expected_size, &result) == -EBUSY);
  assert(!g_c6_ota_attempted);
  assert(!pthread_mutex_unlock(&g_rpc_transaction_lock));

  trial_pending = true;
  assert(esp_hosted_rpc_c6_ota(image, expected_size, &result) == -EBUSY);
  assert(!g_c6_ota_attempted && !calls);
  trial_pending = false;

  for (malformed = 1; malformed <= 5; malformed++)
    {
      g_c6_ota_attempted = false; /* Simulate a fresh host boot in fixture. */
      unsigned before = calls;
      assert(esp_hosted_rpc_c6_ota(image, expected_size, &result) < 0);
      assert(result.begin_attempted && !result.commit_attempted);
      assert(g_c6_ota_attempted && calls == before + 1);
      assert(esp_hosted_rpc_c6_ota(image, expected_size, &result) == -EALREADY);
      assert(calls == before + 1);
    }

  malformed = 0;
  calls = 0;
  g_c6_ota_attempted = false;
  assert(!esp_hosted_rpc_c6_ota(image, expected_size, &result));
  assert(result.committed && result.acknowledged == expected_size);
  assert(acknowledged == expected_size);
  assert(calls == 2 + (expected_size + 191) / 192);
  unsigned before = calls;
  assert(esp_hosted_rpc_c6_ota(image, expected_size, &result) == -EALREADY);
  assert(calls == before);
  free(image);
  puts("C6_OTA_RPC_OK: actual protobuf packing; 256-byte budget; release gate; exclusive maintenance; malformed/failed replies; terminal attempt guard");
  return 0;
}
