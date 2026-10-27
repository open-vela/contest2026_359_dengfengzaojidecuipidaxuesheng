#!/usr/bin/env python3
"""Prepare an eight-link controller-only C6 source copy; no device actions."""
from pathlib import Path
import argparse
import hashlib
import json
import re
from prepare_c6_hci_frames import patch_source as patch_hci_source

BEGIN = '/* BEGIN OPENVELA C6 EIGHT CONNECTIONS */'
END = '/* END OPENVELA C6 EIGHT CONNECTIONS */'
ANCHOR = 'esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();'
GUARD = '''/* BEGIN OPENVELA C6 EIGHT CONNECTIONS */
#if !defined(CONFIG_IDF_TARGET_ESP32C6) || !defined(CONFIG_BT_CONTROLLER_ONLY) || \\
    !defined(CONFIG_BT_CONTROLLER_ENABLED) || !defined(CONFIG_BT_ENABLED)
#  error Eight-link companion requires the ESP32-C6 controller-only configuration
#endif
#if defined(CONFIG_BT_NIMBLE_ENABLED) || defined(CONFIG_BT_BLUEDROID_ENABLED)
#  error The BLE Host belongs to P4, not the C6 companion
#endif
_Static_assert(CONFIG_BT_LE_MAX_CONNECTIONS == 8, "C6 must configure eight connections");
_Static_assert(DEFAULT_BT_LE_MAX_CONNECTIONS == 8, "SDK controller default must use eight connections");
/* Retained in the final ELF for an offline capacity check. */
const uint32_t openvela_c6_ble_capacity __attribute__((used, retain)) = CONFIG_BT_LE_MAX_CONNECTIONS;
/* END OPENVELA C6 EIGHT CONNECTIONS */
'''
CHECK = '''
	/* Validate the actual SDK initializer before passing it to the controller. */
	ESP_ERROR_CHECK(bt_cfg.nimble_max_connections == openvela_c6_ble_capacity ?
	                ESP_OK : ESP_ERR_INVALID_STATE);
	ESP_LOGI(TAG, "BLE controller capacity=%u", (unsigned)bt_cfg.nimble_max_connections);'''


def patch_source(text):
    if BEGIN in text:
        if text.count(GUARD) != 1 or text.count(ANCHOR + CHECK) != 1:
            raise ValueError('Unexpected existing capacity guard')
        return text
    entry = 'esp_err_t initialise_bluetooth(void)'
    if text.count(entry) != 1 or text.count(ANCHOR) != 1:
        raise ValueError('Pinned Hosted Bluetooth initialization anchor missing')
    return text.replace(entry, GUARD + '\n' + entry, 1).replace(ANCHOR, ANCHOR + CHECK, 1)


def configure(text):
    for line in ['CONFIG_IDF_TARGET="esp32c6"', 'CONFIG_BT_ENABLED=y',
                 'CONFIG_BT_CONTROLLER_ENABLED=y', 'CONFIG_BT_CONTROLLER_ONLY=y']:
        if line not in text.splitlines():
            raise ValueError('Missing pinned configuration: ' + line)
    for name in ['BT_NIMBLE_ENABLED', 'BT_BLUEDROID_ENABLED']:
        if 'CONFIG_' + name + '=y' in text.splitlines():
            raise ValueError('C6 must not own a second BLE Host')
    matches = re.findall(r'^CONFIG_BT_LE_MAX_CONNECTIONS=(\d+)$', text, re.M)
    if len(matches) != 1 or int(matches[0]) not in (3, 8):
        raise ValueError('Expected one known three/eight-link capacity setting')
    return re.sub(r'^CONFIG_BT_LE_MAX_CONNECTIONS=\d+$',
                  'CONFIG_BT_LE_MAX_CONNECTIONS=8', text, flags=re.M)


def apply(project, config):
    project, config = Path(project), Path(config)
    source = project / 'main/slave_bt.c'
    original, old_config = source.read_text(), config.read_text()
    patched, updated = patch_hci_source(patch_source(original)), configure(old_config)
    # Complete validation before either write.
    source.write_text(patched)
    config.write_text(updated)
    sha = lambda data: hashlib.sha256(data.encode()).hexdigest()
    return {'connections': 8, 'hci_frame_length_guard':True, 'source_before_sha256': sha(original),
            'source_sha256': sha(patched), 'config_before_sha256': sha(old_config),
            'config_sha256': sha(updated), 'device_actions': False}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--project', type=Path, required=True)
    p.add_argument('--config', type=Path, required=True)
    a = p.parse_args()
    print(json.dumps(apply(a.project, a.config), indent=2))
