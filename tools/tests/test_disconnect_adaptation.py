"""Verify disconnect composition and rejection before any output is written."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from adapt_c6_disconnect import adapt_sources


class DisconnectAdaptation(unittest.TestCase):
    def setUp(self):
        self.sources = {
            'esp_hosted_rpc.c': 'int esp_hosted_rpc_wifi_connect(void) {}\n',
            'c6net.c': 'int c6net_prepare(void) {}\n',
            'esp_hosted.h': 'int esp_hosted_rpc_wifi_connect(void);\n',
            'c6net.h': 'int c6net_prepare(void);\n',
        }

    def test_complete_and_idempotent(self):
        result = adapt_sources(self.sources)
        self.assertEqual(result, adapt_sources(result))
        self.assertIn('int c6net_disconnect(void);', result['c6net.h'])
        self.assertIn('RPC_ID__Req_WifiDisconnect', result['esp_hosted_rpc.c'])
        self.assertIn('g_c6net_connect_lock', result['c6net.c'])
        self.assertNotIn('WifiDisconnect', self.sources['esp_hosted_rpc.c'])

    def test_missing_or_duplicate_anchor(self):
        for replacement in ('', self.sources['c6net.c'] * 2):
            sources = dict(self.sources, **{'c6net.c': replacement})
            with self.assertRaises(ValueError):
                adapt_sources(sources)

    def test_unknown_existing_implementation(self):
        self.sources['c6net.c'] += 'int c6net_disconnect(void) { return 0; }'
        with self.assertRaises(ValueError):
            adapt_sources(self.sources)


if __name__ == '__main__':
    unittest.main()
