#!/usr/bin/env python3
"""Repair the pinned NimBLE RAM-store record compaction length."""
import hashlib
from pathlib import Path


def apply(apps: Path):
    path = apps / 'wireless/bluetooth/nimble/mynewt-nimble/nimble/host/store/ram/src/ble_store_ram.c'
    text = path.read_text()
    old = 'memmove(dst, src, move_count);'
    new = 'memmove(dst, src, (size_t)move_count * (size_t)value_size);'
    if text.count(old) == 1 and new not in text:
        text = text.replace(old, new)
        path.write_text(text)
    elif text.count(new) != 1 or old in text:
        raise RuntimeError('Unrecognized NimBLE RAM-store compaction; inspect SDK before building')
    return {'path': str(path.relative_to(apps)),
            'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
            'record_compaction_bytes': True}
