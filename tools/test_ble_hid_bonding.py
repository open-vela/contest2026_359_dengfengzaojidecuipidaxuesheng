#!/usr/bin/env python3
"""Check that HID purpose is tagged only after SMP returns to its event queue."""
import argparse
import os
from pathlib import Path
import subprocess
from render_all_ui import function

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
repo=Path(__file__).resolve().parents[1]
text=(repo/'app/espdl-quickapp/overlay/apps/wireless/bluetooth/nimble/glass_ble_hid.c').read_text()
gap=function(text,'gap_event')
enc=function(text,'hid_encryption_changed')
assert 'glass_ble_store_mark_hid_peer' not in gap
assert 'g_bond_pending = true;' in enc
assert 'hid_persist_purpose' not in function(text,'glass_ble_hid_poll')
assert 'g_bond_pending = false;' in function(text,'glass_ble_hid_reset')
code='''#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <pthread.h>
#define BLE_HS_CONN_HANDLE_NONE 65535
#define BLE_HS_ENOTCONN 7
#define LOG_WARNING 4
#define syslog(...) ((void)0)
static bool g_bond_pending,g_ready=true;
static struct {bool connected,encrypted,enabled,disconnect_required,release_pending;int error;uint32_t epoch,cancelled;unsigned queued;} g_mouse;
static uint32_t g_input_session;
static uint64_t g_business_poll_at;
static struct {bool pending;} g_request;
#define MOUSE_REPORT_MS 10
static uint16_t g_conn=3;
static int finds,tags,result,find_error;
static int g_ble_error,disconnects;
static unsigned g_diagnostic_count;
struct ble_gap_conn_desc {struct {bool encrypted,bonded;int key_size;} sec_state;struct {uint8_t type,val[6];} peer_id_addr;};
static struct ble_gap_conn_desc fixture;
static int ble_gap_conn_find(uint16_t conn,struct ble_gap_conn_desc *d)
{assert(conn==3);finds++;*d=fixture;return find_error;}
static int glass_ble_store_mark_hid_peer(uint8_t type,const uint8_t *address)
{tags++;assert(type==1&&address[0]==42);return result;}
static pthread_mutex_t g_lock=PTHREAD_MUTEX_INITIALIZER;
static bool g_advertising;
static uint64_t ticks,deadline;
static unsigned watchdog_resets,channel_polls;
static struct ble_npl_event {int unused;} event;
static struct ble_npl_callout {int unused;} g_watchdog;
#define GLASS_BLE_ROLE_HID 2
static uint64_t now_ms(void){return ticks;}
static int transport_send(void *a,const uint8_t *b,size_t n,bool boot){(void)a;(void)b;(void)n;(void)boot;return 0;}
static void process_request(void){}
static void process_stop(void){}
static int disconnect_if_needed(void){disconnects++;return 0;}
static int stop_advertising(void){return 0;}
static void glass_ble_role_release(unsigned role){assert(role==2);}
static void glass_ble_channel_update(uint16_t c,uint32_t e,bool on,bool secure){(void)c;(void)e;(void)on;(void)secure;}
static void glass_ble_channel_poll(void){channel_polls++;}
static void hid_link_poll(void){}
static int mouse_core_tick(void *m,uint64_t t,int(*send)(void *,const uint8_t *,size_t,bool),void *arg){(void)m;(void)t;(void)send;(void)arg;return 0;}
static unsigned ble_npl_time_ms_to_ticks32(unsigned ms){return ms;}
static void ble_npl_callout_reset(struct ble_npl_callout *c,unsigned ms){(void)c;deadline=ticks+ms;watchdog_resets++;}
'''+enc+function(text,'hid_persist_purpose')+function(text,'glass_ble_hid_poll')+function(text,'watchdog')+'''
int main(void){
 g_mouse.connected=g_mouse.encrypted=true;
 fixture.sec_state.encrypted=fixture.sec_state.bonded=true;fixture.sec_state.key_size=16;
 fixture.peer_id_addr.type=1;fixture.peer_id_addr.val[0]=42;
 hid_persist_purpose();assert(!tags&&!finds);
 g_bond_pending=true;hid_persist_purpose();assert(tags==1&&!g_bond_pending);
 hid_persist_purpose();assert(tags==1);
 result=-ENOSPC;g_bond_pending=true;hid_persist_purpose();assert(tags==2&&g_mouse.error==-ENOSPC);
 fixture.sec_state.bonded=false;g_bond_pending=true;hid_persist_purpose();assert(tags==2);
 fixture.sec_state.bonded=true;fixture.sec_state.key_size=7;g_bond_pending=true;hid_persist_purpose();assert(tags==2);
 find_error=5;g_bond_pending=true;hid_persist_purpose();assert(tags==2&&g_mouse.error==-EIO);
 g_mouse.connected=false;g_bond_pending=true;int n=finds;hid_persist_purpose();assert(finds==n&&!g_bond_pending);
 g_mouse.connected=true;g_mouse.encrypted=false;g_bond_pending=true;hid_persist_purpose();assert(finds==n);
 g_mouse.encrypted=true;g_ready=false;g_bond_pending=true;hid_persist_purpose();assert(finds==n);
 g_ready=true;g_conn=BLE_HS_CONN_HANDLE_NONE;g_bond_pending=true;hid_persist_purpose();assert(finds==n);
 g_conn=3;g_ready=g_mouse.connected=g_mouse.encrypted=g_mouse.enabled=true;
 find_error=result=0;fixture.sec_state.key_size=16;fixture.sec_state.bonded=true;
 g_bond_pending=true;int before=tags;deadline=MOUSE_REPORT_MS;
 for(ticks=1;ticks<=200;ticks++){
  glass_ble_hid_poll();
  if(ticks>=deadline)watchdog(&event);
  if(ticks==51)assert(tags==before+1&&!g_bond_pending);
 }
 assert(watchdog_resets==20&&channel_polls==220);
 g_conn=3;g_mouse.enabled=g_mouse.encrypted=true;g_mouse.error=0;
 g_bond_pending=true;int previous_find=finds,previous_disconnect=disconnects;
 hid_encryption_changed(9,BLE_HS_ENOTCONN);
 assert(g_mouse.encrypted&&g_bond_pending&&finds==previous_find);
 hid_encryption_changed(3,BLE_HS_ENOTCONN);
 assert(!g_mouse.encrypted&&!g_bond_pending&&g_mouse.enabled&&!g_mouse.error);
 assert(finds==previous_find&&disconnects==previous_disconnect&&!g_ble_error);
 hid_encryption_changed(3,0);assert(g_mouse.encrypted&&g_bond_pending);
 fixture.sec_state.key_size=7;hid_encryption_changed(3,0);
 assert(!g_mouse.encrypted&&!g_mouse.enabled&&g_mouse.error==-EACCES&&disconnects==previous_disconnect+1);
 fixture.sec_state.key_size=16;g_mouse.enabled=true;hid_encryption_changed(3,517);
 assert(!g_mouse.encrypted&&!g_mouse.enabled&&g_ble_error==517&&disconnects==previous_disconnect+2);
 puts("BLE_HID_TEARDOWN_OK: actual encryption callback treats ENOTCONN as disconnect cleanup, no extra HCI terminate or fake key failure; foreign handle, weak key and real error guards retained");
 puts("BLE_HID_BONDING_OK: defer past SMP persistence; event queue only; pending consumed once; current encrypted 16-byte bonded owner; reset/disconnect guards; errors retained");
}
'''
source=out/'bonding.c';source.write_text(code)
subprocess.run(['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
    '-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie',
    str(source),'-o',str(out/'test')],check=True)
r=subprocess.run([str(out/'test')],check=True,capture_output=True,text=True,
    env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
(out/'test.log').write_text(r.stdout+r.stderr);print(r.stdout,end='')
