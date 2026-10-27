#!/usr/bin/env python3
"""Verify an existing release and quantify a data-preserving dual-slot layout.

This is a capacity preflight, not a partition writer or a bootloader port.
It never treats compression, a download, or an SD file as proven rollback.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re


def round_up(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def plan(metadata, image, config, flash_bytes=16 * 1024 * 1024):
    digest = hashlib.sha256(image).hexdigest()
    if digest != metadata['sha256']['nuttx.bin']:
        raise ValueError('Release image does not match build metadata')
    offset = int(metadata['flash_offset'], 0)
    data_offset = int(metadata['data_offset'], 0)
    data_size = int(metadata['data_size'], 0)
    if offset != 0x2000 or data_offset + data_size != flash_bytes:
        raise ValueError('Unknown board layout; inspect the actual device')
    for key, expected in [('OFFSET', data_offset), ('SIZE', data_size)]:
        match = re.search(r'^CONFIG_ESPRESSIF_STORAGE_MTD_' + key + r'=(\S+)$',
                          config, re.M)
        if not match or int(match[1], 0) != expected:
            raise ValueError('Resolved configuration disagrees with data layout')
    if offset + len(image) > data_offset:
        raise ValueError('Existing release overlaps user data')
    # Explicit planning assumptions, not deployable MCUboot configuration.
    sector = 0x1000
    boot_reserved = 0x20000
    scratch_reserved = 0x10000
    image_overhead_reserved = 0x2000
    slot = ((data_offset - boot_reserved - scratch_reserved) // (2 * sector)) * sector
    max_payload = max(0, slot - image_overhead_reserved)
    required_slot = round_up(len(image) + image_overhead_reserved, sector)
    return {
        'image_sha256': digest, 'image_bytes': len(image),
        'flash_bytes': flash_bytes, 'data_offset': hex(data_offset),
        'data_bytes': data_size, 'data_preserved': True,
        'current_boot': 'simple-boot' if 'CONFIG_ESPRESSIF_SIMPLE_BOOT=y\n' in config
                        else 'other-boot-method',
        'assumptions': {'boot_reserved': boot_reserved,
                        'scratch_reserved': scratch_reserved,
                        'per_slot_header_signature_trailer_reserved': image_overhead_reserved,
                        'sector_bytes': sector},
        'planned_slot_bytes': slot, 'maximum_image_bytes': max_payload,
        'shrink_at_least_bytes': max(0, len(image) - max_payload),
        'two_current_images_fit': len(image) <= max_payload,
        'minimum_flash_bytes_for_two_current_images_and_same_data':
            boot_reserved + scratch_reserved + 2 * required_slot + data_size,
        'bootloader_xip_compatibility_verified': False,
        'safe_to_deploy_this_plan': False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--firmware', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--require-fit', action='store_true')
    args = parser.parse_args()
    folder = args.firmware
    metadata = json.loads((folder / 'BUILD-METADATA.json').read_text())
    result = plan(metadata, (folder / 'nuttx.bin').read_bytes(),
                  (folder / 'resolved.config').read_text())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    if args.require_fit and not result['two_current_images_fit']:
        raise SystemExit(2)


if __name__ == '__main__':
    main()
