"""Add disconnect to an already prepared C6 tree, preserving unrelated code."""
from pathlib import Path
import argparse

RPC = '''
int esp_hosted_rpc_wifi_disconnect(void)
{
  RpcReqWifiDisconnect payload = RPC__REQ__WIFI_DISCONNECT__INIT;
  Rpc req = { PROTOBUF_C_MESSAGE_INIT(&rpc__descriptor) };
  req.msg_type = RPC_TYPE__Req;
  req.msg_id = RPC_ID__Req_WifiDisconnect;
  req.uid = 1;
  req.payload_case = RPC__PAYLOAD_REQ_WIFI_DISCONNECT;
  req.req_wifi_disconnect = &payload;
  Rpc *response = rpc_transact(&req, "WifiDisconnect");
  if (response == NULL) return -ETIMEDOUT;
  int result = -EPROTO;
  if (response->payload_case == RPC__PAYLOAD_RESP_WIFI_DISCONNECT &&
      response->resp_wifi_disconnect != NULL)
    result = response->resp_wifi_disconnect->resp == 0 ? 0 : -EIO;
  rpc__free_unpacked(response, NULL);
  return result;
}

'''
NET = '''
int c6net_disconnect(void)
{
  int ret = pthread_mutex_lock(&g_c6net_connect_lock);
  if (ret != 0) return -ret;
  if (!c6net_is_initialized()) ret = -ENETDOWN;
  else
    {
      ret = esp_hosted_rpc_wifi_disconnect();
      if (ret == 0) ret = c6_link_update(&g_c6link, C6_LINK_DISCONNECTED);
    }
  pthread_mutex_unlock(&g_c6net_connect_lock);
  return ret;
}

'''


def adapt(source, marker, addition, symbol):
    if symbol in source:
        if addition not in source:
            raise ValueError('Unrecognized existing implementation: ' + symbol)
        return source
    if source.count(marker) != 1:
        raise ValueError('Unexpected source anchor: ' + marker)
    return source.replace(marker, addition + marker, 1)


def adapt_sources(sources):
    specs = {
        'esp_hosted_rpc.c': ('int esp_hosted_rpc_wifi_connect(', RPC,
                             'int esp_hosted_rpc_wifi_disconnect(void)'),
        'c6net.c': ('int c6net_prepare(void)', NET, 'int c6net_disconnect(void)'),
        'esp_hosted.h': ('int esp_hosted_rpc_wifi_connect(',
                         'int esp_hosted_rpc_wifi_disconnect(void);\n',
                         'int esp_hosted_rpc_wifi_disconnect(void);'),
        'c6net.h': ('int c6net_prepare(void);',
                    'int c6net_disconnect(void);\n', 'int c6net_disconnect(void);'),
    }
    outputs = {}
    for name, (marker, addition, symbol) in specs.items():
        source = sources[name]
        outputs[name] = adapt(source, marker, addition, symbol)
    return outputs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.output.exists():
        raise ValueError('Output must be a fresh directory')
    names = ('esp_hosted_rpc.c', 'c6net.c', 'esp_hosted.h', 'c6net.h')
    outputs = adapt_sources({name: (args.source / name).read_text()
                             for name in names})
    args.output.mkdir(parents=True)
    for name, text in outputs.items():
        (args.output / name).write_text(text)


if __name__ == '__main__':
    main()
