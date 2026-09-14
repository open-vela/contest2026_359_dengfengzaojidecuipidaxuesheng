"""Verify composition, provenance and refusal to overwrite existing output."""
import argparse
import hashlib
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from prepare_c6_desktop import prepare


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    args = parser.parse_args()
    before = (args.source / 'c6net.c').read_bytes()
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / 'candidate'
        manifest = prepare(args.source, out)
        for name, expected in manifest['outputs'].items():
            assert hashlib.sha256((out / name).read_bytes()).hexdigest() == expected
        assert 'g_rx_dispatch_lock' in (out / 'esp_hosted.c').read_text()
        rpc = (out / 'esp_hosted_rpc.c').read_text()
        net = (out / 'c6net.c').read_text()
        assert 'g_wifi_event_lock' in rpc
        assert 'esp_hosted_rpc_wifi_scan_results' in rpc
        assert 'int esp_hosted_rpc_wifi_disconnect(void)' in rpc
        assert 'int c6net_disconnect(void)' in net
        assert 'int c6net_disconnect(void);' in (out / 'c6net.h').read_text()
        assert 'bool esp_hosted_rpc_is_waiting(void)' in rpc
        assert 'bool esp_hosted_rpc_is_waiting(void);' in (out / 'esp_hosted.h').read_text()
        assert net.count('#include "esp_hosted.h"') == 1
        assert 'volatile bool' not in net
        assert 'while (!esp_hosted_rpc_is_waiting() && esp_hosted_poll() > 0)' in net
        assert (out / 'rpc_mailbox.h').read_bytes() == (
            Path(__file__).resolve().parents[1] / 'c6/rpc_mailbox.h').read_bytes()
        saved = (out / 'manifest.json').read_bytes()
        try:
            prepare(args.source, out)
        except ValueError:
            pass
        else:
            raise AssertionError('Existing output was accepted')
        assert (out / 'manifest.json').read_bytes() == saved
        invalid = Path(tmp) / 'invalid'
        invalid.mkdir()
        for name in ('esp_hosted.c', 'esp_hosted.h', 'esp_hosted_rpc.c', 'c6net.c',
                     'c6net.h', 'rpc_mailbox.h'):
            (invalid / name).write_bytes((args.source / name).read_bytes())
        (invalid / 'c6net.c').write_text('unrecognized source')
        rejected = Path(tmp) / 'rejected'
        try:
            prepare(invalid, rejected)
        except ValueError:
            pass
        else:
            raise AssertionError('Invalid input was accepted')
        assert not rejected.exists()
    assert (args.source / 'c6net.c').read_bytes() == before
    print('PASS: composed adapters, hashes, no overwrite, preflight and unchanged source')


if __name__ == '__main__':
    main()
