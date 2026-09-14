"""Compile the production desktop backend against deterministic link/IP stubs."""
from pathlib import Path
import subprocess
import tempfile


def main():
    tools = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        (root / 'netutils').mkdir()
        (root / 'c6net.h').write_text('''
#include "link_state.h"
#include "scan_results.h"
int c6net_get_link_snapshot(struct c6_link_snapshot *);
int c6net_prepare(void);
int c6net_disconnect(void);
int c6net_connect(const char *, const char *);
int esp_hosted_rpc_wifi_scan_results(struct c6_scan_ap *, size_t, size_t *);
''')
        (root / 'netutils/netlib.h').write_text('''
#include <netinet/in.h>
int netlib_get_ipv4addr(const char *, struct in_addr *);
int netlib_set_ipv4addr(const char *, const struct in_addr *);
int netlib_set_dripv4addr(const char *, const struct in_addr *);
int netlib_set_ipv4netmask(const char *, const struct in_addr *);
''')
        (root / 'test.c').write_text('''
#include "desktop_backend.h"
#include "link_state.h"
#include <arpa/inet.h>
#include <assert.h>
static int mode, reads;
static int cleared, connect_calls, disconnect_error = -ETIMEDOUT;
static int clear_error;
static int clear(const char *name, const struct in_addr *a, int step) {
  assert(!strcmp(name, "eth0") && !a->s_addr);
  assert(cleared == step - 1);
  cleared++;
  if (clear_error == step) {errno = EIO; return -1;}
  return 0;
}
int netlib_set_ipv4addr(const char *n, const struct in_addr *a) {return clear(n,a,1);}
int netlib_set_dripv4addr(const char *n, const struct in_addr *a) {return clear(n,a,2);}
int netlib_set_ipv4netmask(const char *n, const struct in_addr *a) {return clear(n,a,3);}
int c6net_get_link_snapshot(struct c6_link_snapshot *s) {
  *s = (struct c6_link_snapshot){1, true, true, true, true};
  if (mode == 2 && reads++) s->generation++;
  return 0;
}
int netlib_get_ipv4addr(const char *name, struct in_addr *a) {
  assert(!strcmp(name, "eth0"));
  if (mode == 1) {errno = EIO; return -1;}
  a->s_addr = htonl(mode == 3 ? 0 : 0xc0000201);
  return 0;
}
int c6net_prepare(void) {return 0;}
int c6net_disconnect(void) {return disconnect_error;}
int c6net_connect(const char *a, const char *b) {
  assert(!strcmp(a,"fixture") && !strcmp(b,""));
  connect_calls++;return 0;
}
int esp_hosted_rpc_wifi_scan_results(struct c6_scan_ap *a, size_t b, size_t *c)
{(void)a;(void)b;*c=0;return 0;}
int main(void) {
  struct c6_desktop_link link;
  assert(g_c6_desktop_backend.disconnect() == -ETIMEDOUT);
  assert(cleared == 0);
  disconnect_error = 0;
  assert(!g_c6_desktop_backend.disconnect() && cleared == 3);
  cleared = 0;
  assert(!g_c6_desktop_backend.connect("fixture", "") && connect_calls == 1);
  for (int step = 1; step <= 3; step++) {
    cleared = 0; clear_error = step;
    assert(g_c6_desktop_backend.disconnect() == -EIO);
    assert(connect_calls == 1 && cleared == step);
  }
  cleared = 0; clear_error = 0;
  assert(!g_c6_desktop_backend.status(&link));
  assert(link.ipv4_ready && link.ipv4[0] == 192 && link.ipv4[3] == 1);
  mode=1; assert(g_c6_desktop_backend.status(&link) == -EIO);
  mode=2; assert(g_c6_desktop_backend.status(&link) == -EAGAIN);
  assert(!link.ipv4_ready && !link.associated);
  mode=3; assert(!g_c6_desktop_backend.status(&link));
  assert(!link.ipv4_ready && link.ipv4[0] == 0);
  return 0;
}
''')
        binary = root / 'test'
        subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-pthread', '-I', str(root), '-I', str(tools / 'c6'),
                        str(root / 'test.c'), str(tools / 'c6/desktop_backend.c'),
                        '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=10)
    print('PASS: IPv4 observation and stale generation; disconnect address cleanup and failures')


if __name__ == '__main__':
    main()
