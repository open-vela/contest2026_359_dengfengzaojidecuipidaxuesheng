#!/usr/bin/env python3
"""Audit the host's real make flags, syscfg mapping, and connection pool."""
import argparse
import json
from pathlib import Path
import re
import subprocess


def audit(apps, nuttx, compiler, output, elf=None):
    apps, nuttx, output = map(lambda p: Path(p).resolve(), (apps, nuttx, output))
    compiler = Path(compiler).resolve()
    output.mkdir(parents=True, exist_ok=True)
    port = apps/'wireless/bluetooth/nimble'
    target = 'espclaw-print-nimble-cflags'
    flags = subprocess.check_output([
        'make', '--no-print-directory', '-s', '-C', str(port),
        'APPDIR='+str(apps), 'TOPDIR='+str(nuttx),
        'CROSSDEV='+str(compiler).removesuffix('gcc'),
        '--eval='+target+": ; @printf '%s\\n' $(CFLAGS)", target,
    ], text=True).splitlines()
    (output/'cflags.json').write_text(json.dumps(flags, indent=2)+'\n')
    config = (nuttx/'include/nuttx/config.h').read_text()
    match = re.search(r'^#define CONFIG_NIMBLE_BLE_MAX_CONN\s+(\d+)\s*$', config, re.M)
    if not match:
        raise RuntimeError('Generated NimBLE connection configuration missing')
    requested = int(match[1])
    source = output/'capacity.c'
    source.write_text('''#include <nuttx/config.h>
#include "syscfg/syscfg.h"
#include "ble_hs_priv.h"
#include "glass_ble.h"
#if MYNEWT_VAL_BLE_MAX_CONNECTIONS != CONFIG_NIMBLE_BLE_MAX_CONN
#error NimBLE connection configuration was not applied
#endif
unsigned char audit_connections[MYNEWT_VAL(BLE_MAX_CONNECTIONS)];
unsigned char audit_central_sessions[GLASS_BLE_CENTRAL_LIMIT];
os_membuf_t audit_pool[OS_MEMPOOL_SIZE(MYNEWT_VAL(BLE_MAX_CONNECTIONS),
                                    sizeof(struct ble_hs_conn))];
''')
    obj = output/'capacity.o'
    host_src = port/'mynewt-nimble/nimble/host/src'
    subprocess.run([str(compiler), *flags, '-I'+str(host_src), '-I'+str(port),
                    '-c', str(source), '-o', str(obj)], check=True)
    nm = compiler.with_name(compiler.name.removesuffix('gcc')+'nm')

    def symbols(path):
        text = subprocess.check_output([str(nm), '-S', str(path)], text=True)
        return {name: int(size,16) for _,size,_,name in re.findall(
            r'^([0-9a-f]+)\s+([0-9a-f]+)\s+(\S)\s+(\S+)$',text,re.M)}

    probe = symbols(obj)
    if probe.get('audit_connections') != requested:
        raise RuntimeError('Compiled NimBLE capacity disagrees with generated configuration')
    result = {'configured_connections':requested,
              'effective_connections':probe['audit_connections'],
              'ordinary_session_capacity':probe['audit_central_sessions'],
              'expected_connection_pool_bytes':probe['audit_pool'],
              'linked_connection_pool_verified':False}
    if elf is not None:
        pool = symbols(elf).get('ble_hs_conn_elem_mem')
        if pool != probe['audit_pool']:
            raise RuntimeError(f'Linked NimBLE pool mismatch: {pool} != {probe["audit_pool"]}')
        result.update(linked_connection_pool_verified=True,
                      linked_connection_pool_bytes=pool)
    (output/'VERIFICATION.json').write_text(json.dumps(result,indent=2)+'\n')
    return result


if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--apps',type=Path,required=True)
    p.add_argument('--nuttx',type=Path,required=True)
    p.add_argument('--compiler',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--elf',type=Path)
    a=p.parse_args()
    print(json.dumps(audit(a.apps,a.nuttx,a.compiler,a.output,a.elf),indent=2))
