#!/usr/bin/env python3
"""Regression for prepared trees that depended on older author workspaces."""
from pathlib import Path
import tempfile
import unittest
from build_bilibili_v3 import relocate_kconfig_sources


class Relocation(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)/'new workspace'
        (self.root/'apps/audioutils/alsa-lib').mkdir(parents=True)
        (self.root/'apps/audioutils/alsa-lib/Kconfig').write_text('config ALSA\n')
        (self.root/'nuttx/arch').mkdir(parents=True)
        (self.root/'nuttx/arch/Kconfig').write_text('config ARCH\n')
        self.config = self.root/'apps/audioutils/Kconfig'

    def test_multiple_legacy_roots_are_local_and_idempotent(self):
        self.config.write_text('source "/author/assistant-old/apps/audioutils/alsa-lib/Kconfig"\n'
                               'source "/other/apps/old-build/nuttx/arch/Kconfig"\n')
        relocate_kconfig_sources(self.root, '/baseline')
        text = self.config.read_text()
        self.assertIn((self.root/'apps/audioutils/alsa-lib/Kconfig').as_posix(), text)
        self.assertIn((self.root/'nuttx/arch/Kconfig').as_posix(), text)
        relocate_kconfig_sources(self.root, '/baseline')
        self.assertEqual(text, self.config.read_text())

    def test_hidden_generated_config_and_variables(self):
        self.config = self.root/'apps/.Kconfig'
        self.config.write_text('source "/old/apps/audioutils/alsa-lib/Kconfig"\nsource "$APPSDIR/Kconfig"\n')
        relocate_kconfig_sources(self.root, '/baseline')
        self.assertNotIn('/old/', self.config.read_text())
        self.assertIn('source "$APPSDIR/Kconfig"', self.config.read_text())

    def test_missing_required_input_does_not_use_old_tree(self):
        self.config.write_text('source "/old/apps/missing/Kconfig"\n')
        with self.assertRaises(FileNotFoundError):
            relocate_kconfig_sources(self.root, '/baseline')

    def test_optional_missing_input_is_still_relocated(self):
        self.config.write_text('osource "/old/apps/missing/Kconfig"\n')
        relocate_kconfig_sources(self.root, '/baseline')
        self.assertIn((self.root/'apps/missing/Kconfig').as_posix(), self.config.read_text())

    def test_parent_traversal_is_rejected(self):
        self.config.write_text('source "/old/apps/../private/Kconfig"\n')
        with self.assertRaises(ValueError):
            relocate_kconfig_sources(self.root, '/baseline')


if __name__ == '__main__':
    unittest.main()
