/****************************************************************************
 * apps/system/c6probe/c6_wifi_phy_rpc.inc
 *
 * Fixed-version PHY RPCs on the existing serialized transaction owner.
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "c6_wifi_phy.h"
#include "c6_wifi_trial.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* The storage selector is global on C6. Hold the existing transaction owner
 * across RAM selection, one PHY setter and FLASH restoration. No other
 * configuration request may observe the temporary selector.
 */

static FAR Rpc *c6_phy_exchange_locked(FAR const Rpc *req,
                                      FAR const char *label)
{
  Rpc owned = *req;
  owned.uid = c6_rpc_next_uid(&g_rpc);
  return owned.uid == 0 ? NULL : rpc_transact_locked(&owned, label);
}

static int c6_phy_storage_locked(int storage)
{
  RpcReqWifiSetStorage payload = RPC__REQ__WIFI_SET_STORAGE__INIT;
  Rpc req = RPC__INIT;
  FAR Rpc *resp;
  int ret = -EPROTO;
  payload.storage = storage;
  req.msg_type = RPC_TYPE__Req;
  req.msg_id = RPC_ID__Req_WifiSetStorage;
  req.payload_case = RPC__PAYLOAD_REQ_WIFI_SET_STORAGE;
  req.req_wifi_set_storage = &payload;
  resp = c6_phy_exchange_locked(&req, "WifiSetStorage");
  if (resp == NULL)
    {
      return -ETIMEDOUT;
    }

  if (c6_softap_response(resp, &req) &&
      resp->payload_case == RPC__PAYLOAD_RESP_WIFI_SET_STORAGE &&
      resp->resp_wifi_set_storage != NULL)
    {
      ret = resp->resp_wifi_set_storage->resp == 0 ? 0 : -EIO;
    }

  rpc__free_unpacked(resp, NULL);
  return ret;
}

int esp_hosted_rpc_phy_storage_restore(void)
{
  int ret = pthread_mutex_lock(&g_rpc_transaction_lock);
  if (ret != 0)
    {
      return -ret;
    }

  ret = g_c6_ota_attempted || c6_wifi_trial_pending() ? -EBUSY :
        c6_phy_storage_locked(0);
  pthread_mutex_unlock(&g_rpc_transaction_lock);
  return ret;
}

static FAR Rpc *c6_phy_ram_transact(FAR const Rpc *req,
                                  FAR const char *label, FAR int *error)
{
  FAR Rpc *resp = NULL;
  int ret = pthread_mutex_lock(&g_rpc_transaction_lock);
  if (ret != 0)
    {
      *error = -ret;
      return NULL;
    }

  if (g_c6_ota_attempted || c6_wifi_trial_pending())
    {
      *error = -EBUSY;
    }
  else
    {
      *error = c6_phy_storage_locked(1);
      if (*error == 0)
        {
          resp = c6_phy_exchange_locked(req, label);
          if (resp == NULL)
            {
              *error = -ETIMEDOUT;
            }
        }

      /* Even a failed RAM reply may have changed the selector. */

      if (c6_phy_storage_locked(0) != 0)
        {
          *error = -EUCLEAN;
          if (resp != NULL)
            {
              rpc__free_unpacked(resp, NULL);
              resp = NULL;
            }
        }
    }

  pthread_mutex_unlock(&g_rpc_transaction_lock);
  return resp;
}

static bool c6_phy_protocol_valid(unsigned value)
{
  return value != 0 && (value & ~C6_WIFI_PROTOCOL_MASK) == 0;
}

static bool c6_phy_bandwidth_valid(unsigned value)
{
  return value == C6_WIFI_BW20 || value == C6_WIFI_BW40;
}

int esp_hosted_rpc_phy_get_protocol(int iface, FAR uint8_t *value)
{
  RpcReqWifiGetProtocol payload = RPC__REQ__WIFI_GET_PROTOCOL__INIT;
  Rpc req = RPC__INIT;
  FAR Rpc *resp;
  int ret = -EPROTO;

  if ((iface != 0 && iface != 1) || value == NULL)
    {
      return -EINVAL;
    }

  payload.ifx = iface;
  req.msg_type = RPC_TYPE__Req;
  req.msg_id = RPC_ID__Req_WifiGetProtocol;
  req.payload_case = RPC__PAYLOAD_REQ_WIFI_GET_PROTOCOL;
  req.req_wifi_get_protocol = &payload;
  resp = rpc_transact(&req, "WifiGetProtocol");
  if (resp == NULL)
    {
      return -ETIMEDOUT;
    }

  if (c6_softap_response(resp, &req) &&
      resp->payload_case == RPC__PAYLOAD_RESP_WIFI_GET_PROTOCOL &&
      resp->resp_wifi_get_protocol != NULL)
    {
      FAR RpcRespWifiGetProtocol *body = resp->resp_wifi_get_protocol;
      if (body->resp != 0)
        {
          ret = -EIO;
        }
      else if (c6_phy_protocol_valid((unsigned)body->protocol_bitmap))
        {
          *value = (uint8_t)body->protocol_bitmap;
          ret = 0;
        }
    }

  rpc__free_unpacked(resp, NULL);
  return ret;
}

int esp_hosted_rpc_phy_set_protocol(int iface, uint8_t value)
{
  RpcReqWifiSetProtocol payload = RPC__REQ__WIFI_SET_PROTOCOL__INIT;
  Rpc req = RPC__INIT;
  FAR Rpc *resp;
  int ret = -EPROTO;

  if ((iface != 0 && iface != 1) || !c6_phy_protocol_valid(value))
    {
      return -EINVAL;
    }

  payload.ifx = iface;
  payload.protocol_bitmap = value;
  req.msg_type = RPC_TYPE__Req;
  req.msg_id = RPC_ID__Req_WifiSetProtocol;
  req.payload_case = RPC__PAYLOAD_REQ_WIFI_SET_PROTOCOL;
  req.req_wifi_set_protocol = &payload;
  resp = c6_phy_ram_transact(&req, "WifiSetProtocol", &ret);
  if (resp == NULL)
    {
      return ret;
    }

  ret = -EPROTO;

  if (c6_softap_response(resp, &req) &&
      resp->payload_case == RPC__PAYLOAD_RESP_WIFI_SET_PROTOCOL &&
      resp->resp_wifi_set_protocol != NULL)
    {
      FAR RpcRespWifiSetProtocol *body = resp->resp_wifi_set_protocol;
      if (body->resp != 0)
        {
          ret = -EIO;
        }
      else
        {
          ret = 0;
        }
    }

  rpc__free_unpacked(resp, NULL);
  return ret;
}

int esp_hosted_rpc_phy_get_bandwidth(int iface, FAR uint8_t *value)
{
  RpcReqWifiGetBandwidth payload = RPC__REQ__WIFI_GET_BANDWIDTH__INIT;
  Rpc req = RPC__INIT;
  FAR Rpc *resp;
  int ret = -EPROTO;

  if ((iface != 0 && iface != 1) || value == NULL)
    {
      return -EINVAL;
    }

  payload.ifx = iface;
  req.msg_type = RPC_TYPE__Req;
  req.msg_id = RPC_ID__Req_WifiGetBandwidth;
  req.payload_case = RPC__PAYLOAD_REQ_WIFI_GET_BANDWIDTH;
  req.req_wifi_get_bandwidth = &payload;
  resp = rpc_transact(&req, "WifiGetBandwidth");
  if (resp == NULL)
    {
      return -ETIMEDOUT;
    }

  if (c6_softap_response(resp, &req) &&
      resp->payload_case == RPC__PAYLOAD_RESP_WIFI_GET_BANDWIDTH &&
      resp->resp_wifi_get_bandwidth != NULL)
    {
      FAR RpcRespWifiGetBandwidth *body = resp->resp_wifi_get_bandwidth;
      if (body->resp != 0)
        {
          ret = -EIO;
        }
      else if (c6_phy_bandwidth_valid((unsigned)body->bw))
        {
          *value = (uint8_t)body->bw;
          ret = 0;
        }
    }

  rpc__free_unpacked(resp, NULL);
  return ret;
}

int esp_hosted_rpc_phy_set_bandwidth(int iface, uint8_t value)
{
  RpcReqWifiSetBandwidth payload = RPC__REQ__WIFI_SET_BANDWIDTH__INIT;
  Rpc req = RPC__INIT;
  FAR Rpc *resp;
  int ret = -EPROTO;

  if ((iface != 0 && iface != 1) || !c6_phy_bandwidth_valid(value))
    {
      return -EINVAL;
    }

  payload.ifx = iface;
  payload.bw = value;
  req.msg_type = RPC_TYPE__Req;
  req.msg_id = RPC_ID__Req_WifiSetBandwidth;
  req.payload_case = RPC__PAYLOAD_REQ_WIFI_SET_BANDWIDTH;
  req.req_wifi_set_bandwidth = &payload;
  resp = c6_phy_ram_transact(&req, "WifiSetBandwidth", &ret);
  if (resp == NULL)
    {
      return ret;
    }

  ret = -EPROTO;

  if (c6_softap_response(resp, &req) &&
      resp->payload_case == RPC__PAYLOAD_RESP_WIFI_SET_BANDWIDTH &&
      resp->resp_wifi_set_bandwidth != NULL)
    {
      FAR RpcRespWifiSetBandwidth *body = resp->resp_wifi_set_bandwidth;
      if (body->resp != 0)
        {
          ret = -EIO;
        }
      else
        {
          ret = 0;
        }
    }

  rpc__free_unpacked(resp, NULL);
  return ret;
}

int esp_hosted_rpc_phy_clients(FAR uint8_t *count, FAR uint8_t *he_count)
{
  RpcReqWifiApGetStaList payload = RPC__REQ__WIFI_AP_GET_STA_LIST__INIT;
  Rpc req = RPC__INIT;
  FAR Rpc *resp;
  FAR WifiStaList *list;
  unsigned he = 0;
  int ret = -EPROTO;

  if (count == NULL || he_count == NULL)
    {
      return -EINVAL;
    }

  req.msg_type = RPC_TYPE__Req;
  req.msg_id = RPC_ID__Req_WifiApGetStaList;
  req.payload_case = RPC__PAYLOAD_REQ_WIFI_AP_GET_STA_LIST;
  req.req_wifi_ap_get_sta_list = &payload;
  resp = rpc_transact(&req, "WifiApGetStaList");
  if (resp == NULL)
    {
      return -ETIMEDOUT;
    }

  if (!c6_softap_response(resp, &req) ||
      resp->payload_case != RPC__PAYLOAD_RESP_WIFI_AP_GET_STA_LIST ||
      resp->resp_wifi_ap_get_sta_list == NULL)
    {
      goto out;
    }

  if (resp->resp_wifi_ap_get_sta_list->resp != 0)
    {
      ret = -EIO;
      goto out;
    }

  list = resp->resp_wifi_ap_get_sta_list->sta_list;

  /* Legacy slave serializes its full 15-slot array, with num valid slots. */

  if (list == NULL || list->num < 0 || list->num > 2 ||
      list->n_sta > 15 || (size_t)list->num > list->n_sta ||
      (list->num != 0 && list->sta == NULL))
    {
      goto out;
    }

  for (int i = 0; i < list->num; i++)
    {
      FAR WifiStaInfo *sta = list->sta[i];
      if (sta == NULL || sta->mac.data == NULL || sta->mac.len != 6 ||
          (sta->mac.data[0] & 1) != 0 ||
          memcmp(sta->mac.data, "\0\0\0\0\0\0", 6) == 0 ||
          sta->rssi < -127 || sta->rssi > 0)
        {
          goto out;
        }

      if (i != 0 && memcmp(sta->mac.data, list->sta[0]->mac.data, 6) == 0)
        {
          goto out;
        }

      he += (sta->bitmask & (1u << 4)) != 0;
    }

  *count = (uint8_t)list->num;
  *he_count = (uint8_t)he;
  ret = 0;
out:
  rpc__free_unpacked(resp, NULL);
  return ret;
}

int esp_hosted_rpc_phy_sta(FAR uint8_t *mode)
{
  RpcReqWifiStaGetNegotiatedPhymode payload =
    RPC__REQ__WIFI_STA_GET_NEGOTIATED_PHYMODE__INIT;
  Rpc req = RPC__INIT;
  FAR Rpc *resp;
  int ret = -EPROTO;

  if (mode == NULL)
    {
      return -EINVAL;
    }

  req.msg_type = RPC_TYPE__Req;
  req.msg_id = RPC_ID__Req_WifiStaGetNegotiatedPhymode;
  req.payload_case = RPC__PAYLOAD_REQ_WIFI_STA_GET_NEGOTIATED_PHYMODE;
  req.req_wifi_sta_get_negotiated_phymode = &payload;
  resp = rpc_transact(&req, "WifiStaGetNegotiatedPhymode");
  if (resp == NULL)
    {
      return -ETIMEDOUT;
    }

  if (c6_softap_response(resp, &req) &&
      resp->payload_case ==
        RPC__PAYLOAD_RESP_WIFI_STA_GET_NEGOTIATED_PHYMODE &&
      resp->resp_wifi_sta_get_negotiated_phymode != NULL)
    {
      FAR RpcRespWifiStaGetNegotiatedPhymode *body =
        resp->resp_wifi_sta_get_negotiated_phymode;
      if (body->resp != 0)
        {
          ret = -EIO;
        }
      else if (body->phymode == 1 || body->phymode == 2 ||
               body->phymode == 4 || body->phymode == 5 ||
               body->phymode == C6_WIFI_PHY_HE20)
        {
          *mode = (uint8_t)body->phymode;
          ret = 0;
        }
    }

  rpc__free_unpacked(resp, NULL);
  return ret;
}
