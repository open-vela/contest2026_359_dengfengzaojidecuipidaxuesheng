/* SPDX-License-Identifier: Apache-2.0 */

#include <stdlib.h>
#include <stdatomic.h>

#include "c6_wifi_trial.h"

/* Included after the shared RPC transaction owner. Keep an owned old config,
 * including optional STA fields, until commit or rollback is acknowledged.
 * The companion storage selector is global, so unrelated RPCs must not
 * interleave between these operations or during the DHCP trial window.
 */

#define C6_WIFI_STORAGE_FLASH 0
#define C6_WIFI_STORAGE_RAM   1
#define C6_WIFI_NOT_CONNECTED 0x300f

struct c6_wifi_trial_s
{
  Rpc *saved;
  WifiStaConfig sta;
  WifiPmfConfig pmf;
  WifiScanThreshold threshold;
  WifiConfig cfg;
  uint8_t ssid[32];
  uint8_t password[64];
  bool connected;
  bool commit_sent;
};

static struct c6_wifi_trial_s *g_c6_wifi_trial;
static atomic_bool g_c6_wifi_trial_active;

static void c6_wifi_trial_dispose(struct c6_wifi_trial_s *trial)
{
  if (trial == NULL)
    {
      return;
    }

  if (trial->saved != NULL)
    {
      WifiStaConfig *sta = trial->saved->resp_wifi_get_config->cfg->sta;

      if (sta->password.data != NULL)
        {
          explicit_bzero(sta->password.data, sta->password.len);
        }

      if (sta->sae_h2e_identifier.data != NULL)
        {
          explicit_bzero(sta->sae_h2e_identifier.data,
                         sta->sae_h2e_identifier.len);
        }

      rpc__free_unpacked(trial->saved, NULL);
    }

  explicit_bzero(trial, sizeof(*trial));
  free(trial);
}

static int c6_wifi_trial_exchange(Rpc *request, Rpc **response)
{
  request->msg_type = RPC_TYPE__Req;
  request->uid = c6_rpc_next_uid(&g_rpc);
  *response = NULL;
  if (request->uid == 0)
    {
      return -EOVERFLOW;
    }

  /* This existing label suppresses credential hexdumps in the owner. */

  *response = rpc_transact_locked(request, "WifiSetConfig");
  if (*response == NULL)
    {
      return -ETIMEDOUT;
    }

  if ((*response)->msg_type != RPC_TYPE__Resp ||
      (*response)->msg_id != request->msg_id + 256 ||
      (*response)->uid != request->uid)
    {
      rpc__free_unpacked(*response, NULL);
      *response = NULL;
      return -EPROTO;
    }

  return 0;
}

static int c6_wifi_trial_storage(int storage)
{
  RpcReqWifiSetStorage payload = RPC__REQ__WIFI_SET_STORAGE__INIT;
  Rpc request = RPC__INIT;
  Rpc *response;
  int ret;

  payload.storage = storage;
  request.msg_id = RPC_ID__Req_WifiSetStorage;
  request.payload_case = RPC__PAYLOAD_REQ_WIFI_SET_STORAGE;
  request.req_wifi_set_storage = &payload;
  ret = c6_wifi_trial_exchange(&request, &response);
  if (ret < 0)
    {
      return ret;
    }

  ret = -EPROTO;
  if (response->payload_case == RPC__PAYLOAD_RESP_WIFI_SET_STORAGE &&
      response->resp_wifi_set_storage != NULL)
    {
      ret = response->resp_wifi_set_storage->resp == 0 ? 0 : -EIO;
    }

  rpc__free_unpacked(response, NULL);
  return ret;
}

static int c6_wifi_trial_config(WifiConfig *cfg)
{
  RpcReqWifiSetConfig payload = RPC__REQ__WIFI_SET_CONFIG__INIT;
  Rpc request = RPC__INIT;
  Rpc *response;
  int ret;

  payload.iface = 0;
  payload.cfg = cfg;
  request.msg_id = RPC_ID__Req_WifiSetConfig;
  request.payload_case = RPC__PAYLOAD_REQ_WIFI_SET_CONFIG;
  request.req_wifi_set_config = &payload;
  ret = c6_wifi_trial_exchange(&request, &response);
  if (ret < 0)
    {
      return ret;
    }

  ret = -EPROTO;
  if (response->payload_case == RPC__PAYLOAD_RESP_WIFI_SET_CONFIG &&
      response->resp_wifi_set_config != NULL)
    {
      ret = response->resp_wifi_set_config->resp == 0 ? 0 : -EIO;
    }

  rpc__free_unpacked(response, NULL);
  return ret;
}

static int c6_wifi_trial_link(bool connect)
{
  RpcReqWifiConnect start = RPC__REQ__WIFI_CONNECT__INIT;
  RpcReqWifiDisconnect stop = RPC__REQ__WIFI_DISCONNECT__INIT;
  Rpc request = RPC__INIT;
  Rpc *response;
  int ret;

  request.msg_id = connect ? RPC_ID__Req_WifiConnect :
                            RPC_ID__Req_WifiDisconnect;
  request.payload_case = connect ? RPC__PAYLOAD_REQ_WIFI_CONNECT :
                                  RPC__PAYLOAD_REQ_WIFI_DISCONNECT;
  if (connect)
    {
      request.req_wifi_connect = &start;
    }
  else
    {
      request.req_wifi_disconnect = &stop;
    }

  ret = c6_wifi_trial_exchange(&request, &response);
  if (ret < 0)
    {
      return ret;
    }

  ret = -EPROTO;
  if (connect && response->payload_case == RPC__PAYLOAD_RESP_WIFI_CONNECT &&
      response->resp_wifi_connect != NULL)
    {
      ret = response->resp_wifi_connect->resp == 0 ? 0 : -EIO;
    }
  else if (!connect &&
           response->payload_case == RPC__PAYLOAD_RESP_WIFI_DISCONNECT &&
           response->resp_wifi_disconnect != NULL)
    {
      int status = response->resp_wifi_disconnect->resp;

      ret = status == 0 || status == C6_WIFI_NOT_CONNECTED ? 0 : -EIO;
    }

  rpc__free_unpacked(response, NULL);
  return ret;
}

static int c6_wifi_trial_snapshot(struct c6_wifi_trial_s *trial)
{
  RpcReqWifiGetConfig payload = RPC__REQ__WIFI_GET_CONFIG__INIT;
  Rpc request = RPC__INIT;
  Rpc *response;
  int ret;

  payload.iface = 0;
  request.msg_id = RPC_ID__Req_WifiGetConfig;
  request.payload_case = RPC__PAYLOAD_REQ_WIFI_GET_CONFIG;
  request.req_wifi_get_config = &payload;
  ret = c6_wifi_trial_exchange(&request, &response);
  if (ret < 0)
    {
      return ret;
    }

  ret = -EPROTO;
  if (response->payload_case == RPC__PAYLOAD_RESP_WIFI_GET_CONFIG &&
      response->resp_wifi_get_config != NULL)
    {
      RpcRespWifiGetConfig *value = response->resp_wifi_get_config;

      if (value->resp != 0)
        {
          ret = -EIO;
        }
      else if (value->iface == 0 && value->cfg != NULL &&
               value->cfg->u_case == WIFI_CONFIG__U_STA &&
               value->cfg->sta != NULL &&
               value->cfg->sta->ssid.len <= 32 &&
               value->cfg->sta->password.len <= 64)
        {
          trial->saved = response;
          return 0;
        }
    }

  rpc__free_unpacked(response, NULL);
  return ret;
}

int c6_wifi_trial_begin(const char *ssid, const char *password)
{
  struct c6_wifi_trial_s *trial;
  size_t ssidlen;
  size_t passlen;
  int ret;

  if (ssid == NULL || password == NULL)
    {
      return -EINVAL;
    }

  ssidlen = strnlen(ssid, 33);
  passlen = strnlen(password, 65);
  if (ssidlen == 0 || ssidlen > 32 || passlen > 64 ||
      (passlen != 0 && passlen < 8))
    {
      return -EINVAL;
    }

  trial = calloc(1, sizeof(*trial));
  if (trial == NULL)
    {
      return -ENOMEM;
    }

  trial->sta = (WifiStaConfig)WIFI_STA_CONFIG__INIT;
  trial->pmf = (WifiPmfConfig)WIFI_PMF_CONFIG__INIT;
  trial->threshold = (WifiScanThreshold)WIFI_SCAN_THRESHOLD__INIT;
  trial->cfg = (WifiConfig)WIFI_CONFIG__INIT;
  memcpy(trial->ssid, ssid, ssidlen);
  memcpy(trial->password, password, passlen);
  trial->sta.ssid.data = trial->ssid;
  trial->sta.ssid.len = ssidlen < 32 ? ssidlen + 1 : ssidlen;
  trial->sta.password.data = trial->password;
  trial->sta.password.len = passlen < 64 ? passlen + 1 : passlen;
  trial->sta.threshold = &trial->threshold;
  trial->sta.pmf_cfg = &trial->pmf;
  trial->cfg.u_case = WIFI_CONFIG__U_STA;
  trial->cfg.sta = &trial->sta;

  ret = pthread_mutex_lock(&g_rpc_transaction_lock);
  if (ret != 0)
    {
      c6_wifi_trial_dispose(trial);
      return -ret;
    }

  if (g_c6_ota_attempted)
    {
      ret = -ESHUTDOWN;
    }
  else if (g_c6_wifi_trial_active)
    {
      ret = -EBUSY;
    }
  else
    {
      ret = c6_wifi_trial_snapshot(trial);
      if (ret == 0)
        {
          g_c6_wifi_trial = trial;
          g_c6_wifi_trial_active = true;
          ret = c6_wifi_trial_storage(C6_WIFI_STORAGE_RAM);
          if (ret == 0) ret = c6_wifi_trial_link(false);
          if (ret == 0) ret = c6_wifi_trial_config(&trial->cfg);
          if (ret == 0) ret = c6_wifi_trial_link(true);
          trial->connected = ret == 0;
          trial = NULL; /* Retain the old config even on an uncertain reply. */
        }
    }

  pthread_mutex_unlock(&g_rpc_transaction_lock);
  c6_wifi_trial_dispose(trial);
  return ret;
}

int c6_wifi_trial_finish(bool commit)
{
  struct c6_wifi_trial_s *trial;
  int ret = pthread_mutex_lock(&g_rpc_transaction_lock);

  if (ret != 0)
    {
      return -ret;
    }

  trial = g_c6_wifi_trial;
  if (g_c6_ota_attempted)
    {
      ret = -ESHUTDOWN;
    }
  else if (trial == NULL)
    {
      ret = -ENOENT;
    }
  else if (commit && !trial->connected)
    {
      ret = -EINVAL;
    }
  else if (commit)
    {
      ret = c6_wifi_trial_storage(C6_WIFI_STORAGE_FLASH);
      if (ret == 0)
        {
          trial->commit_sent = true;
          ret = c6_wifi_trial_config(&trial->cfg);
        }
    }
  else
    {
      ret = c6_wifi_trial_storage(C6_WIFI_STORAGE_RAM);
      if (ret == 0) ret = c6_wifi_trial_link(false);
      if (ret == 0)
        ret = c6_wifi_trial_config(trial->saved->resp_wifi_get_config->cfg);
      if (ret == 0) ret = c6_wifi_trial_storage(C6_WIFI_STORAGE_FLASH);
      if (ret == 0 && trial->commit_sent)
        ret = c6_wifi_trial_config(trial->saved->resp_wifi_get_config->cfg);
    }

  if (ret == 0)
    {
      g_c6_wifi_trial = NULL;
      g_c6_wifi_trial_active = false;
      c6_wifi_trial_dispose(trial);
    }

  pthread_mutex_unlock(&g_rpc_transaction_lock);
  return ret;
}

bool c6_wifi_trial_pending(void)
{
  return atomic_load(&g_c6_wifi_trial_active);
}
