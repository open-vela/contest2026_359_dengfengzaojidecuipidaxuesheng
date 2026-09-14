"""Prevent c6net from polling Hosted while an RPC owns the response window."""
import argparse
from pathlib import Path
from adapt_c6_rx_dispatch import once


def adapt_rpc(text):
    text = once(text, 'static pthread_mutex_t g_rpc_transaction_lock = PTHREAD_MUTEX_INITIALIZER;',
                '''static pthread_mutex_t g_rpc_transaction_lock = PTHREAD_MUTEX_INITIALIZER;

bool esp_hosted_rpc_is_waiting(void)
{
  return c6_rpc_waiting(&g_rpc);
}''')
    return text


def adapt_net(text):
    if '#include "esp_hosted.h"' not in text:
        text = once(text, '#include "c6net.h"',
                    '#include "c6net.h"\n#include "esp_hosted.h"')
    text = once(text, '      while (esp_hosted_poll() > 0)',
                '      while (!esp_hosted_rpc_is_waiting() && esp_hosted_poll() > 0)')
    return text


def adapt_header(text):
    anchor = 'int esp_hosted_poll(void);'
    declaration = '''int esp_hosted_poll(void);

/* True while the synchronous RPC path owns the Hosted response window. */

bool esp_hosted_rpc_is_waiting(void);'''
    return once(text, anchor, declaration)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('rpc', type=Path)
    p.add_argument('net', type=Path)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    output = a.output
    if output.exists(): raise SystemExit('Fresh output required')
    output.mkdir(parents=True)
    (output / 'esp_hosted_rpc.c').write_text(adapt_rpc(a.rpc.read_text()))
    (output / 'c6net.c').write_text(adapt_net(a.net.read_text()))


if __name__ == '__main__':
    main()
