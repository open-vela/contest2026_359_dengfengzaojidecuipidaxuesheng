#!/usr/bin/env python3
"""Mint a stable-per-image, fresh-per-build BLE HID test identity."""
import hashlib
import json
from pathlib import Path
import secrets


def create(apps: Path, output: Path):
    output.mkdir(parents=True, exist_ok=True)
    history = output / 'ble-identity-history'
    history.mkdir(exist_ok=True)
    prior_names = {json.loads(p.read_text())['name'] for p in history.glob('*.json')}
    while True:
        address = bytearray(secrets.token_bytes(6))
        address[5] |= 0xc0
        payload = int.from_bytes(address, 'little') & ((1 << 46) - 1)
        if payload in (0, (1 << 46) - 1):
            continue
        token = bytes(reversed(address)).hex().upper()
        label = 'OpenVela Mouse-' + token[-8:]
        if not (history / (token + '.json')).exists() and label not in prior_names:
            break
    assert len(label.encode()) <= 29  # Legacy scan response, AD length/type.
    stem = 'ble_' + token.lower()
    info = {
        'address': ':'.join(f'{b:02X}' for b in reversed(address)),
        'address_little_endian': list(address),
        'address_type': 'static-random',
        'name': label,
        'store_path': '/data/config/' + stem + '.bin',
        'store_temp': '/data/config/.' + stem + '.tmp',
        'purpose_path': '/data/config/bh_' + token.lower() + '.bin',
        'purpose_temp': '/data/config/.bh_' + token.lower() + '.tmp',
    }
    header = ('/* Generated per candidate. Stable across this image\'s reboots. */\n'
              '#pragma once\n#define GLASS_BLE_CANDIDATE_ADDRESS {' +
              ', '.join(f'0x{b:02x}' for b in address) + '}\n')
    for macro, key in [('NAME', 'name'), ('ADDRESS_TEXT', 'address'),
                       ('STORE_PATH', 'store_path'), ('STORE_TEMP', 'store_temp'),
                       ('PURPOSE_PATH', 'purpose_path'), ('PURPOSE_TEMP', 'purpose_temp')]:
        header += '#define GLASS_BLE_CANDIDATE_' + macro + ' ' + json.dumps(info[key]) + '\n'
    target = apps / 'wireless/bluetooth/nimble/glass_ble_candidate.h'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(header)
    info['header_sha256'] = hashlib.sha256(target.read_bytes()).hexdigest()
    (output / 'BLE-IDENTITY.json').write_text(json.dumps(info, indent=2) + '\n')
    (history / (token + '.json')).write_text(json.dumps(info, indent=2) + '\n')
    return info
