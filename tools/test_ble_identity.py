#!/usr/bin/env python3
"""Test fresh build identities and the production host sync address gate."""
import argparse
import json
import os
from pathlib import Path
import subprocess
from ble_candidate_identity import create
from render_all_ui import function

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
repo=Path(__file__).resolve().parents[1]
apps=out/'apps';artifact=out/'artifacts'
first=create(apps,artifact);second=create(apps,artifact)
assert first['address']!=second['address'] and first['name']!=second['name']
assert first['store_path']!=second['store_path'] and first['purpose_path']!=second['purpose_path']
for i in [first,second]:
 address=i['address_little_endian']
 assert address[5]&0xc0==0xc0
 assert 0<(int.from_bytes(bytes(address),'little')&((1<<46)-1))<(1<<46)-1
 assert len(i['name'].encode())+2<=31
 assert all(len(Path(i[k]).name)<=32 for k in ['store_path','store_temp','purpose_path','purpose_temp'])
assert json.loads((artifact/'BLE-IDENTITY.json').read_text())==second
text=(repo/'app/espdl-quickapp/overlay/apps/wireless/bluetooth/nimble/glass_ble.c').read_text()
code='''#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "glass_ble_candidate.h"
#define BLE_HS_EINVAL 3
#define BLE_ADDR_RANDOM 1
#define BLE_OWN_ADDR_RANDOM 1
static pthread_mutex_t g_host_lock=PTHREAD_MUTEX_INITIALIZER;
static bool g_ready;
static int g_error,stage,failure,hid_calls,corrupt,nrpa_mode,wrong_type;
static uint8_t configured[6];
static int ble_hs_util_ensure_addr(int privacy){assert(!privacy);stage++;return failure==1?6:0;}
static int ble_hs_id_set_rnd(const uint8_t *address){assert(stage++==1);memcpy(configured,address,6);return failure==2?6:0;}
static int ble_hs_id_copy_addr(uint8_t type,uint8_t *address,int *nrpa){assert(type==1&&stage++==2);memcpy(address,configured,6);if(corrupt)address[0]^=1;*nrpa=nrpa_mode;return failure==3?6:0;}
static int ble_hs_id_infer_auto(int privacy,uint8_t *type){assert(!privacy&&stage++==3);*type=wrong_type?0:1;return failure==4?6:0;}
static void glass_ble_hid_synced(uint8_t type){assert(type==1&&stage++==4);hid_calls++;}
'''+function(text,'synced')+'''
int main(void){
 static const uint8_t expected[6]=GLASS_BLE_CANDIDATE_ADDRESS;
 for(int n=0;n<2;n++){stage=0;synced();assert(g_ready&&!g_error&&stage==5);assert(!memcmp(expected,configured,6));}
 assert(hid_calls==2);
 for(failure=1;failure<=4;failure++){stage=0;synced();assert(!g_ready&&g_error==-EIO&&hid_calls==2);}
 failure=0;corrupt=1;stage=0;synced();assert(!g_ready&&g_error==-EIO&&hid_calls==2);
 corrupt=0;nrpa_mode=1;stage=0;synced();assert(!g_ready&&hid_calls==2);
 nrpa_mode=0;wrong_type=1;stage=0;synced();assert(!g_ready&&hid_calls==2);
 puts("BLE_IDENTITY_OK: fresh per build; static-random LE byte order; bounded name/path; isolated bond namespaces; stable runtime resync; set/readback/type failures stop HID startup");
}
'''
source=out/'identity.c';source.write_text(code)
subprocess.run(['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-pthread',
 '-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie',
 '-I'+str(apps/'wireless/bluetooth/nimble'),str(source),'-o',str(out/'test')],check=True)
r=subprocess.run([str(out/'test')],check=True,capture_output=True,text=True,
 env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
(out/'test.log').write_text(r.stdout+r.stderr);print(r.stdout,end='')
