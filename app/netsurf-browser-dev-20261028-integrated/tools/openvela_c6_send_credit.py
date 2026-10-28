"""Restore Hosted TX credits when the pinned C6 SDIO enqueue API fails.

Source-only adapter. Does not build, flash, reset, or access any device.
"""
from pathlib import Path
import argparse
import difflib
import hashlib

SOURCE_SHA256 = '867dcb9fa1f59741063140f12658b5f9df7db9cbd0ab734e8b724aa1a15216b5'
COMMIT = '0c4aa5f40ce0feebb454c1d671c39329c795b037'


def patch_source(data):
    if hashlib.sha256(data).hexdigest() != SOURCE_SHA256:
        raise ValueError('Source differs from the pinned ESP-Hosted 2.0.13 file')
    text = data.decode('utf-8')
    for log in ['sdio slave tx error', 'sdio slave transmit error']:
        needle = f'\tif (ret != ESP_OK) {{\n\t\tESP_LOGE(TAG , "{log}, ret : 0x%x\\r\\n", ret);'
        replacement = ('\tif (ret != ESP_OK) {\n'
                       '#if !SIMPLIFIED_SDIO_SLAVE\n'
                       '\t\t/* No queued buffer means no completion will return this credit. */\n'
                       '\t\txSemaphoreGive(sdio_send_queue_sem);\n'
                       '#endif\n'
                       f'\t\tESP_LOGE(TAG , "{log}, ret : 0x%x\\r\\n", ret);')
        if text.count(needle) != 1:
            raise ValueError(f'Unexpected enqueue failure block: {log}')
        text = text.replace(needle, replacement, 1)
    return text.encode('utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--diff', type=Path)
    args = parser.parse_args()
    before = args.source.read_bytes()
    after = patch_source(before)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.output.exists() and args.output.read_bytes() != after:
        raise ValueError('Output already exists with different content')
    args.output.write_bytes(after)
    if args.diff:
        args.diff.write_text(''.join(difflib.unified_diff(
            before.decode().splitlines(True), after.decode().splitlines(True),
            fromfile='a/slave/main/sdio_slave_api.c',
            tofile='b/slave/main/sdio_slave_api.c')), encoding='utf-8')
    print(hashlib.sha256(after).hexdigest())


if __name__ == '__main__':
    main()
