/****************************************************************************
 * apps/system/c6probe/c6_softap_rpc.inc
 *
 * Bounded AP configuration on the existing serialized Hosted RPC owner.
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "c6_softap.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool c6_softap_response(FAR const Rpc *response,
                               FAR const Rpc *request)
{
  /* The transaction owner already matches its generated UID before transfer
   * of response ownership. Check the remaining envelope identity here.
   */

  return response->msg_type == RPC_TYPE__Resp &&
         response->msg_id == request->msg_id + 256;
}

static int c6_softap_validate(FAR const char *ssid,
                            FAR const char *password)
{
  size_t ssidlen;
  size_t passlen;

  if (ssid == NULL || password == NULL)
    {
      return -EINVAL;
    }

  ssidlen = strnlen(ssid, C6_SOFTAP_SSID_MAX + 1);
  passlen = strnlen(password, C6_SOFTAP_PASSWORD_MAX + 1);
  if (ssidlen == 0 || ssidlen > C6_SOFTAP_SSID_MAX ||
      passlen < C6_SOFTAP_PASSWORD_MIN || passlen > C6_SOFTAP_PASSWORD_MAX)
    {
      return -EINVAL;
    }

  for (size_t i = 0; i < passlen; i++)
    {
      if ((unsigned char)password[i] < 32 ||
          (unsigned char)password[i] > 126)
        {
          return -EINVAL;
        }
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int esp_hosted_rpc_wifi_stop(void)
{
  RpcReqWifiStop payload = RPC__REQ__WIFI_STOP__INIT;
  Rpc req = RPC__INIT;
  FAR Rpc *resp;
  int ret = -EPROTO;

  req.msg_type = RPC_TYPE__Req;
  req.msg_id = RPC_ID__Req_WifiStop;
  req.payload_case = RPC__PAYLOAD_REQ_WIFI_STOP;
  req.req_wifi_stop = &payload;
  resp = rpc_transact(&req, "WifiStop");
  if (resp == NULL)
    {
      return -ETIMEDOUT;
    }

  if (c6_softap_response(resp, &req) &&
      resp->payload_case == RPC__PAYLOAD_RESP_WIFI_STOP &&
      resp->resp_wifi_stop != NULL)
    {
      ret = resp->resp_wifi_stop->resp == 0 ? 0 : -EIO;
    }

  rpc__free_unpacked(resp, NULL);
  return ret;
}

#include "c6_wifi_phy_rpc.inc"

int esp_hosted_rpc_wifi_get_mode(FAR int *mode)
{
  RpcReqGetMode payload = RPC__REQ__GET_MODE__INIT;
  Rpc req = RPC__INIT;
  FAR Rpc *resp;
  int ret = -EPROTO;

  if (mode == NULL)
    {
      return -EINVAL;
    }

  req.msg_type = RPC_TYPE__Req;
  req.msg_id = RPC_ID__Req_GetWifiMode;
  req.payload_case = RPC__PAYLOAD_REQ_GET_WIFI_MODE;
  req.req_get_wifi_mode = &payload;
  resp = rpc_transact(&req, "GetWifiMode");
  if (resp == NULL)
    {
      return -ETIMEDOUT;
    }

  if (c6_softap_response(resp, &req) &&
      resp->payload_case == RPC__PAYLOAD_RESP_GET_WIFI_MODE &&
      resp->resp_get_wifi_mode != NULL)
    {
      ret = resp->resp_get_wifi_mode->resp == 0 ? 0 : -EIO;
      if (ret == 0)
        {
          *mode = resp->resp_get_wifi_mode->mode;
        }
    }

  rpc__free_unpacked(resp, NULL);
  return ret;
}

int esp_hosted_rpc_softap_configure(FAR const char *ssid,
                                  FAR const char *password)
{
  WifiApConfig ap = WIFI_AP_CONFIG__INIT;
  WifiPmfConfig pmf = WIFI_PMF_CONFIG__INIT;
  WifiConfig cfg = WIFI_CONFIG__INIT;
  RpcReqWifiSetConfig payload = RPC__REQ__WIFI_SET_CONFIG__INIT;
  Rpc req = RPC__INIT;
  FAR Rpc *resp;
  int ret = c6_softap_validate(ssid, password);

  if (ret < 0)
    {
      return ret;
    }

  ap.ssid.data = (FAR uint8_t *)ssid;
  ap.ssid.len = strlen(ssid);
  ap.ssid_len = ap.ssid.len;
  ap.password.data = (FAR uint8_t *)password;
  ap.password.len = strlen(password);
  ap.channel = 1;             /* STA channel takes priority in AP+STA mode. */
  ap.authmode = 3;            /* WIFI_AUTH_WPA2_PSK */
  ap.pairwise_cipher = 4;     /* WIFI_CIPHER_TYPE_CCMP */
  ap.max_connection = 2;
  ap.beacon_interval = 100;
  ap.csa_count = 3;
  ap.dtim_period = 1;
  pmf.capable = true;
  ap.pmf_cfg = &pmf;
  cfg.u_case = WIFI_CONFIG__U_AP;
  cfg.ap = &ap;
  payload.iface = RPC_WIFI_IF_AP;
  payload.cfg = &cfg;
  req.msg_type = RPC_TYPE__Req;
  req.msg_id = RPC_ID__Req_WifiSetConfig;
  req.payload_case = RPC__PAYLOAD_REQ_WIFI_SET_CONFIG;
  req.req_wifi_set_config = &payload;

  /* This existing label also suppresses the transaction's credential dump. */

  resp = rpc_transact(&req, "WifiSetConfig");
  if (resp == NULL)
    {
      return -ETIMEDOUT;
    }

  ret = -EPROTO;
  if (c6_softap_response(resp, &req) &&
      resp->payload_case == RPC__PAYLOAD_RESP_WIFI_SET_CONFIG &&
      resp->resp_wifi_set_config != NULL)
    {
      ret = resp->resp_wifi_set_config->resp == 0 ? 0 : -EIO;
    }

  rpc__free_unpacked(resp, NULL);
  return ret;
}

int esp_hosted_rpc_softap_verify(FAR const char *ssid,
                               FAR const char *password)
{
  RpcReqWifiGetConfig payload = RPC__REQ__WIFI_GET_CONFIG__INIT;
  Rpc req = RPC__INIT;
  FAR RpcRespWifiGetConfig *value;
  FAR WifiApConfig *ap;
  FAR Rpc *resp;
  size_t ssidlen;
  size_t passlen;
  int ret = c6_softap_validate(ssid, password);

  if (ret < 0)
    {
      return ret;
    }

  ssidlen = strlen(ssid);
  passlen = strlen(password);
  payload.iface = RPC_WIFI_IF_AP;
  req.msg_type = RPC_TYPE__Req;
  req.msg_id = RPC_ID__Req_WifiGetConfig;
  req.payload_case = RPC__PAYLOAD_REQ_WIFI_GET_CONFIG;
  req.req_wifi_get_config = &payload;
  resp = rpc_transact(&req, "WifiGetAPConfig");
  if (resp == NULL)
    {
      return -ETIMEDOUT;
    }

  ret = -EPROTO;
  if (!c6_softap_response(resp, &req) ||
      resp->payload_case != RPC__PAYLOAD_RESP_WIFI_GET_CONFIG ||
      resp->resp_wifi_get_config == NULL)
    {
      goto out;
    }

  value = resp->resp_wifi_get_config;
  if (value->resp != 0)
    {
      ret = -EIO;
      goto out;
    }

  if (value->iface != RPC_WIFI_IF_AP || value->cfg == NULL ||
      value->cfg->u_case != WIFI_CONFIG__U_AP || value->cfg->ap == NULL)
    {
      goto out;
    }

  ap = value->cfg->ap;
  ret = -EBADMSG;
  if (ap->ssid.len >= ssidlen && ap->ssid.data != NULL &&
      ap->ssid_len == ssidlen && !memcmp(ap->ssid.data, ssid, ssidlen) &&
      ap->password.len >= passlen && ap->password.data != NULL &&
      !memcmp(ap->password.data, password, passlen) &&
      (ap->password.len == passlen || ap->password.data[passlen] == 0) &&
      ap->authmode == 3 && ap->pairwise_cipher == 4 &&
      ap->ssid_hidden == 0 && ap->max_connection == 2 &&
      ap->pmf_cfg != NULL && ap->pmf_cfg->capable)
    {
      ret = 0;
    }

out:
  /* Never log the decoded configuration. Clear the received password before
   * releasing protobuf allocations, including malformed/error replies.
   */

  if (resp->payload_case == RPC__PAYLOAD_RESP_WIFI_GET_CONFIG &&
      resp->resp_wifi_get_config != NULL &&
      resp->resp_wifi_get_config->cfg != NULL &&
      resp->resp_wifi_get_config->cfg->u_case == WIFI_CONFIG__U_AP &&
      resp->resp_wifi_get_config->cfg->ap != NULL)
    {
      ap = resp->resp_wifi_get_config->cfg->ap;
      if (ap->password.data != NULL)
        {
          FAR volatile uint8_t *clear = ap->password.data;
          for (size_t i = 0; i < ap->password.len; i++)
            {
              clear[i] = 0;
            }
        }
    }

  rpc__free_unpacked(resp, NULL);
  return ret;
}
