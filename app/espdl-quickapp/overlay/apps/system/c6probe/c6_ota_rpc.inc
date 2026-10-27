/* SPDX-License-Identifier: Apache-2.0 */

#include "c6_ota.h"
#include "c6_ota_release.h"
#include "c6_wifi_trial.h"

/* Included after the serialized RPC implementation. A whole update owns
 * its transaction lock, including gaps between chunks. Old Hosted has no
 * offset/idempotency/abort ABI; consume one attempt per P4 boot, including
 * uncertain begin/end replies. Other RPC clients cannot interleave.
 */

static int c6_ota_exchange(enum c6_ota_operation_e operation,
                          const uint8_t *data, size_t size, void *arg)
{
  RpcReqOTABegin begin = RPC__REQ__OTABEGIN__INIT;
  RpcReqOTAWrite write = RPC__REQ__OTAWRITE__INIT;
  RpcReqOTAEnd end = RPC__REQ__OTAEND__INIT;
  Rpc request = RPC__INIT;
  Rpc *response;
  int status = -EPROTO;

  (void)arg;
  request.msg_type = RPC_TYPE__Req;
  request.uid = c6_rpc_next_uid(&g_rpc);
  if (!request.uid) return -EOVERFLOW;

  switch (operation)
    {
      case C6_OTA_BEGIN:
        request.msg_id = RPC_ID__Req_OTABegin;
        request.payload_case = RPC__PAYLOAD_REQ_OTA_BEGIN;
        request.req_ota_begin = &begin;
        break;

      case C6_OTA_WRITE:
        if (data == NULL || size == 0 || size > C6_OTA_CHUNK_SIZE)
          return -EINVAL;
        request.msg_id = RPC_ID__Req_OTAWrite;
        request.payload_case = RPC__PAYLOAD_REQ_OTA_WRITE;
        request.req_ota_write = &write;
        write.ota_data.data = (uint8_t *)data;
        write.ota_data.len = size;
        break;

      case C6_OTA_END:
        request.msg_id = RPC_ID__Req_OTAEnd;
        request.payload_case = RPC__PAYLOAD_REQ_OTA_END;
        request.req_ota_end = &end;
        break;

      default:
        return -EINVAL;
    }

  response = rpc_transact_locked(&request, "C6OTA");
  if (response == NULL) return -ETIMEDOUT;
  if (response->msg_type == RPC_TYPE__Resp &&
      response->msg_id == request.msg_id + 256)
    {
      if (operation == C6_OTA_BEGIN &&
          response->payload_case == RPC__PAYLOAD_RESP_OTA_BEGIN &&
          response->resp_ota_begin != NULL)
        status = response->resp_ota_begin->resp;
      else if (operation == C6_OTA_WRITE &&
               response->payload_case == RPC__PAYLOAD_RESP_OTA_WRITE &&
               response->resp_ota_write != NULL)
        status = response->resp_ota_write->resp;
      else if (operation == C6_OTA_END &&
               response->payload_case == RPC__PAYLOAD_RESP_OTA_END &&
               response->resp_ota_end != NULL)
        status = response->resp_ota_end->resp;
    }

  rpc__free_unpacked(response, NULL);
  return status == 0 ? 0 : status == -EPROTO ? -EPROTO : -EIO;
}

int esp_hosted_rpc_c6_ota(const uint8_t *image, size_t size,
                          struct c6_ota_result_s *result)
{
  int ret;

  if (result == NULL) return -EINVAL;
  memset(result, 0, sizeof(*result));
  ret = size == C6_OTA_RELEASE_BYTES ?
        c6_ota_validate(image, size, C6_OTA_RELEASE_SHA256) : -EPERM;
  if (ret != 0) return result->error = ret;
  ret = pthread_mutex_trylock(&g_rpc_transaction_lock);
  if (ret != 0) return result->error = -ret;
  if (c6_wifi_trial_pending())
    ret = -EBUSY;
  else if (g_c6_ota_attempted)
    ret = -EALREADY;
  else
    {
      g_c6_ota_attempted = true;
      ret = c6_ota_transfer(image, size, c6_ota_exchange, NULL, result);
    }

  result->error = ret;
  pthread_mutex_unlock(&g_rpc_transaction_lock);
  return ret;
}
