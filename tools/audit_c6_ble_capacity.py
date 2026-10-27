#!/usr/bin/env python3
"""Verify the C6 ELF, real SDK ABI and resolved configuration without a device."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import struct
import subprocess

from prepare_c6_ble_capacity import GUARD
from prepare_c6_hci_frames import BEGIN as HCI_BEGIN


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def symbols(path):
    data=Path(path).read_bytes()
    if data[:7]!=b'\x7fELF\x01\x01\x01' or struct.unpack_from('<H',data,18)[0]!=243:
        raise ValueError('Expected little-endian ELF32 RISC-V')
    offset=struct.unpack_from('<I',data,32)[0]
    width,count=struct.unpack_from('<HH',data,46)
    sections=[struct.unpack_from('<10I',data,offset+i*width) for i in range(count)]
    found={}
    for section in sections:
        if section[1]!=2:continue
        strings=sections[section[6]]
        table=data[strings[4]:strings[4]+strings[5]]
        for i in range(section[4],section[4]+section[5],section[9]):
            name,value,size,info,other,index=struct.unpack_from('<IIIBBH',data,i)
            if not name or index>=len(sections):continue
            text=table[name:table.index(b'\0',name)].decode()
            area=sections[index]
            start=area[4]+value-area[3]
            raw=data[start:start+size] if area[1]!=8 else b''
            found[text]={'address':value,'size':size,'data':raw}
    return found


def config(path):
    values={}
    for line in Path(path).read_text().splitlines():
        if line.startswith('CONFIG_') and '=' in line:
            name,value=line.split('=',1);values[name]=value
        elif line.startswith('# CONFIG_') and line.endswith(' is not set'):
            values[line[2:-11]]='n'
    return values


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build',type=Path,required=True)
    p.add_argument('--baseline',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();build=a.build.resolve();base=a.baseline.resolve()
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    work=build.parent
    old=config(base/'sdkconfig.tx-first-error');new=config(work/'sdkconfig.ble-eight')
    delta={k:{'before':old.get(k),'after':new.get(k)} for k in old.keys()|new.keys() if old.get(k)!=new.get(k)}
    assert delta=={'CONFIG_BT_LE_MAX_CONNECTIONS':{'before':'3','after':'8'}},delta
    assert new['CONFIG_BT_CONTROLLER_ONLY']=='y' and new['CONFIG_BT_ENABLED']=='y'
    assert new['CONFIG_BT_NIMBLE_ENABLED']==new['CONFIG_BT_BLUEDROID_ENABLED']=='n'
    commands=json.loads((build/'compile_commands.json').read_text())
    entry=next(e for e in commands if e['file'].endswith('/main/slave_bt.c'))
    args=entry.get('arguments') or shlex.split(entry['command'])
    cleaned=[];skip=False
    for item in args:
        if skip:skip=False;continue
        if item in ('-o','-MF','-MT','-MQ'):skip=True;continue
        if item in ('-c','-MD','-MMD') or item==entry['file']:continue
        cleaned.append(item)
    probe=out/'sdk-probe.c'
    probe.write_text('''#include "sdkconfig.h"
#ifdef PROBE_OLD_CONFIG
#undef CONFIG_BT_LE_MAX_CONNECTIONS
#define CONFIG_BT_LE_MAX_CONNECTIONS 3
#endif
#include <stddef.h>
#include <stdint.h>
#include "esp_bt.h"
'''+GUARD+'''
unsigned char c6_abi_connections[DEFAULT_BT_LE_MAX_CONNECTIONS];
unsigned char c6_abi_config_size[sizeof(esp_bt_controller_config_t)];
unsigned char c6_abi_capacity_offset[offsetof(esp_bt_controller_config_t,nimble_max_connections)+1];
''')
    obj=out/'sdk-probe.o'
    compiled=subprocess.run([*cleaned,'-c',str(probe),'-o',str(obj)],cwd=entry['directory'],capture_output=True,text=True)
    (out/'sdk-probe.log').write_text(compiled.stdout+compiled.stderr)
    compiled.check_returncode()
    negative=subprocess.run([*cleaned,'-DPROBE_OLD_CONFIG','-c',str(probe),'-o',str(out/'rejected.o')],
                            cwd=entry['directory'],capture_output=True,text=True)
    (out/'sdk-three-rejected.log').write_text(negative.stdout+negative.stderr)
    assert negative.returncode and 'C6 must configure eight connections' in negative.stderr
    target=symbols(obj);final=symbols(build/'network_adapter.elf')
    assert target['c6_abi_connections']['size']==8
    assert struct.unpack('<I',target['openvela_c6_ble_capacity']['data'])[0]==8
    marker=final['openvela_c6_ble_capacity']
    assert marker['size']==4 and struct.unpack('<I',marker['data'])[0]==8
    assert final['initialise_bluetooth']['size']>0
    bt_source=work/'esp-hosted-mcu/slave/main/slave_bt.c'
    frame_guard=HCI_BEGIN in bt_source.read_text()
    if frame_guard:
        assert final['process_hci_rx_pkt']['size']>0
        assert b'Invalid HCI frame length/type' in (build/'network_adapter.elf').read_bytes()
    app=build/'network_adapter.bin';raw=app.read_bytes()
    assert raw[0]==0xe9 and int.from_bytes(raw[12:14],'little')==13
    assert len(raw)<=0x180000
    partition=build/'partition_table/partition-table.bin'
    assert partition.read_bytes()==(base/'build/partition_table/partition-table.bin').read_bytes()
    assert (work/'esp-hosted-mcu/slave/partitions.esp32c6.csv').read_bytes()==(base/'esp-hosted-mcu/slave/partitions.esp32c6.csv').read_bytes()
    preserved={name:sha(work/'esp-hosted-mcu/slave/main'/name) for name in ['app_main.c','sdio_slave_api.c']}
    for name,digest in preserved.items():assert digest==sha(base/'esp-hosted-mcu/slave/main'/name)
    report={'passed':True,'scope':'cross_compile_and_ELF_not_runtime','target':'esp32c6',
            'controller_connections':8,'host_on_c6':False,'config_delta':delta,
            'real_sdk_probe_connections':target['c6_abi_connections']['size'],
            'controller_config_bytes':target['c6_abi_config_size']['size'],
            'capacity_field_offset':target['c6_abi_capacity_offset']['size']-1,
            'linked_capacity_marker':8,'linked_capacity_marker_address':hex(marker['address']),
            'old_three_config_rejected':True,'image_bytes':len(raw),'slot_bytes':0x180000,
            'hci_frame_length_guard':frame_guard,
            'slot_remaining_bytes':0x180000-len(raw),'image_sha256':sha(app),
            'elf_sha256':sha(build/'network_adapter.elf'),'partition_sha256':sha(partition),
            'partition_unchanged':True,'preserved_source_sha256':preserved,
            'bluetooth_source_sha256':sha(work/'esp-hosted-mcu/slave/main/slave_bt.c'),
            'device_actions':False,'eight_peer_radio_test':False}
    (out/'VERIFICATION.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__':main()
