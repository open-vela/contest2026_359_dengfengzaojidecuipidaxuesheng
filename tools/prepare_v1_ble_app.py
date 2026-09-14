"""Generate an isolated BLE app and transport patch; never alter source trees."""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
from prepare_v1_ble_claim import adapt


def prepare(tree, output, *, v3=False):
    tree, output = tree.resolve(), output.resolve()
    if output.exists() or tree == output or tree in output.parents:
        raise ValueError('Fresh independent output required')
    config = set((tree / 'nuttx/.config').read_text().splitlines())
    is_v1 = 'CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y' in config
    if v3:
        if is_v1 or 'CONFIG_ARCH_CHIP_ESP32P4=y' not in config:
            raise ValueError('v3 ESP32-P4 configuration required')
    elif not is_v1:
        raise ValueError('v1 only; use --v3-experimental for v3 staging')
    source = tree / 'apps/system/c6probe/esp_hosted.c'
    before = source.read_text()
    after = adapt(before, v3=v3)
    files = {}
    if v3:
        files['ble_socket_register.c'] = (Path(__file__).parent / 'c6/ble_socket_register.c').read_bytes()
    for name in ('ble_main.c', 'ble_driver.c', 'ble_driver.h', 'ble_hosted.c',
                 'ble_hosted.h', 'ble_register.c', 'ble_register.h', 'ble_h4.h'):
        files[name] = (Path(__file__).parent / 'c6' / name).read_bytes()
    files['Kconfig'] = b'''config SYSTEM_C6BLE
\tbool "Experimental v1 C6 BLE HCI transport"
\tdefault n
\tdepends on ESP32P4_SELECTS_REV_LESS_V3 && SYSTEM_C6PROBE=y
\tdepends on UART_BTH4 && !DISABLE_PTHREAD
\t---help---
\t\tRegister HCI using c6ble register. Requires the HCI ownership patch.
\t\tHosted must already be prepared before opening the device.
'''
    files['Make.defs'] = b'''ifeq ($(CONFIG_SYSTEM_C6BLE),y)
CONFIGURED_APPS += $(APPDIR)/system/c6ble
endif
'''
    files['Makefile'] = b'''include $(APPDIR)/Make.defs
PROGNAME = c6ble
PRIORITY = 100
STACKSIZE = 8192
MODULE = $(CONFIG_SYSTEM_C6BLE)
CFLAGS += -I$(APPDIR)/system/c6probe
MAINSRC = ble_main.c
CSRCS = ble_driver.c ble_hosted.c ble_register.c
include $(APPDIR)/Application.mk
'''
    if v3:
        files['Kconfig'] = b'''config SYSTEM_C6BLE_V3_EXPERIMENTAL
\tbool "Experimental v3 C6 BLE HCI transport"
\tdefault n
\tdepends on ARCH_CHIP_ESP32P4 && !ESP32P4_SELECTS_REV_LESS_V3
\tdepends on SYSTEM_C6PROBE=y && NET_BLUETOOTH && !DISABLE_PTHREAD
\tdepends on !WIRELESS_BLUETOOTH_HOST
\t---help---
\t\tRequires the matching HCI ownership patch. Register with c6ble register.
\t\tPrepare Hosted before opening HCI; do not reset it while HCI is open.
\t\tRegisters raw HCI networking for the Apache NimBLE socket transport.
\t\tThis transport alone does not provide a BLE host or GATT services.
'''
        for name in ('Make.defs', 'Makefile'):
            files[name] = files[name].replace(b'CONFIG_SYSTEM_C6BLE',
                                              b'CONFIG_SYSTEM_C6BLE_V3_EXPERIMENTAL')
        files['Makefile'] = files['Makefile'].replace(b'ble_register.c', b'ble_socket_register.c')
    output.mkdir(parents=True)
    app = output / 'c6ble'
    app.mkdir()
    for name, data in files.items():
        (app / name).write_bytes(data)
    patch = ''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
        fromfile='a/system/c6probe/esp_hosted.c', tofile='b/system/c6probe/esp_hosted.c'))
    (output / 'hci-claim.patch').write_text(patch)
    (output / 'manifest.json').write_text(json.dumps({
        'status': 'uninstalled-unlinked-' + ('v3' if v3 else 'v1') + '-candidate',
        'transport_input': hashlib.sha256(source.read_bytes()).hexdigest(),
        'files': {n: hashlib.sha256(d).hexdigest() for n, d in files.items()},
        'patch_sha256': hashlib.sha256(patch.encode()).hexdigest(),
    }, indent=2) + '\n')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('tree', type=Path)
    p.add_argument('output', type=Path)
    p.add_argument('--v3-experimental', action='store_true')
    a = p.parse_args()
    prepare(a.tree, a.output, v3=a.v3_experimental)
