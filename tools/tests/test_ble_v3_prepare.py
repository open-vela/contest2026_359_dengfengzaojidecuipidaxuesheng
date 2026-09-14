"""Validate opt-in v3 staging without changing an active firmware tree."""
import json
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from prepare_v1_ble_app import prepare


def main():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        tree = root / 'tree'
        (tree / 'nuttx').mkdir(parents=True)
        source = tree / 'apps/system/c6probe/esp_hosted.c'
        source.parent.mkdir(parents=True)
        source.write_text('''static pthread_mutex_t g_rx_dispatch_lock;
int esp_hosted_register(uint8_t if_type, esp_hosted_rx_cb_t cb,
                       void *arg)
{
  g_hosted.rx_cb[if_type]  = cb;
}
''')
        config = tree / 'nuttx/.config'
        config.write_text('CONFIG_ARCH_CHIP_ESP32P4=y\n')
        original = source.read_bytes()
        output = root / 'candidate'
        prepare(tree, output, v3=True)
        patch = (output / 'hci-claim.patch').read_text()
        assert patch.count('#ifdef CONFIG_SYSTEM_C6BLE_V3_EXPERIMENTAL') == 2
        assert 'CONFIG_ESP32P4_SELECTS_REV_LESS_V3' not in patch
        kconfig = (output / 'c6ble/Kconfig').read_text()
        assert '\tdefault n' in kconfig and 'NET_BLUETOOTH' in kconfig
        assert '!WIRELESS_BLUETOOTH_HOST' in kconfig
        assert 'ble_socket_register.c' in (output / 'c6ble/Makefile').read_text()
        assert 'CONFIG_SYSTEM_C6BLE_V3_EXPERIMENTAL' in (output / 'c6ble/Makefile').read_text()
        assert json.loads((output / 'manifest.json').read_text())['status'] == 'uninstalled-unlinked-v3-candidate'
        assert source.read_bytes() == original
        try:
            prepare(tree, output, v3=True)
        except ValueError:
            pass
        else:
            raise AssertionError('Existing output accepted')
        config.write_text('CONFIG_ARCH_CHIP_ESP32P4=y\nCONFIG_ESP32P4_SELECTS_REV_LESS_V3=y\n')
        rejected = root / 'rejected'
        try:
            prepare(tree, rejected, v3=True)
        except ValueError:
            pass
        else:
            raise AssertionError('v1 accepted as v3')
        assert not rejected.exists()
    print('PASS: v3 opt-in, matching patch, unchanged source, board and overwrite rejection')


if __name__ == '__main__':
    main()
