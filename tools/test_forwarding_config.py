#!/usr/bin/env python3
import re
import unittest
from build_bilibili_v3 import forwarding_config


class ForwardingConfigTest(unittest.TestCase):
    base='CONFIG_IOB_NBUFFERS=96\nCONFIG_NET_FORWARD_NSTRUCT=4\nCONFIG_NET_FORWARD_ALLOC_STRUCT=0\n# CONFIG_NET_STATISTICS is not set\nCONFIG_NET_NAT_HASH_BITS=8\nCONFIG_NET_PREALLOC_DEVIF_CALLBACKS=20\n'

    def test_accepted_default_is_unchanged_and_explicit(self):
        self.assertEqual(forwarding_config(self.base),self.base)

    def test_diagnostic_is_bounded_idempotent_and_preserves_other_options(self):
        updated=forwarding_config(self.base,16,True)
        self.assertEqual(forwarding_config(updated,16,True),updated)
        self.assertIn('CONFIG_NET_FORWARD_NSTRUCT=16\n',updated)
        self.assertIn('CONFIG_NET_FORWARD_ALLOC_STRUCT=0\n',updated)
        self.assertIn('CONFIG_NET_STATISTICS=y\n',updated)
        self.assertIn('CONFIG_IOB_NBUFFERS=96\n',updated)
        self.assertIn('CONFIG_NET_NAT_HASH_BITS=8\n',updated)
        self.assertEqual(forwarding_config(updated),self.base.replace('CALLBACKS=20','CALLBACKS=32'))
        self.assertIn('CONFIG_NET_PREALLOC_DEVIF_CALLBACKS=32\n',updated)

    def test_callbacks_reserved_without_shrinking_existing_pool(self):
        for slots in [4,8,16]:
            fixed=forwarding_config(self.base.replace('CALLBACKS=20','CALLBACKS=16'),slots)
            count=int(re.search(r'CALLBACKS=(\d+)',fixed)[1])
            self.assertGreaterEqual(count-slots,16)
        self.assertIn('CALLBACKS=64',forwarding_config(self.base.replace('CALLBACKS=20','CALLBACKS=64'),16))
        with self.assertRaises(ValueError):
            forwarding_config(re.sub(r'^CONFIG_NET_PREALLOC_DEVIF_CALLBACKS=.*\n','',self.base,flags=re.M),16)

    def test_no_unbounded_or_iob_exhausting_pool(self):
        for size in [0,1,12,32,96,1000000]:
            with self.assertRaises(ValueError):forwarding_config(self.base,size,True)
        with self.assertRaises(ValueError):
            forwarding_config(self.base.replace('IOB_NBUFFERS=96','IOB_NBUFFERS=64'),16,True)
        with self.assertRaises(ValueError):
            forwarding_config(re.sub(r'^CONFIG_IOB_NBUFFERS=.*\n','',self.base,flags=re.M),16,True)


if __name__=='__main__':unittest.main(verbosity=2)
