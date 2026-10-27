/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
static pthread_mutex_t g_rpc_transaction_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_c6_ota_attempted, pending;
bool c6_wifi_trial_pending(void) { return pending; }
#include "rpc_mailbox.h"
static struct c6_rpc_mailbox g_rpc = C6_RPC_MAILBOX_INITIALIZER;
#include "esp_hosted_rpc.pb-c.h"
#include "c6_softap.h"
#define RPC_WIFI_IF_AP 1
static unsigned fault, calls, client_fault;
static int protocol = 0x47, bandwidth = 1;
static uint32_t sta_mode = 6;
static int storage, storage_fault;
static unsigned phy_mutations, storage_calls;
static Rpc *peer(const Rpc *req, const char *label)
{
  Rpc resp = RPC__INIT;
  RpcRespWifiSetProtocol sp = RPC__RESP__WIFI_SET_PROTOCOL__INIT;
  RpcRespWifiGetProtocol gp = RPC__RESP__WIFI_GET_PROTOCOL__INIT;
  RpcRespWifiSetBandwidth sb = RPC__RESP__WIFI_SET_BANDWIDTH__INIT;
  RpcRespWifiGetBandwidth gb = RPC__RESP__WIFI_GET_BANDWIDTH__INIT;
  RpcRespWifiSetStorage storage_reply = RPC__RESP__WIFI_SET_STORAGE__INIT;
  RpcRespWifiApGetStaList clients = RPC__RESP__WIFI_AP_GET_STA_LIST__INIT;
  RpcRespWifiStaGetNegotiatedPhymode sta = RPC__RESP__WIFI_STA_GET_NEGOTIATED_PHYMODE__INIT;
  WifiStaList list = WIFI_STA_LIST__INIT;
  WifiStaInfo entries[15];
  WifiStaInfo *pointers[15];
  uint8_t mac[15][6] = {{0}};
  uint8_t wire[1024];
  (void)label; calls++;
  assert(req->msg_type == RPC_TYPE__Req && rpc__get_packed_size(req) < 128);
  if (fault == 1) return NULL;
  resp.msg_type = RPC_TYPE__Resp; resp.msg_id = req->msg_id + 256;
  resp.uid = req->uid;
  int error = fault == 2 ? 123 : 0;
  switch (req->msg_id) {
  case RPC_ID__Req_WifiSetStorage:
    assert(pthread_mutex_trylock(&g_rpc_transaction_lock) == EBUSY);
    storage_calls++;
    storage = req->req_wifi_set_storage->storage;
    assert(storage == 0 || storage == 1);
    if ((storage_fault == 1 && storage == 1) || (storage_fault == 2 && storage == 0)) return NULL;
    storage_reply.resp = error;
    resp.payload_case = RPC__PAYLOAD_RESP_WIFI_SET_STORAGE;
    resp.resp_wifi_set_storage = &storage_reply; break;
  case RPC_ID__Req_WifiSetProtocol:
    assert(storage == 1); phy_mutations++;
    assert(req->msg_id == 297 && req->payload_case == RPC__PAYLOAD_REQ_WIFI_SET_PROTOCOL);
    assert(req->req_wifi_set_protocol->ifx == 1 && req->req_wifi_set_protocol->protocol_bitmap == 0x47);
    sp.resp = error; resp.payload_case = RPC__PAYLOAD_RESP_WIFI_SET_PROTOCOL; resp.resp_wifi_set_protocol = &sp; break;
  case RPC_ID__Req_WifiGetProtocol:
    assert(req->msg_id == 298 && req->req_wifi_get_protocol->ifx == 1);
    gp.resp = error; gp.protocol_bitmap = protocol;
    resp.payload_case = RPC__PAYLOAD_RESP_WIFI_GET_PROTOCOL; resp.resp_wifi_get_protocol = &gp; break;
  case RPC_ID__Req_WifiSetBandwidth:
    assert(storage == 1); phy_mutations++;
    assert(req->msg_id == 299 && req->req_wifi_set_bandwidth->ifx == 1 && req->req_wifi_set_bandwidth->bw == 1);
    sb.resp = error; resp.payload_case = RPC__PAYLOAD_RESP_WIFI_SET_BANDWIDTH; resp.resp_wifi_set_bandwidth = &sb; break;
  case RPC_ID__Req_WifiGetBandwidth:
    assert(req->msg_id == 300 && req->req_wifi_get_bandwidth->ifx == 1);
    gb.resp = error; gb.bw = bandwidth;
    resp.payload_case = RPC__PAYLOAD_RESP_WIFI_GET_BANDWIDTH; resp.resp_wifi_get_bandwidth = &gb; break;
  case RPC_ID__Req_WifiApGetStaList:
    assert(req->msg_id == 311);
    clients.resp = error; clients.sta_list = &list;
    list.n_sta = 15; list.num = 2; list.sta = pointers;
    for (unsigned i = 0; i < 15; i++) {
      wifi_sta_info__init(&entries[i]); pointers[i] = &entries[i];
      mac[i][0] = 2; mac[i][5] = i + 1;
      entries[i].mac.data = mac[i]; entries[i].mac.len = 6;
      entries[i].rssi = -60; entries[i].bitmask = i == 0 ? 0x10 : 0x07;
    }
    if (client_fault == 1) clients.sta_list = NULL;
    if (client_fault == 2) list.num = -1;
    if (client_fault == 3) list.num = 3;
    if (client_fault == 4) list.n_sta = 1;
    if (client_fault == 5) entries[0].mac.len = 5;
    if (client_fault == 6) mac[0][0] = 3;
    if (client_fault == 7) memset(mac[0], 0, 6);
    if (client_fault == 8) entries[0].rssi = -128;
    if (client_fault == 9) entries[0].rssi = 1;
    if (client_fault == 10) memcpy(mac[1], mac[0], 6);
    if (client_fault == 11) list.num = 0;
    resp.payload_case = RPC__PAYLOAD_RESP_WIFI_AP_GET_STA_LIST; resp.resp_wifi_ap_get_sta_list = &clients; break;
  case RPC_ID__Req_WifiStaGetNegotiatedPhymode:
    assert(req->msg_id == 339);
    sta.resp = error; sta.phymode = sta_mode;
    resp.payload_case = RPC__PAYLOAD_RESP_WIFI_STA_GET_NEGOTIATED_PHYMODE; resp.resp_wifi_sta_get_negotiated_phymode = &sta; break;
  default: assert(!"unexpected RPC");
  }
  if (fault == 3) resp.msg_type = RPC_TYPE__Event;
  if (fault == 4) resp.msg_id++;
  if (fault == 5) resp.payload_case = RPC__PAYLOAD__NOT_SET;
  size_t size = rpc__get_packed_size(&resp);
  assert(size <= sizeof(wire));
  assert(rpc__pack(&resp, wire) == size);
  return rpc__unpack(NULL, size, wire);
}
#define RPC_TLV_MAX 512
#define RPC_RESP_WAIT_MS 2
#define RPC_POLL_INTERVAL_MS 1
#define ESP_HOSTED_IF_SERIAL 3
static const char *wire_label;
static void rpc_rx_cb(void) {}
static int rpc_tlv_compose(uint8_t *dst, size_t cap, const uint8_t *src, uint16_t n)
{ assert(n <= cap); memcpy(dst, src, n); return n; }
static int esp_hosted_register(int type, void (*cb)(void), void *arg)
{ assert(type == 3 && cb == rpc_rx_cb && !arg); return 0; }
static int esp_hosted_send(int type, int num, const uint8_t *wire, uint16_t n)
{
  assert(type == 3 && num == 0 && c6_rpc_waiting(&g_rpc));
  Rpc *req = rpc__unpack(NULL, n, wire); assert(req && req->uid);
  Rpc *resp = peer(req, wire_label);
  if (resp) {
    assert(!c6_rpc_accept(&g_rpc, req->uid + 1, resp));
    assert(c6_rpc_accept(&g_rpc, req->uid, resp));
  }
  rpc__free_unpacked(req, NULL); return 0;
}
static int esp_hosted_poll(void) { return 0; }
static void host_usleep(unsigned duration) { (void)duration; }
#define usleep host_usleep
#include "rpc-locked-under-test.inc"
#undef usleep
static Rpc *rpc_transact(const Rpc *request, const char *label)
{
  Rpc owned = *request; owned.uid = c6_rpc_next_uid(&g_rpc);
  return rpc_transact_locked(&owned, label);
}
static bool c6_softap_response(const Rpc *response, const Rpc *request)
{ return response->msg_type == RPC_TYPE__Resp && response->msg_id == request->msg_id + 256; }
#ifdef HOST_BAD_PHY
#  include "bad-phy-rpc.inc"
#else
#  include "c6_wifi_phy_rpc.inc"
#endif
int main(void)
{
  uint8_t value = 99, count = 99, he = 99;
  assert(!esp_hosted_rpc_phy_set_protocol(1, 0x47));
  assert(!esp_hosted_rpc_phy_set_bandwidth(1, 1));
  assert(!esp_hosted_rpc_phy_get_protocol(1, &value) && value == 0x47);
  assert(!esp_hosted_rpc_phy_get_bandwidth(1, &value) && value == 1);
  assert(!esp_hosted_rpc_phy_clients(&count, &he) && count == 2 && he == 1);
  assert(!esp_hosted_rpc_phy_sta(&value) && value == 6);
  for (fault = 1; fault <= 5; fault++) {
    value = count = he = 99;
    assert(esp_hosted_rpc_phy_set_protocol(1, 0x47) < 0);
    assert(esp_hosted_rpc_phy_set_bandwidth(1, 1) < 0);
    assert(esp_hosted_rpc_phy_get_protocol(1, &value) < 0 && value == 99);
    assert(esp_hosted_rpc_phy_get_bandwidth(1, &value) < 0 && value == 99);
    assert(esp_hosted_rpc_phy_clients(&count, &he) < 0 && count == 99 && he == 99);
    assert(esp_hosted_rpc_phy_sta(&value) < 0 && value == 99);
  }
  fault = 0;
  assert(storage == 0);
  unsigned mutated = phy_mutations;
  storage_fault = 1;
  assert(esp_hosted_rpc_phy_set_protocol(1, 0x47) == -ETIMEDOUT);
  assert(storage == 0 && phy_mutations == mutated);
  storage_fault = 2;
  assert(esp_hosted_rpc_phy_set_protocol(1, 0x47) == -EUCLEAN);
  assert(phy_mutations == mutated + 1);
  storage_fault = 0;
  assert(!esp_hosted_rpc_phy_storage_restore() && storage == 0);
  unsigned storage_before = storage_calls;
  pending = true;
  assert(esp_hosted_rpc_phy_set_protocol(1, 0x47) == -EBUSY);
  assert(esp_hosted_rpc_phy_storage_restore() == -EBUSY);
  pending = false; g_c6_ota_attempted = true;
  assert(esp_hosted_rpc_phy_set_bandwidth(1, 1) == -EBUSY);
  assert(storage_calls == storage_before);
  g_c6_ota_attempted = false;
  int bad_protocols[] = {0, -1, 0x10, 0x80, 0x100};
  for (unsigned i = 0; i < sizeof(bad_protocols)/sizeof(*bad_protocols); i++) {
    protocol = bad_protocols[i]; assert(esp_hosted_rpc_phy_get_protocol(1, &value) == -EPROTO);
  }
  int bad_bandwidths[] = {0, -1, 3, 256};
  for (unsigned i = 0; i < sizeof(bad_bandwidths)/sizeof(*bad_bandwidths); i++) {
    bandwidth = bad_bandwidths[i]; assert(esp_hosted_rpc_phy_get_bandwidth(1, &value) == -EPROTO);
  }
  for (client_fault = 1; client_fault <= 10; client_fault++)
    assert(esp_hosted_rpc_phy_clients(&count, &he) == -EPROTO);
  assert(!esp_hosted_rpc_phy_clients(&count, &he) && !count && !he);
  for (sta_mode = 0; sta_mode <= 8; sta_mode++) {
    int ret = esp_hosted_rpc_phy_sta(&value);
    if (sta_mode == 1 || sta_mode == 2 || sta_mode == 4 || sta_mode == 5 || sta_mode == 6)
      assert(!ret && value == sta_mode);
    else assert(ret == -EPROTO);
  }
  unsigned old = calls;
  assert(esp_hosted_rpc_phy_get_protocol(2, &value) == -EINVAL);
  assert(esp_hosted_rpc_phy_get_protocol(1, NULL) == -EINVAL);
  assert(esp_hosted_rpc_phy_set_protocol(1, 0x10) == -EINVAL);
  assert(esp_hosted_rpc_phy_set_bandwidth(1, 0) == -EINVAL);
  assert(esp_hosted_rpc_phy_clients(NULL, &he) == -EINVAL);
  assert(esp_hosted_rpc_phy_sta(NULL) == -EINVAL && calls == old);
  puts("C6_WIFI_PHY_RPC_OK: actual protobuf ABI, proto3 zero replies, malformed envelopes/ranges, full 15-slot list, HE bit and IDF5.5 enum");
  return 0;
}
