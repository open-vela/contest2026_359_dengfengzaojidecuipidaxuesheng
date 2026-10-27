#!/usr/bin/env python3
"""Connect the bounded Hosted 2.0.13 OTA path to a prepared NuttX tree."""

import hashlib
from pathlib import Path


def replace_once(text, old, new):
    if new in text:
        return text
    if text.count(old) != 1:
        raise ValueError('Unexpected prepared OTA anchor: ' + old[:100])
    return text.replace(old, new, 1)


def apply(apps):
    folder = Path(apps) / 'system/c6probe'
    rpc = folder / 'esp_hosted_rpc.c'
    text = rpc.read_text()
    required = ['rpc_transact_locked(', 'c6_rpc_next_uid(&g_rpc)',
                'pthread_mutex_unlock(&g_rpc_transaction_lock)']
    if not all(marker in text for marker in required):
        raise ValueError('OTA requires the serialized, unique-UID RPC owner')
    text = replace_once(text,
        'static pthread_mutex_t g_rpc_transaction_lock = PTHREAD_MUTEX_INITIALIZER;',
        'static pthread_mutex_t g_rpc_transaction_lock = PTHREAD_MUTEX_INITIALIZER;\n'
        'static bool g_c6_ota_attempted; /* Terminal until P4 restart. */')
    trial_guard = ('if (req == NULL || g_c6_ota_attempted ||\n'
                   '      c6_wifi_trial_pending())\n')
    if trial_guard not in text:
        text = replace_once(text, 'if (req == NULL)\n',
                            'if (req == NULL || g_c6_ota_attempted)\n')
    # Firmware chunks are not diagnostic payloads. Avoid 5800 hexdumps and
    # per-chunk text blocking the serial console during a real update.
    text = replace_once(text,
        '  printf("rpc: %s tx pb=%u tlv=%d\\n", label, (unsigned int)packed, tlvlen);',
        '  if (strcmp(label, "C6OTA") != 0)\n'
        '    printf("rpc: %s tx pb=%u tlv=%d\\n", label, (unsigned int)packed, tlvlen);')
    text = replace_once(text,
        '  if (strcmp(label, "WifiSetConfig") != 0)',
        '  if (strcmp(label, "C6OTA") == 0)\n'
        '    {\n'
        '      /* OTA logs contain progress/status only, never image bytes. */\n'
        '    }\n'
        '  else if (strcmp(label, "WifiSetConfig") != 0)')
    include = '#include "c6_ota_rpc.inc"'
    if include not in text:
        text = text.rstrip() + '\n\n' + include + '\n'
    if rpc.read_text() != text:
        rpc.write_text(text)
    make = folder / 'Makefile'
    text = make.read_text()
    text = replace_once(text, 'include $(APPDIR)/Application.mk',
        'CSRCS += c6_ota.c c6_ota_command.c\n\ninclude $(APPDIR)/Application.mk')
    if make.read_text() != text:
        make.write_text(text)
    return {'protocol': 'Hosted 2.0.13 begin/write/end; end commits',
            'chunk_bytes': 192, 'attempts_per_boot': 1,
            'source_sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                              for p in [rpc, make]}}


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apps', type=Path, required=True)
    print(apply(parser.parse_args().apps))
