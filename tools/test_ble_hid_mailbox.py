#!/usr/bin/env python3
"""Run production HID request/stop boundaries with a separate host thread."""
import argparse, hashlib, json, os, subprocess
from pathlib import Path
from render_all_ui import function

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[1]
ble=root/'app/espdl-quickapp/overlay/apps/wireless/bluetooth/nimble'
source=(ble/'glass_ble_hid.c').read_text()
names=['now_ms','input_cancel_locked','process_request','process_stop','rpc','snapshot',
       'glass_ble_hid_start','glass_ble_hid_stop','glass_ble_hid_mouse',
       'glass_ble_hid_release','glass_ble_hid_click']
functions='\n\n'.join(function(source,name) for name in names)
assert 'ble_npl_eventq_put' not in functions
globals_=source[source.index('static pthread_mutex_t g_lock'):source.index('static uint64_t now_ms')]
globals_=globals_.replace('static uint8_t g_address;','').replace('static int g_ble_error, g_stop_result;','static int g_stop_result;')
globals_=globals_.replace('static uint16_t g_report_handle, g_boot_handle;','')
globals_=globals_.replace('static struct ble_npl_callout g_watchdog;','').replace('static const char g_name[] = "OpenVela Mouse";','')
globals_=globals_.replace('static bool g_bond_pending;','')
globals_=globals_.replace('static unsigned g_diagnostic_count;','')
globals_=globals_.replace('static const char g_name[] = GLASS_BLE_CANDIDATE_NAME;','')
prelude='''#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "mouse_core.h"
#define BLE_HS_CONN_HANDLE_NONE 65535
#define GLASS_BLE_ROLE_HID 2u
#define RPC_TIMEOUT_SEC 2
static _Thread_local bool host_context;
static atomic_bool running=true,paused,wakeup;
static void glass_ble_central_hid_wake(void){atomic_store(&wakeup,true);}
static unsigned role,reports;
static bool adv_error;
static int advertise(void);
static int transport_send(void *,const uint8_t *,size_t,bool);
static int stop_advertising(void);
static int disconnect_if_needed(void);
static void glass_ble_role_release(unsigned);
static int glass_ble_host_wait(void){return 0;}
static int glass_ble_central_mouse_pause(void){assert(!host_context);return 0;}
static int glass_ble_central_mouse_resume(void){assert(!host_context);return 0;}
static uint32_t g_input_session;
static uint64_t g_request_until;
'''
fixture='''
static int advertise(void)
{assert(host_context);assert(!role||role==2);role=2;g_mouse.enabled=g_advertising=g_user_enabled=true;return 0;}
static int transport_send(void *arg,const uint8_t *bytes,size_t length,bool boot)
{assert(host_context);(void)arg;(void)bytes;(void)length;(void)boot;reports++;return 0;}
static int stop_advertising(void)
{assert(host_context);g_advertising=false;return adv_error?-EIO:0;}
static int disconnect_if_needed(void)
{assert(host_context);if(g_mouse.disconnect_required){mouse_core_disconnect(&g_mouse);g_conn=BLE_HS_CONN_HANDLE_NONE;}return 0;}
static void glass_ble_role_release(unsigned value)
{assert(host_context);if(role==value)role=0;}
static void *host_loop(void *unused)
{(void)unused;host_context=true;while(atomic_load(&running)){
 if(!atomic_load(&paused)){atomic_exchange(&wakeup,false);pthread_mutex_lock(&g_lock);process_request();process_stop();pthread_mutex_unlock(&g_lock);}
 usleep(1000);}return NULL;}
static int start_result,stop_result;
static void *start_caller(void *unused){(void)unused;start_result=glass_ble_hid_start();return NULL;}
static void *stop_caller(void *unused){(void)unused;stop_result=glass_ble_hid_stop();return NULL;}
int main(void)
{
 g_ready=g_initialized=true;pthread_t host;assert(!pthread_create(&host,NULL,host_loop,NULL));
 for(unsigned i=0;i<30;i++){
   assert(!glass_ble_hid_start());assert(role==2&&g_advertising);
   assert(!glass_ble_hid_stop());assert(!role&&!g_advertising&&!g_user_enabled);
 }
 assert(!glass_ble_hid_start());adv_error=true;
 assert(glass_ble_hid_stop()==-EIO);assert(!role&&!g_advertising);adv_error=false;
 assert(!glass_ble_hid_start());pthread_mutex_lock(&g_lock);
 mouse_core_connect(&g_mouse);g_mouse.enabled=g_mouse.encrypted=g_mouse.report_notify=true;
 pthread_mutex_unlock(&g_lock);
 assert(!glass_ble_hid_mouse(1,5,0,0));assert(reports);
 assert(glass_ble_hid_click(1)==-EBUSY);assert(!glass_ble_hid_release());
 assert(!glass_ble_hid_click(1));assert(!glass_ble_hid_stop());assert(!role&&!g_mouse.connected);
 atomic_store(&paused,true);usleep(10000);pthread_t start,stop;assert(!pthread_create(&start,NULL,start_caller,NULL));
 bool pending=false;for(unsigned i=0;i<2000&&!pending;i++){
   pthread_mutex_lock(&g_lock);pending=g_request.pending;pthread_mutex_unlock(&g_lock);usleep(1000);
 }assert(pending);assert(!pthread_create(&stop,NULL,stop_caller,NULL));
 pending=false;for(unsigned i=0;i<2000&&!pending;i++){
   pthread_mutex_lock(&g_lock);pending=g_stop_pending;pthread_mutex_unlock(&g_lock);usleep(1000);
 }assert(pending);atomic_store(&paused,false);
 pthread_join(start,NULL);pthread_join(stop,NULL);
 assert(start_result==-ECANCELED&&!stop_result&&!role&&!g_mouse.enabled&&!g_request.pending);
 atomic_store(&running,false);pthread_join(host,NULL);
 puts("BLE_HID_MAILBOX_OK: 30 start/stop cycles; all protocol and role release on host thread; inactive-role cleanup despite stop error; release/click; stop invalidates queued start; no cross-task NPL queue posts");
}
'''
test=out/'hid_mailbox.c';test.write_text(prelude+globals_+functions+fixture)
flags=['-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie','-pthread']
subprocess.run(['gcc',*flags,'-I'+str(ble),str(test),str(ble/'mouse_core.c'),'-o',str(out/'test')],check=True)
result=subprocess.run([str(out/'test')],check=True,capture_output=True,text=True,
                      env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
(out/'test.log').write_text(result.stdout+result.stderr);print(result.stdout,end='')
(out/'VERIFICATION.json').write_text(json.dumps({'source_sha256':hashlib.sha256(source.encode()).hexdigest(),'functions':names,'passed':True},indent=2))
