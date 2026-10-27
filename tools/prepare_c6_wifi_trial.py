#!/usr/bin/env python3
"""Add RAM-only STA trials to the prepared, serialized Hosted RPC owner."""

import hashlib
from pathlib import Path


def once(text, old, new):
    if new in text:
        return text
    if text.count(old) != 1:
        raise ValueError('Unknown WiFi trial RPC anchor: ' + old[:100])
    return text.replace(old, new, 1)


def apply(apps):
    folder = Path(apps) / 'system/c6probe'
    rpc = folder / 'esp_hosted_rpc.c'
    source = rpc.read_text()
    required = ['static FAR Rpc *rpc_transact_locked(',
                'c6_rpc_next_uid(&g_rpc)',
                'static bool g_c6_ota_attempted;',
                '#include "c6_ota_rpc.inc"']
    if not all(marker in source for marker in required):
        raise ValueError('WiFi trials require the prepared OTA/RPC owner')
    updated = once(source, '#include "esp_hosted.h"',
                   '#include "esp_hosted.h"\n#include "c6_wifi_trial.h"')
    updated = once(updated, 'if (req == NULL || g_c6_ota_attempted)\n',
                   'if (req == NULL || g_c6_ota_attempted ||\n'
                   '      c6_wifi_trial_pending())\n')
    include = '#include "c6_wifi_trial_rpc.inc"'
    if include not in updated:
        updated = updated.rstrip() + '\n\n' + include + '\n'
    if updated.count(include) != 1:
        raise ValueError('Repeated WiFi trial RPC fragment')
    if updated != source:
        rpc.write_text(updated)
    return {'trial_storage': 'RAM; FLASH only after verified DHCP commit',
            'old_sta_config': 'owned complete protobuf snapshot',
            'ordinary_rpc_excluded_while_pending': True,
            'source_sha256': hashlib.sha256(rpc.read_bytes()).hexdigest()}


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apps', type=Path, required=True)
    print(apply(parser.parse_args().apps))
