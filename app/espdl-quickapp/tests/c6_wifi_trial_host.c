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
#include "c6_wifi_trial.h"

static pthread_mutex_t g_rpc_transaction_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_c6_ota_attempted;
static unsigned g_rpc;
static unsigned calls;
static unsigned fail_at;
static unsigned malformed;
static unsigned flash_writes;
static bool connected;
static int storage;
static uint8_t ram[512];
static uint8_t flash[512];
static uint8_t old[512];
static size_t ram_size;
static size_t flash_size;
static size_t old_size;

static unsigned c6_rpc_next_uid(unsigned *sequence)
{
  return ++*sequence;
}

static Rpc *rpc_transact_locked(const Rpc *request, const char *label)
{
  uint8_t wire[1024];
  RpcReqWifiSetConfig *set;
  Rpc response = RPC__INIT;
  RpcRespWifiGetConfig get = RPC__RESP__WIFI_GET_CONFIG__INIT;
  RpcRespWifiSetConfig config = RPC__RESP__WIFI_SET_CONFIG__INIT;
  RpcRespWifiSetStorage store = RPC__RESP__WIFI_SET_STORAGE__INIT;
  RpcRespWifiConnect start = RPC__RESP__WIFI_CONNECT__INIT;
  RpcRespWifiDisconnect stop = RPC__RESP__WIFI_DISCONNECT__INIT;
  size_t length = rpc__get_packed_size(request);
  Rpc *decoded;
  WifiConfig *current = NULL;

  assert(!strcmp(label, "WifiSetConfig")); /* No credential hexdumps. */
  assert(length <= 256);
  assert(rpc__pack(request, wire) == length);
  decoded = rpc__unpack(NULL, length, wire);
  assert(decoded && decoded->uid && decoded->msg_type == RPC_TYPE__Req);
  calls++;
  response.msg_type = RPC_TYPE__Resp;
  response.msg_id = decoded->msg_id + 256;
  response.uid = decoded->uid;
  switch (decoded->msg_id)
    {
      case RPC_ID__Req_WifiGetConfig:
        assert(decoded->payload_case == RPC__PAYLOAD_REQ_WIFI_GET_CONFIG);
        assert(decoded->req_wifi_get_config->iface == 0);
        current = wifi_config__unpack(NULL, ram_size, ram);
        assert(current);
        get.cfg = current;
        get.iface = 0;
        response.payload_case = RPC__PAYLOAD_RESP_WIFI_GET_CONFIG;
        response.resp_wifi_get_config = &get;
        break;
      case RPC_ID__Req_WifiSetStorage:
        assert(decoded->payload_case == RPC__PAYLOAD_REQ_WIFI_SET_STORAGE);
        storage = decoded->req_wifi_set_storage->storage;
        assert(storage == 0 || storage == 1);
        response.payload_case = RPC__PAYLOAD_RESP_WIFI_SET_STORAGE;
        response.resp_wifi_set_storage = &store;
        break;
      case RPC_ID__Req_WifiSetConfig:
        assert(decoded->payload_case == RPC__PAYLOAD_REQ_WIFI_SET_CONFIG);
        set = decoded->req_wifi_set_config;
        assert(set && set->iface == 0 && set->cfg);
        assert(set->cfg->u_case == WIFI_CONFIG__U_STA);
        ram_size = wifi_config__get_packed_size(set->cfg);
        assert(ram_size <= sizeof(ram));
        assert(wifi_config__pack(set->cfg, ram) == ram_size);
        if (storage == 0)
          {
            flash_writes++;
            memcpy(flash, ram, ram_size);
            flash_size = ram_size;
          }
        response.payload_case = RPC__PAYLOAD_RESP_WIFI_SET_CONFIG;
        response.resp_wifi_set_config = &config;
        break;
      case RPC_ID__Req_WifiConnect:
        assert(decoded->payload_case == RPC__PAYLOAD_REQ_WIFI_CONNECT);
        assert(!connected);
        connected = true;
        response.payload_case = RPC__PAYLOAD_RESP_WIFI_CONNECT;
        response.resp_wifi_connect = &start;
        break;
      case RPC_ID__Req_WifiDisconnect:
        assert(decoded->payload_case == RPC__PAYLOAD_REQ_WIFI_DISCONNECT);
        stop.resp = connected ? 0 : 0x300f;
        connected = false;
        response.payload_case = RPC__PAYLOAD_RESP_WIFI_DISCONNECT;
        response.resp_wifi_disconnect = &stop;
        break;
      default:
        assert(false);
    }

  rpc__free_unpacked(decoded, NULL);
  /* Apply before losing the reply: rollback must cover uncertain writes. */
  if (calls == fail_at && !malformed)
    {
      if (current) wifi_config__free_unpacked(current, NULL);
      return NULL;
    }

  if (calls == fail_at)
    {
      if (malformed == 1) response.msg_type = RPC_TYPE__Event;
      if (malformed == 2) response.msg_id++;
      if (malformed == 3) response.uid++;
      if (malformed == 4) response.payload_case = RPC__PAYLOAD__NOT_SET;
      if (malformed == 5) get.resp = store.resp = config.resp =
                           start.resp = stop.resp = 0x102;
    }

  length = rpc__get_packed_size(&response);
  assert(length <= sizeof(wire));
  assert(rpc__pack(&response, wire) == length);
  Rpc *reply = rpc__unpack(NULL, length, wire);
  if (current) wifi_config__free_unpacked(current, NULL);
  return reply;
}

#include "c6_wifi_trial_rpc.inc"

static void reset(void)
{
  assert(!c6_wifi_trial_pending());
  assert(!g_c6_wifi_trial);
  WifiConfig cfg = WIFI_CONFIG__INIT;
  WifiStaConfig sta = WIFI_STA_CONFIG__INIT;
  WifiScanThreshold threshold = WIFI_SCAN_THRESHOLD__INIT;
  WifiPmfConfig pmf = WIFI_PMF_CONFIG__INIT;
  uint8_t ssid[32] = "previous-network";
  uint8_t password[64] = "previous-password";
  uint8_t bssid[6] = {1, 2, 3, 4, 5, 6};
  uint8_t identifier[8] = "old-id";
  sta.ssid = (ProtobufCBinaryData){sizeof(ssid), ssid};
  sta.password = (ProtobufCBinaryData){sizeof(password), password};
  sta.bssid = (ProtobufCBinaryData){sizeof(bssid), bssid};
  sta.sae_h2e_identifier = (ProtobufCBinaryData){sizeof(identifier), identifier};
  sta.channel = 11;
  sta.listen_interval = 3;
  sta.failure_retry_cnt = 2;
  sta.bitmask = 0x1234;
  sta.he_bitmask = 0x4321;
  sta.threshold = &threshold;
  sta.pmf_cfg = &pmf;
  pmf.capable = true;
  cfg.u_case = WIFI_CONFIG__U_STA;
  cfg.sta = &sta;
  old_size = wifi_config__get_packed_size(&cfg);
  assert(old_size <= sizeof(old));
  assert(wifi_config__pack(&cfg, old) == old_size);
  memcpy(ram, old, old_size);
  memcpy(flash, old, old_size);
  ram_size = flash_size = old_size;
  calls = fail_at = malformed = flash_writes = 0;
  storage = 0;
  connected = true;
  g_c6_ota_attempted = false;
}

static void old_persisted(void)
{
  assert(flash_size == old_size && !memcmp(flash, old, old_size));
}

static void restored(void)
{
  assert(!c6_wifi_trial_pending());
  assert(storage == 0 && !connected);
  assert(ram_size == old_size && !memcmp(ram, old, old_size));
  old_persisted();
}

int main(void)
{
  reset();
  assert(c6_wifi_trial_begin(NULL, "password") == -EINVAL);
  assert(c6_wifi_trial_begin("", "password") == -EINVAL);
  assert(c6_wifi_trial_begin("new", "short") == -EINVAL);
  assert(!calls);
  g_c6_ota_attempted = true;
  assert(c6_wifi_trial_begin("new", "password") == -ESHUTDOWN);
  assert(!calls && !c6_wifi_trial_pending());

  /* Every begin RPC: timeout or malformed/remote-error reply, no FLASH save. */
  for (unsigned bad = 0; bad <= 5; bad++)
    for (unsigned position = 1; position <= 5; position++)
      {
        reset();
        malformed = bad;
        fail_at = position;
        assert(c6_wifi_trial_begin("new-network", "new-password") < 0);
        assert(calls == position && !flash_writes);
        old_persisted();
        if (position == 1)
          {
            assert(!c6_wifi_trial_pending());
          }
        else
          {
            assert(c6_wifi_trial_pending());
            fail_at = 0;
            assert(c6_wifi_trial_finish(true) == -EINVAL);
            assert(!c6_wifi_trial_finish(false));
            restored();
          }
      }

  /* Read-only validation success still never commits; abort is explicit. */
  reset();
  assert(!c6_wifi_trial_begin("new-network", "new-password"));
  assert(c6_wifi_trial_pending() && connected && storage == 1);
  old_persisted();
  assert(!flash_writes);
  unsigned before = calls;
  assert(c6_wifi_trial_begin("another", "password") == -EBUSY);
  assert(calls == before);
  assert(!c6_wifi_trial_finish(false));
  restored();
  assert(!flash_writes);

  /* Every commit uncertainty retains snapshot; abort restores old FLASH. */
  for (unsigned position = 1; position <= 2; position++)
    {
      reset();
      assert(!c6_wifi_trial_begin("new-network", "new-password"));
      fail_at = calls + position;
      assert(c6_wifi_trial_finish(true) == -ETIMEDOUT);
      assert(c6_wifi_trial_pending());
      fail_at = 0;
      assert(!c6_wifi_trial_finish(false));
      restored();
    }

  /* Every rollback RPC failure leaves state retained for explicit recovery. */
  for (unsigned position = 1; position <= 4; position++)
    {
      reset();
      assert(!c6_wifi_trial_begin("new-network", "new-password"));
      fail_at = calls + position;
      assert(c6_wifi_trial_finish(false) == -ETIMEDOUT);
      assert(c6_wifi_trial_pending());
      old_persisted();
      fail_at = 0;
      assert(!c6_wifi_trial_finish(false));
      restored();
    }

  reset();
  assert(!c6_wifi_trial_begin("new-network", "new-password"));
  assert(!c6_wifi_trial_finish(true));
  assert(!c6_wifi_trial_pending() && storage == 0 && connected);
  assert(flash_writes == 1 && ram_size == flash_size);
  assert(!memcmp(ram, flash, ram_size));
  assert(flash_size != old_size || memcmp(flash, old, old_size));
  assert(c6_wifi_trial_finish(false) == -ENOENT);
  puts("C6_WIFI_TRIAL_OK: real protobuf; RAM trial; whole-config rollback; every RPC fault; uncertain commit recovery; OTA exclusion; no credential logs");
  return 0;
}
