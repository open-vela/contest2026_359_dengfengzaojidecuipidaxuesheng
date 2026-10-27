#!/usr/bin/env python3
"""Read ROM security state for the known P4/C6 board; never program eFuses."""

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import subprocess
import sys
import time


def parse_security(output):
    result = {}
    for field, label in [('secure_boot', 'Secure Boot'),
                         ('flash_encryption', 'Flash Encryption')]:
        match = re.search(r'^\s*' + label + r':\s*(Enabled|Disabled)\b',
                          output, re.I | re.M)
        result[field] = (match[1].lower() == 'enabled') if match else None
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--p4-port', default='COM4')
    parser.add_argument('--c6-port', default='COM5')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.p4_port == args.c6_port:
        parser.error('P4 and C6 ports must differ')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    result = {'read_at': datetime.now(timezone.utc).isoformat(),
              'efuses_programmed': False, 'flash_written': False,
              'p4_reset_after_probe': False, 'devices': {}}
    try:
        for chip, port, before, mac in [
                ('esp32c6', args.c6_port, 'default-reset', '98:88:e0:72:58:90'),
                ('esp32p4', args.p4_port, 'usb-reset', 'e8:f6:0a:e7:6f:f1')]:
            # P4 controls the companion reset GPIO. Query C6 before taking P4
            # into ROM, then restart P4 to establish a fresh radio session.
            # Run while the lazy BLE host is inactive (checked separately).
            command = [sys.executable, '-m', 'esptool', '--chip', chip,
                       '--port', port, '--before', before,
                       '--after', 'no-reset' if chip == 'esp32p4' else 'hard-reset',
                       '--no-stub', 'get-security-info']
            completed = subprocess.run(command, capture_output=True, timeout=40)
            raw = completed.stdout + completed.stderr
            (out / (chip + '.log')).write_bytes(raw)
            text = raw.decode('utf-8', errors='replace')
            if completed.returncode:
                raise RuntimeError(chip + ' ROM security read failed')
            if mac not in text.lower():
                raise RuntimeError(chip + ' expected board MAC not confirmed')
            state = parse_security(text)
            state['expected_mac_confirmed'] = True
            result['devices'][chip] = state
            print(chip, json.dumps(state), flush=True)
    finally:
        # Restore normal boot even if a security field cannot be queried.
        import serial
        from esptool.reset import USBJTAGSerialReset, HardReset
        com = serial.Serial(port=None, baudrate=115200, timeout=.1)
        com.dtr = False
        com.rts = False
        com.port = args.p4_port
        with com:
            USBJTAGSerialReset(com)()
            HardReset(com, uses_usb=True)()
            boot = bytearray()
            until = time.monotonic() + 30
            while time.monotonic() < until:
                boot.extend(com.read(8192))
        (out / 'p4-restored-boot.log').write_bytes(boot)
        result['p4_reset_after_probe'] = b'nsh>' in boot
        (out / 'SECURITY.json').write_text(json.dumps(result, indent=2) + '\n')
    if not result['p4_reset_after_probe']:
        raise RuntimeError('P4 normal boot not confirmed after security read')


if __name__ == '__main__':
    main()
