"""Execute the exact generated disconnect RPC against malformed responses."""
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from adapt_c6_disconnect import RPC


def main():
    prelude = r'''
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <string.h>
#define RPC__REQ__WIFI_DISCONNECT__INIT {0}
#define PROTOBUF_C_MESSAGE_INIT(x) 0
enum {RPC_TYPE__Req=1, RPC_ID__Req_WifiDisconnect=2,
      RPC__PAYLOAD_REQ_WIFI_DISCONNECT=3, RPC__PAYLOAD_RESP_WIFI_DISCONNECT=4};
typedef struct {int unused;} RpcReqWifiDisconnect;
typedef struct {int resp;} Response;
typedef struct {
  int base, msg_type, msg_id, uid, payload_case;
  RpcReqWifiDisconnect *req_wifi_disconnect;
  Response *resp_wifi_disconnect;
} Rpc;
static int mode, freed;
static Rpc response;
static Response payload;
static Rpc *rpc_transact(Rpc *req, const char *name) {
  assert(req->msg_type == RPC_TYPE__Req);
  assert(req->msg_id == RPC_ID__Req_WifiDisconnect);
  assert(req->payload_case == RPC__PAYLOAD_REQ_WIFI_DISCONNECT);
  assert(req->req_wifi_disconnect && !strcmp(name,"WifiDisconnect"));
  if (mode == 1) return NULL;
  payload.resp = mode == 2 ? 7 : 0;
  response.payload_case = mode == 3 ? 99 : RPC__PAYLOAD_RESP_WIFI_DISCONNECT;
  response.resp_wifi_disconnect = mode == 4 ? NULL : &payload;
  return &response;
}
static void rpc__free_unpacked(Rpc *r, void *allocator) {
  assert(r == &response && !allocator);freed++;
}
'''
    tests = r'''
int main(void) {
  const int expected[] = {0,-ETIMEDOUT,-EIO,-EPROTO,-EPROTO};
  for (int cycle=0; cycle<100; cycle++)
    for (mode=0; mode<5; mode++) {
      int before=freed;
      assert(esp_hosted_rpc_wifi_disconnect() == expected[mode]);
      assert(freed == before + (mode != 1));
    }
  return 0;
}
'''
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        source = root / 'rpc.c'
        source.write_text(prelude + RPC + tests)
        binary = root / 'rpc'
        subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=10)
    print('PASS: 500 disconnect RPC calls; success, timeout, remote error, malformed response, ownership')


if __name__ == '__main__':
    main()
