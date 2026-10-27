#!/usr/bin/env python3
"""Exercise production HID access and repeat-pairing ownership boundaries."""
import argparse,os,re,subprocess
from pathlib import Path
from render_all_ui import function

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
repo=Path(__file__).resolve().parents[1]
ble=repo/'app/espdl-quickapp/overlay/apps/wireless/bluetooth/nimble'
text=(ble/'glass_ble_hid.c').read_text()
defines='\n'.join(l for l in text.splitlines() if re.match(r'#define (?:HID_|READ_SEC)',l))
body='\n\n'.join(function(text,n) for n in ['append','hid_access','hid_repeat_pairing'])
code='''#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "mouse_core.h"
#define BLE_HS_CONN_HANDLE_NONE 65535
#define BLE_ATT_ERR_INSUFFICIENT_AUTHOR 8
#define BLE_ATT_ERR_INSUFFICIENT_RES 17
#define BLE_ATT_ERR_UNLIKELY 14
#define BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN 13
#define BLE_GATT_ACCESS_OP_READ_DSC 1
#define BLE_GATT_ACCESS_OP_READ_CHR 2
#define BLE_GATT_ACCESS_OP_WRITE_CHR 3
#define BLE_GAP_REPEAT_PAIRING_IGNORE 0
#define BLE_GAP_REPEAT_PAIRING_RETRY 1
struct os_mbuf {uint16_t len;uint8_t data[512];};
struct ble_gatt_access_ctxt {int op;struct os_mbuf *om;};
struct ble_gap_conn_desc {uint8_t peer_id_addr;};
#define OS_MBUF_PKTLEN(om) ((om)->len)
static pthread_mutex_t g_lock=PTHREAD_MUTEX_INITIALIZER;
static uint16_t g_conn=3;
static struct mouse_core g_mouse;
static unsigned appends,reports,disconnects,finds,deletes;
static int find_error,delete_error;
static int os_mbuf_append(struct os_mbuf *om,const void *p,size_t n)
{appends++;assert(om->len+n<=sizeof(om->data));memcpy(om->data+om->len,p,n);om->len+=n;return 0;}
static int ble_hs_mbuf_to_flat(struct os_mbuf *om,void *out,uint16_t n,uint16_t *copied)
{assert(om->len<=n);memcpy(out,om->data,om->len);*copied=om->len;return 0;}
static int transport_send(void *arg,const uint8_t *p,size_t n,bool boot)
{(void)arg;(void)p;(void)n;(void)boot;assert(g_conn==3);reports++;return 0;}
static int disconnect_if_needed(void){disconnects++;return 0;}
static void input_cancel_locked(void){mouse_core_cancel_input(&g_mouse,0);}
static int ble_gap_conn_find(uint16_t conn,struct ble_gap_conn_desc *d)
{finds++;assert(conn==3);d->peer_id_addr=42;return find_error;}
static int ble_store_util_delete_peer(const uint8_t *addr)
{deletes++;assert(*addr==42);return delete_error;}
'''+defines+'\n'+body+'''
int main(void)
{
 memset(&g_mouse,0,sizeof(g_mouse));mouse_core_connect(&g_mouse);
 g_mouse.enabled=g_mouse.encrypted=g_mouse.report_notify=true;
 struct os_mbuf om={.len=1,.data={0}};
 struct ble_gatt_access_ctxt ctxt={.op=BLE_GATT_ACCESS_OP_WRITE_CHR,.om=&om};
 struct mouse_core before=g_mouse;
 assert(hid_access(7,0,&ctxt,(void *)(uintptr_t)HID_PROTOCOL)==BLE_ATT_ERR_INSUFFICIENT_AUTHOR);
 assert(!memcmp(&before,&g_mouse,sizeof(before))&&!reports&&!disconnects&&!appends);
 assert(hid_access(7,0,&ctxt,(void *)(uintptr_t)HID_CONTROL)==BLE_ATT_ERR_INSUFFICIENT_AUTHOR);
 ctxt.op=BLE_GATT_ACCESS_OP_READ_CHR;om.len=0;
 assert(hid_access(7,0,&ctxt,(void *)(uintptr_t)HID_MAP)==BLE_ATT_ERR_INSUFFICIENT_AUTHOR&&!appends);
 assert(!hid_access(3,0,&ctxt,(void *)(uintptr_t)HID_MAP)&&om.len==mouse_report_map_size);
 ctxt.op=BLE_GATT_ACCESS_OP_WRITE_CHR;om.len=1;om.data[0]=0;
 assert(!hid_access(3,0,&ctxt,(void *)(uintptr_t)HID_PROTOCOL)&&g_mouse.boot);
 om.data[0]=0;assert(!hid_access(3,0,&ctxt,(void *)(uintptr_t)HID_CONTROL)&&g_mouse.suspended);
 unsigned a=appends,f=finds,d=deletes;
 assert(hid_repeat_pairing(7)==BLE_GAP_REPEAT_PAIRING_IGNORE&&finds==f&&deletes==d);
 assert(hid_repeat_pairing(3)==BLE_GAP_REPEAT_PAIRING_RETRY&&deletes==d+1);
 find_error=1;assert(hid_repeat_pairing(3)==BLE_GAP_REPEAT_PAIRING_IGNORE&&deletes==d+1);
 find_error=0;delete_error=1;assert(hid_repeat_pairing(3)==BLE_GAP_REPEAT_PAIRING_IGNORE);
 g_conn=BLE_HS_CONN_HANDLE_NONE;f=finds;d=deletes;
 assert(hid_repeat_pairing(3)==BLE_GAP_REPEAT_PAIRING_IGNORE&&finds==f&&deletes==d);
 assert(hid_access(3,0,&ctxt,(void *)(uintptr_t)HID_PROTOCOL)==BLE_ATT_ERR_INSUFFICIENT_AUTHOR&&appends==a);
 puts("BLE_HID_OWNERSHIP_OK: foreign/stale reads and writes rejected; owner map/mode/suspend preserved; repeat-pairing owner only; lookup/delete errors propagated");
}
'''
source=out/'ownership.c';source.write_text(code)
subprocess.run(['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-pthread',
    '-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie',
    '-I'+str(ble),str(source),str(ble/'mouse_core.c'),'-o',str(out/'test')],check=True)
r=subprocess.run([str(out/'test')],check=True,capture_output=True,text=True,
    env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
(out/'test.log').write_text(r.stdout+r.stderr);print(r.stdout,end='')
