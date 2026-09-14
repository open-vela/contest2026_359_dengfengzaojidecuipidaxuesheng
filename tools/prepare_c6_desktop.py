"""Compose C6 desktop prerequisites into a fresh directory, without hardware IO."""
import argparse
import hashlib
import json
from pathlib import Path

from adapt_c6_rx_dispatch import adapt as rx
from adapt_c6_wifi_events import adapt as events
from adapt_c6_scan_results import adapt as scan
from adapt_c6_daemon_retry import adapt as retry
from adapt_c6_link_state import adapt as link
from adapt_c6_prepare import adapt as radio_prepare
from adapt_c6_rpc_poll_owner import adapt_net as poll_net
from adapt_c6_rpc_poll_owner import adapt_header as poll_header
from adapt_c6_rpc_poll_owner import adapt_rpc as poll_rpc
from adapt_c6_disconnect import adapt_sources as disconnect

TOOLS = Path(__file__).resolve().parent

MAILBOX_WAITING_FUNCTION = b'''static bool c6_rpc_waiting(struct c6_rpc_mailbox *box)
{
  if (pthread_mutex_lock(&box->lock) != 0) return true;
  bool waiting = box->waiting != 0 || box->response != NULL;
  pthread_mutex_unlock(&box->lock);
  return waiting;
}

'''


def digest(data):
    return hashlib.sha256(data).hexdigest()


def prepare(source, output):
    source = source.resolve()
    output = output.resolve()
    if output.exists() or source == output or source in output.parents:
        raise ValueError('A fresh output outside the source directory is required')
    names = ('esp_hosted.c', 'esp_hosted.h', 'esp_hosted_rpc.c', 'c6net.c',
             'c6net.h', 'rpc_mailbox.h')
    inputs = {name: (source / name).read_bytes() for name in names}
    rpc = inputs['esp_hosted_rpc.c'].decode()
    mailbox = inputs['rpc_mailbox.h']
    current_mailbox = (TOOLS / 'c6/rpc_mailbox.h').read_bytes()
    previous_mailbox = current_mailbox.replace(MAILBOX_WAITING_FUNCTION, b'', 1)
    if previous_mailbox == current_mailbox:
        raise RuntimeError('Current mailbox header lacks the polling guard')
    if ('#include "rpc_mailbox.h"' not in rpc or
            mailbox not in (current_mailbox, previous_mailbox)):
        raise ValueError('Matching mailbox adaptation is required first')
    if 'g_rpc_transaction_lock' not in rpc:
        raise ValueError('RPC transaction serialization is required')
    # Perform every transformation before creating any output files.
    generated = {
        'esp_hosted.c': rx(inputs['esp_hosted.c'].decode()).encode(),
        'esp_hosted.h': poll_header(inputs['esp_hosted.h'].decode()).encode(),
        'esp_hosted_rpc.c': poll_rpc(scan(events(rpc))).encode(),
        'c6net.c': radio_prepare(poll_net(link(retry(inputs['c6net.c'].decode())))).encode(),
        'rpc_mailbox.h': current_mailbox,
        'scan_results.h': (TOOLS / 'c6/scan_results.h').read_bytes(),
        'link_state.h': (TOOLS / 'c6/link_state.h').read_bytes(),
        'desktop_worker.h': (TOOLS / 'c6/desktop_worker.h').read_bytes(),
        'desktop_worker.c': (TOOLS / 'c6/desktop_worker.c').read_bytes(),
        'desktop_backend.h': (TOOLS / 'c6/desktop_backend.h').read_bytes(),
        'desktop_backend.c': (TOOLS / 'c6/desktop_backend.c').read_bytes(),
    }
    header = inputs['c6net.h'].decode()
    anchor = 'bool c6net_is_initialized(void);'
    if header.count(anchor) != 1:
        raise ValueError('Unexpected c6net public header')
    header = header.replace(anchor, 'int c6net_prepare(void);\nstruct c6_link_snapshot;\n'
                            'int c6net_get_link_snapshot(struct c6_link_snapshot *out);\n' + anchor)
    generated['c6net.h'] = header.encode()
    names = ('esp_hosted_rpc.c', 'c6net.c', 'esp_hosted.h', 'c6net.h')
    for name, text in disconnect({name: generated[name].decode()
                                  for name in names}).items():
        generated[name] = text.encode()
    output.mkdir(parents=True, exist_ok=False)
    for name, data in generated.items():
        (output / name).write_bytes(data)
    manifest = {
        'status': 'prerequisites-only-not-desktop-feature',
        'source': str(source),
        'inputs': {name: digest(data) for name, data in inputs.items()},
        'outputs': {name: digest(data) for name, data in generated.items()},
    }
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, help='Mailbox-adapted system/c6probe directory')
    parser.add_argument('output', type=Path, help='New isolated output directory')
    args = parser.parse_args()
    prepare(args.source, args.output)
    print('Prepared composed prerequisites; no build, active-tree writes or radio operations')


if __name__ == '__main__':
    main()
