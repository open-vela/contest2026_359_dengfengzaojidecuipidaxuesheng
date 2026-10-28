"""Pinned SDIO error propagation fixes. No deployment, retries or resets."""
import re


def once(text, before, after):
    if text.count(before) != 1:
        raise ValueError("Unsupported source: " + before[:100])
    return text.replace(before, after)


def span(text, signature):
    start = text.rindex(signature)
    at = text.index('{', start)
    end, depth = at + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return start, end


def patch_sdio(text):
    text = once(text, '#include <string.h>',
                '#include <string.h>\n#include <syslog.h>')
    start, end = span(text, 'int sdio_io_rw_direct(')
    body = text[start:end]
    body = once(body, '''  sdio_takelock(dev);
  sdio_sendcmdpoll(dev, SD_ACMD52, arg.value);
  ret = SDIO_RECVR5(dev, SD_ACMD52, &data);''', '''  ret = sdio_takelock(dev);
  if (ret < 0)
    {
      return ret;
    }

  ret = sdio_sendcmdpoll(dev, SD_ACMD52, arg.value);
  if (ret == OK)
    {
      ret = SDIO_RECVR5(dev, SD_ACMD52, &data);
    }
''')
    body = once(body, '  if (resp.flags.error)', '''  if (resp.flags.error || resp.flags.com_crc_error ||
      resp.flags.illegal_command)''')
    text = text[:start] + body + text[end:]
    start, end = span(text, 'int sdio_io_rw_extended(')
    body = text[start:end]
    body = once(body, '  uint32_t data;', '  uint32_t data = 0;')
    body = once(body, '  sdio_eventset_t wkupevent;', '''  sdio_eventset_t wkupevent = 0;
  FAR const char *stage = "lock";''')
    body = once(body, '  sdio_takelock(dev);', '''  ret = sdio_takelock(dev);
  if (ret < 0)
    {
      return ret;
    }''')
    body = once(body, '''          SDIO_DMASENDSETUP(dev, buf, blocklen * nblocks);
          SDIO_SENDCMD(dev, SD_ACMD53WR, arg.value);

          wkupevent = SDIO_EVENTWAIT(dev);
          ret = SDIO_RECVR5(dev, SD_ACMD53WR, &data);''', '''          stage = "tx-setup";
          ret = SDIO_DMASENDSETUP(dev, buf, blocklen * nblocks);
          if (ret < 0)
            {
              goto cancel;
            }

          stage = "tx-command";
          ret = SDIO_SENDCMD(dev, SD_ACMD53WR, arg.value);
          if (ret < 0)
            {
              goto cancel;
            }

          stage = "tx-response";
          wkupevent = SDIO_EVENTWAIT(dev);
          ret = SDIO_RECVR5(dev, SD_ACMD53WR, &data);''')
    body = once(body, '''          sdio_sendcmdpoll(dev, SD_ACMD53WR, arg.value);
          ret = SDIO_RECVR5(dev, SD_ACMD53WR, &data);

          SDIO_DMASENDSETUP(dev, buf, blocklen * nblocks);
          wkupevent = SDIO_EVENTWAIT(dev);''', '''          stage = "tx-command";
          ret = sdio_sendcmdpoll(dev, SD_ACMD53WR, arg.value);
          if (ret < 0)
            {
              goto cancel;
            }

          stage = "tx-response";
          ret = SDIO_RECVR5(dev, SD_ACMD53WR, &data);
          if (ret < 0)
            {
              goto cancel;
            }

          memcpy(&resp, &data, sizeof(resp));
          if (resp.flags.error || resp.flags.com_crc_error ||
              resp.flags.illegal_command)
            {
              ret = -EIO;
              goto cancel;
            }

          if (resp.flags.function_number || resp.flags.out_of_range)
            {
              ret = -EINVAL;
              goto cancel;
            }

          stage = "tx-setup";
          ret = SDIO_DMASENDSETUP(dev, buf, blocklen * nblocks);
          if (ret < 0)
            {
              goto cancel;
            }

          wkupevent = SDIO_EVENTWAIT(dev);''')
    body = once(body, '      ret = SDIO_DMARECVSETUP(dev, buf, blocklen * nblocks);',
                '      stage = "rx-setup";\n      ret = SDIO_DMARECVSETUP(dev, buf, blocklen * nblocks);')
    body = body.replace('''          SDIO_CANCEL(dev);
          sdio_givelock(dev);
          return ret;''', '          goto cancel;')
    body = once(body, '      ret = SDIO_SENDCMD(dev, SD_ACMD53RD, arg.value);',
                '\n      stage = "rx-command";\n      ret = SDIO_SENDCMD(dev, SD_ACMD53RD, arg.value);')
    body = once(body, '      ret = SDIO_RECVR5(dev, SD_ACMD53RD, &data);',
                '      stage = "rx-response";\n      ret = SDIO_RECVR5(dev, SD_ACMD53RD, &data);')
    tail = body.index('  wlinfo("Transaction ends')
    body = body[:tail] + '''  /* Preserve the CMD53 response.  CMD52 with argument zero is a read
   * of CCCR[0], not a function abort, and must not replace this R5.
   */

  if (ret != OK)
    {
      goto out;
    }

  memcpy(&resp, &data, sizeof(resp));
  stage = "r5-status";
  if (resp.flags.error || resp.flags.com_crc_error ||
      resp.flags.illegal_command)
    {
      ret = -EIO;
    }
  else if (resp.flags.function_number || resp.flags.out_of_range)
    {
      ret = -EINVAL;
    }
  else
    {
      stage = "transfer";
      if ((wkupevent & SDIOWAIT_TIMEOUT) != 0)
        {
          ret = -ETIMEDOUT;
        }
      else if ((wkupevent & SDIOWAIT_ERROR) != 0 ||
               (wkupevent & SDIOWAIT_TRANSFERDONE) == 0)
        {
          ret = -EIO;
        }
    }

  goto out;

cancel:

  /* No wait may follow a failed setup or command submission. */

  SDIO_CANCEL(dev);

out:
  if (ret < 0)
    {
      syslog(LOG_ERR, "SDIO53 %c addr=%05" PRIx32
             " stage=%s ret=%d event=%x r5=%08" PRIx32 "\\n",
             write ? 'W' : 'R', address, stage, ret,
             (unsigned int)wkupevent, data);
    }

  sdio_givelock(dev);
  return ret;
}'''
    return text[:start] + body + text[end:]


def patch_p4(text):
    start, end = span(text, 'static int esp32p4_sendcmd(')
    body = once(text[start:end], '''  esp32p4_ciu_sendcmd(regval, arg);

  return OK;''', '''  /* ciu_sendcmd returns one if the previous command never cleared. */

  return esp32p4_ciu_sendcmd(regval, arg) == 0 ? OK : -ETIMEDOUT;''')
    text = text[:start] + body + text[end:]
    for signature in ('static int esp32p4_dmarecvsetup(',
                      'static int esp32p4_dmasendsetup('):
        start, end = span(text, signature)
        body = once(text[start:end], '  uint32_t regval;',
                    '  uint32_t regval;\n  int ret;')
        body = once(body, '''  if (esp32p4_fill_dma_desc(priv))
    {
      return -ENOMEM;
    }''', '''  ret = esp32p4_fill_dma_desc(priv);
  if (ret < 0)
    {
      return ret;
    }''')
        text = text[:start] + body + text[end:]
    return text


def patch_rpc(text):
    # A per-call output survives unlocking without a shared last-error race.
    text = once(text, 'static FAR Rpc *rpc_transact_locked(FAR const Rpc *req, FAR const char *label)',
                '''static FAR Rpc *rpc_transact_locked(FAR const Rpc *req,
                                    FAR const char *label, FAR int *error)''')
    start, end = span(text, 'static FAR Rpc *rpc_transact_locked(')
    body = text[start:end]
    body = once(body, '  uint8_t buf[256];',
                '  FAR Rpc *response;\n  uint8_t buf[256];')
    body = once(body, '  packed = rpc__get_packed_size(req);',
                '  *error = -ETIMEDOUT;\n  packed = rpc__get_packed_size(req);')
    body = once(body, '''             (unsigned int)packed);
      return NULL;''', '''             (unsigned int)packed);
      *error = -EMSGSIZE;
      return NULL;''')
    body = once(body, '''  if (rpc__pack(req, buf) != packed)
    {
      return NULL;''', '''  if (rpc__pack(req, buf) != packed)
    {
      *error = -EPROTO;
      return NULL;''')
    body = once(body, '''  if (tlvlen < 0)
    {
      return NULL;''', '''  if (tlvlen < 0)
    {
      *error = tlvlen;
      return NULL;''')
    body = once(body, '  if (!c6_rpc_begin(&g_rpc, req->uid)) return NULL;', '''  if (!c6_rpc_begin(&g_rpc, req->uid))
    {
      *error = -EBUSY;
      return NULL;
    }''')
    body = once(body, '''  return c6_rpc_finish(&g_rpc);

failed:''', '''  response = c6_rpc_finish(&g_rpc);
  if (response != NULL)
    {
      *error = OK;
    }

  return response;

failed:
  *error = ret;''')
    text = text[:start] + body + text[end:]
    text = once(text, 'static FAR Rpc *rpc_transact(FAR const Rpc *req, FAR const char *label)',
                '''static FAR Rpc *rpc_transact(FAR const Rpc *req,
                             FAR const char *label, FAR int *error)''')
    start, end = span(text, 'static FAR Rpc *rpc_transact(')
    body = text[start:end]
    body = once(body, '  ret = pthread_mutex_lock(&g_rpc_transaction_lock);',
                '  *error = -EINVAL;\n  ret = pthread_mutex_lock(&g_rpc_transaction_lock);')
    body = once(body, '''  if (ret != 0)
    {
      return NULL;''', '''  if (ret != 0)
    {
      *error = -ret;
      return NULL;''')
    body = once(body, '  response = request.uid ? rpc_transact_locked(&request, label) : NULL;',
                '''  *error = -EBUSY;
  response = request.uid ?
             rpc_transact_locked(&request, label, error) : NULL;''')
    text = text[:start] + body + text[end:]
    signatures = re.findall(r'^int (esp_hosted_rpc_\w+)\(', text, re.M)
    for name in signatures:
        start, end = span(text, 'int ' + name + '(')
        body = text[start:end]
        if 'rpc_transact(' not in body:
            continue
        brace = body.index('{') + 1
        body = body[:brace] + '\n  int rpc_error;' + body[brace:]
        body = re.sub(r'rpc_transact\(([^;\n]*?)\)', r'rpc_transact(\1, &rpc_error)', body)
        body = body.replace('return -ETIMEDOUT;', 'return rpc_error;')
        text = text[:start] + body + text[end:]
    return text


def patch_rssi(text):
    text = once(text, '''int esp_hosted_rpc_wifi_rssi(int *dbm)
{''', '''int esp_hosted_rpc_wifi_rssi(int *dbm)
{
  int rpc_error;

''')
    return once(text, '''  Rpc *resp=rpc_transact(&req,"WifiStaGetRssi");if(!resp)return -ETIMEDOUT;''',
                '''  Rpc *resp = rpc_transact(&req, "WifiStaGetRssi", &rpc_error);
  if (resp == NULL)
    {
      return rpc_error;
    }
''')


PATCHES = {
    'nuttx/drivers/mmcsd/sdio.c': patch_sdio,
    'nuttx/arch/risc-v/src/esp32p4/esp32p4_sdmmc.c': patch_p4,
    'apps/system/c6probe/esp_hosted_rpc.c': patch_rpc,
    'apps/system/c6probe/status_rssi.inc': patch_rssi,
}
