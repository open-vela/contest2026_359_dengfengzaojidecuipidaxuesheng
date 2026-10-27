/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
static pthread_mutex_t g_rpc_transaction_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_c6_ota_attempted;
bool c6_wifi_trial_pending(void) { return false; }
#define rpc_transact_locked rpc_transact
static unsigned g_rpc;
static unsigned c6_rpc_next_uid(unsigned *sequence) { return ++*sequence; }

#include "esp_hosted_rpc.pb-c.h"
#include "c6_softap.h"

#define FAR
#define RPC_WIFI_IF_AP 1

static unsigned calls;
static unsigned fault;
static const char *test_ssid = "ESPClaw-Recovery-Unit";
static const char *test_password = "test-only-Pass-123";

static Rpc *rpc_transact(const Rpc *req, const char *label)
{
  Rpc resp = RPC__INIT;
  RpcRespWifiStop stopped = RPC__RESP__WIFI_STOP__INIT;
  RpcRespGetMode mode = RPC__RESP__GET_MODE__INIT;
  RpcRespWifiSetConfig set = RPC__RESP__WIFI_SET_CONFIG__INIT;
  RpcRespWifiGetConfig get = RPC__RESP__WIFI_GET_CONFIG__INIT;
  WifiConfig cfg = WIFI_CONFIG__INIT;
  WifiApConfig ap = WIFI_AP_CONFIG__INIT;
  WifiPmfConfig pmf = WIFI_PMF_CONFIG__INIT;
  uint8_t ssid[32] = {0};
  uint8_t password[64] = {0};
  uint8_t wire[512];

  calls++;
  assert(rpc__get_packed_size(req) <= 256);
  assert(req->msg_type == RPC_TYPE__Req);
  if (fault == 1) return NULL;
  resp.msg_type = RPC_TYPE__Resp;
  resp.msg_id = req->msg_id + 256;
  if (req->msg_id == RPC_ID__Req_WifiStop) {
    assert(req->payload_case == RPC__PAYLOAD_REQ_WIFI_STOP);
    assert(!strcmp(label, "WifiStop"));
    resp.payload_case = RPC__PAYLOAD_RESP_WIFI_STOP;
    resp.resp_wifi_stop = &stopped;
    stopped.resp = fault == 2 ? 123 : 0;
  } else if (req->msg_id == RPC_ID__Req_GetWifiMode) {
    assert(req->payload_case == RPC__PAYLOAD_REQ_GET_WIFI_MODE);
    resp.payload_case = RPC__PAYLOAD_RESP_GET_WIFI_MODE;
    resp.resp_get_wifi_mode = &mode;
    mode.mode = 3;
    mode.resp = fault == 2 ? 123 : 0;
  } else if (req->msg_id == RPC_ID__Req_WifiSetConfig) {
    assert(!strcmp(label, "WifiSetConfig")); /* Redacted transaction path. */
    assert(req->req_wifi_set_config->iface == 1);
    assert(req->req_wifi_set_config->cfg->u_case == WIFI_CONFIG__U_AP);
    const WifiApConfig *a = req->req_wifi_set_config->cfg->ap;
    assert(a->ssid_len == strlen(test_ssid));
    assert(a->ssid.len == strlen(test_ssid));
    assert(!memcmp(a->ssid.data, test_ssid, a->ssid.len));
    assert(a->password.len == strlen(test_password));
    assert(!memcmp(a->password.data, test_password, a->password.len));
    assert(a->authmode == 3 && a->pairwise_cipher == 4);
    assert(a->max_connection == 2 && a->ssid_hidden == 0);
    assert(a->channel == 1 && a->beacon_interval == 100);
    assert(a->pmf_cfg && a->pmf_cfg->capable && !a->pmf_cfg->required);
    resp.payload_case = RPC__PAYLOAD_RESP_WIFI_SET_CONFIG;
    resp.resp_wifi_set_config = &set;
    set.resp = fault == 2 ? 123 : 0;
  } else {
    assert(req->msg_id == RPC_ID__Req_WifiGetConfig);
    assert(req->payload_case == RPC__PAYLOAD_REQ_WIFI_GET_CONFIG);
    assert(req->req_wifi_get_config->iface == 1);
    resp.payload_case = RPC__PAYLOAD_RESP_WIFI_GET_CONFIG;
    resp.resp_wifi_get_config = &get;
    get.resp = fault == 2 ? 123 : 0;
    get.iface = 1;
    get.cfg = fault == 4 ? NULL : &cfg;
    cfg.u_case = WIFI_CONFIG__U_AP;
    cfg.ap = &ap;
    memcpy(ssid, test_ssid, strlen(test_ssid));
    memcpy(password, test_password, strlen(test_password));
    ap.ssid.data = ssid; ap.ssid.len = sizeof(ssid);
    ap.ssid_len = strlen(test_ssid);
    ap.password.data = password; ap.password.len = sizeof(password);
    ap.authmode = 3; ap.pairwise_cipher = 4; ap.max_connection = 2;
    ap.pmf_cfg = &pmf; pmf.capable = true;
    if (fault == 5) ap.authmode = 0;
    if (fault == 6) ap.pairwise_cipher = 2;
    if (fault == 7) ssid[0] ^= 1;
    if (fault == 8) password[0] ^= 1;
    if (fault == 9) ap.password.len = 1;
    if (fault == 10) ap.ssid_len = 33;
    if (fault == 11) pmf.capable = false;
    if (fault == 12) get.iface = 0;
  }
  if (fault == 3) resp.payload_case = RPC__PAYLOAD__NOT_SET;
  if (fault == 13) resp.msg_type = RPC_TYPE__Event;
  if (fault == 14) resp.msg_id++;
  size_t n = rpc__pack(&resp, wire);
  return rpc__unpack(NULL, n, wire);
}

#include "c6_softap_rpc.inc"

int main(void)
{
  int mode = 0;
  assert(!esp_hosted_rpc_wifi_stop());
  assert(!esp_hosted_rpc_wifi_get_mode(&mode) && mode == 3);
  assert(!esp_hosted_rpc_softap_configure(test_ssid, test_password));
  assert(!esp_hosted_rpc_softap_verify(test_ssid, test_password));
  unsigned old = calls;
  assert(esp_hosted_rpc_softap_configure(NULL, test_password) == -EINVAL);
  assert(esp_hosted_rpc_softap_configure("", test_password) == -EINVAL);
  assert(esp_hosted_rpc_softap_configure(test_ssid, "short") == -EINVAL);
  assert(esp_hosted_rpc_softap_configure(test_ssid, "bad\npassword") == -EINVAL);
  assert(esp_hosted_rpc_softap_verify(test_ssid, NULL) == -EINVAL);
  assert(esp_hosted_rpc_wifi_get_mode(NULL) == -EINVAL);
  assert(calls == old);
  for (fault = 1; fault <= 3; fault++) {
    assert(esp_hosted_rpc_wifi_stop() < 0);
    assert(esp_hosted_rpc_wifi_get_mode(&mode) < 0);
    assert(esp_hosted_rpc_softap_configure(test_ssid, test_password) < 0);
  }
  for (fault = 1; fault <= 14; fault++)
    assert(esp_hosted_rpc_softap_verify(test_ssid, test_password) < 0);
  puts("C6_SOFTAP_RPC_OK: real protobuf, strict AP readback, redaction label, malformed/timeout/error rejection");
  return 0;
}
