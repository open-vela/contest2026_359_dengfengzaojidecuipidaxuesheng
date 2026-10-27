#!/usr/bin/env python3
"""Add SoftAP RPCs to the pinned, serialized companion-radio client."""

import hashlib
from pathlib import Path


def replace_once(text, old, new):
    if text.count(new) == 1 and text.count(old) == 0:
        return text
    if text.count(old) != 1 or new in text:
        raise ValueError('Unknown SoftAP transport/header anchor: ' + old[:80])
    return text.replace(old, new, 1)


def apply(apps):
    folder = Path(apps) / 'system/c6probe'
    rpc = folder / 'esp_hosted_rpc.c'
    text = rpc.read_text()
    required = ['rpc_transact_locked(', 'c6_rpc_next_uid(&g_rpc)',
                'pthread_mutex_unlock(&g_rpc_transaction_lock)',
                'if (strcmp(label, "WifiSetConfig") != 0)']
    if not all(marker in text for marker in required):
        raise ValueError('SoftAP requires the known serialized/redacted RPC owner')
    include = '#include "c6_softap.h"'
    if include not in text:
        anchor = '#include "esp_hosted_rpc.pb-c.h"'
        if text.count(anchor) != 1:
            raise ValueError('Unexpected SoftAP RPC includes')
        text = text.replace(anchor, anchor + '\n' + include, 1)
    old = '  req_payload.mode = mode;'
    new = '  req_payload.mode = c6_softap_preserve_mode(mode);'
    if new not in text:
        if text.count(old) != 1:
            raise ValueError('Unexpected SoftAP mode setter')
        text = text.replace(old, new, 1)
    include = '#include "c6_softap_rpc.inc"'
    if include not in text:
        text = text.rstrip() + '\n\n' + include + '\n'
    proto = (folder / 'esp_hosted_rpc.pb-c.h').read_text()
    for marker in ['WIFI_CONFIG__U_AP = 1', 'RpcReqWifiStop',
                   'RPC_ID__Req_GetWifiMode = 259',
                   'RPC_ID__Req_WifiSetConfig = 284',
                   'RPC_ID__Req_WifiGetConfig = 285',
                   'RPC_ID__Req_WifiSetProtocol = 297',
                   'RPC_ID__Req_WifiGetProtocol = 298',
                   'RPC_ID__Req_WifiSetBandwidth = 299',
                   'RPC_ID__Req_WifiGetBandwidth = 300',
                   'RPC_ID__Req_WifiApGetStaList = 311',
                   'RPC_ID__Req_WifiStaGetNegotiatedPhymode = 339']:
        if marker not in proto:
            raise ValueError('Unsupported Hosted AP protobuf ABI: ' + marker)
    transport = folder / 'esp_hosted.c'
    header = folder / 'esp_hosted.h'
    old = '  if (if_type >= ESP_HOSTED_IF_MAX) return;\n\n  cb = g_hosted.rx_cb[if_type];'
    new = '''  if (if_type >= ESP_HOSTED_IF_MAX) return;

  /* Keep the legacy callback selector as if_type. STA/AP expose only
   * instance zero; validate the wire instance before dispatching them.
   */

  if ((if_type == ESP_HOSTED_IF_STA || if_type == ESP_HOSTED_IF_AP) &&
      hdr->if_num != 0)
    {
      return;
    }

  cb = g_hosted.rx_cb[if_type];'''
    transport_text = replace_once(transport.read_text(), old, new)
    if transport_text.count('cb(g_hosted.rx_arg[if_type], if_type,') != 1:
        raise ValueError('Unknown Hosted callback selector contract')
    header_text = replace_once(header.read_text(),
        'typedef CODE void (*esp_hosted_rx_cb_t)(FAR void *arg, uint8_t if_num,',
        'typedef CODE void (*esp_hosted_rx_cb_t)(FAR void *arg, uint8_t if_type,')
    # Validate all inputs before writing any member of the preparation set.
    for path, updated in [(rpc, text), (transport, transport_text),
                          (header, header_text)]:
        if path.read_text() != updated:
            path.write_text(updated)
    return {'mode': 'AP+STA; Wi-Fi stopped during credential installation',
            'interface': 'ap0', 'authentication': 'WPA2-PSK/CCMP',
            'callback_selector': 'if_type; STA/AP wire instance must be zero',
            'tool_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            'source_sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                              for p in [rpc, transport, header]}}


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apps', type=Path, required=True)
    print(apply(parser.parse_args().apps))
