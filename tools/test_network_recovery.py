#!/usr/bin/env python3
"""Fault-inject the production recovery service and persistent network worker."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sharing', action='store_true')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    desktop = repo / 'app/espdl-quickapp/overlay/apps/system/desktop'
    c6 = desktop.parent / 'c6probe'
    include = out / 'include'
    (include / 'nuttx').mkdir(parents=True, exist_ok=True)
    (include / 'netutils').mkdir(parents=True, exist_ok=True)
    (include / 'nuttx/config.h').write_text('#define CONFIG_NET 1\n'
        '#define CONFIG_SYSTEM_C6PROBE 1\n')
    (include / 'nuttx/compiler.h').write_text('#define FAR\n')
    if args.sharing:
        with (include/'nuttx/config.h').open('a') as config:
            config.write('#define CONFIG_NET_NAT44 1\n')
    (include / 'cJSON.h').write_text('typedef struct cJSON cJSON;\n')
    (include / 'netutils/dhcpd_runtime.h').write_bytes((
        repo / 'app/espdl-quickapp/overlay/apps/include/netutils/dhcpd_runtime.h').read_bytes())
    (include / 'netutils/dhcpd.h').write_text('''
#include <stdint.h>
int dhcpd_set_startip(uint32_t);
int dhcpd_set_netmask(uint32_t);
int dhcpd_set_routerip(uint32_t);
int dhcpd_start(const char *);
int dhcpd_stop(void);
''')
    (include / 'netutils/netlib.h').write_text('''
#include <netinet/in.h>
int netlib_get_ipv4addr(const char *, struct in_addr *);
int netlib_get_ipv4netmask(const char *, struct in_addr *);
int netlib_set_ipv4addr(const char *, const struct in_addr *);
int netlib_set_ipv4netmask(const char *, const struct in_addr *);
int netlib_set_dripv4addr(const char *, const struct in_addr *);
''')
    sources = [desktop / 'glass_network_recovery.c',
               desktop / 'glass_network_recovery.h',
               repo / 'tools/c6/desktop_worker.c',
               repo / 'tools/c6/desktop_worker.h']
    frozen = out / 'source'
    frozen.mkdir(exist_ok=True)
    inputs = {p: p.read_bytes() for p in sources}
    for path, data in inputs.items():
        (frozen / path.name).write_bytes(data)
    (out / 'recovery-under-test.c').write_text('#include "' +
        str(frozen / sources[0].name).replace('\\', '/') + '"\n')
    exe = out / 'network-recovery-host'
    command = ['gcc', '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
        '-Wno-sign-compare', '-fsanitize=address,undefined',
        '-fno-omit-frame-pointer', '-pthread',
        '-DCONFIG_SYSTEM_C6_DESKTOP_STACKSIZE=65536',
        '-I' + str(include), '-I' + str(out), '-I' + str(desktop),
        '-I' + str(c6), '-I' + str(repo / 'tools/c6'),
        str(repo / 'app/espdl-quickapp/tests/network_recovery_host.c'),
        str(frozen / 'desktop_worker.c'), '-o', str(exe)]
    subprocess.run(command, check=True)
    subprocess.run([str(exe)], check=True, timeout=90)
    # HTTP schemas have no credential accessor; status snapshot has no secret.
    http = (desktop / 'glass_portal_http.c').read_text()
    assert 'glass_network_recovery_credentials' not in http
    reply = http[http.index('static void network_reply('):
                 http.index('static int network_connect(')]
    assert 'password' not in reply and 'secret' not in reply
    manifest = {'status': 'host-only-no-hardware',
        'host_stack_override': 65536,
        'reason': 'ASan host runtime; target stack is unchanged',
        'sources': {str(p.relative_to(repo)): hashlib.sha256(data).hexdigest()
                    for p, data in inputs.items()}}
    (out / 'RESULT.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('NETWORK_RECOVERY_HTTP_OK: no ephemeral AP password accessor in HTTP status/config source')


if __name__ == '__main__':
    main()
