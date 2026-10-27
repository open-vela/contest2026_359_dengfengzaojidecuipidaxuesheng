#!/usr/bin/env python3
"""Exercise real QPK BLE ownership, multi-session JSON and exact cleanup."""
import argparse
import os
from pathlib import Path
import subprocess
from render_all_ui import function

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--apps', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--connections', type=int, choices=[2,8], default=2)
a = p.parse_args()
repo = Path(__file__).resolve().parents[1]
desktop = repo/'app/espdl-quickapp/overlay/apps/system/desktop'
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
source = (desktop/'qpk_hw_platform.c').read_text()
body = '\n'.join(function(source, name) for name in
                 ['number_arg', 'ble_owned', 'ble_prune', 'ble_cleanup_owned', 'ble_operation'])
code = r'''
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "qpk_hardware.h"
#include "glass_ble.h"
#include "glass_ble_central.h"
struct hw_platform {bool ble;uint32_t channel_epoch,ble_scan,ble_links[GLASS_BLE_CENTRAL_LIMIT];};
static pthread_mutex_t claims_lock=PTHREAD_MUTEX_INITIALIZER;
static struct hw_platform *ble_owner;
static struct glass_ble_state state;
static unsigned next_generation=10,gatt_calls,disconnect_count,scan_count;
static uint32_t disconnected[8],stopped_scan,cleared_epoch;
int glass_ble_channel_clear(uint32_t epoch){cleared_epoch=epoch;return 0;}
int qpk_ble_channel_execute(const char *op,const cJSON *args,cJSON *out,struct qpk_hw_progress *progress)
{(void)op;(void)args;(void)out;(void)progress;return 0;}
void glass_ble_read(struct glass_ble_state *s){*s=state;}
int glass_ble_start(void){return 0;}
int glass_ble_central_scan_session(const uint8_t service[16],uint32_t *accepted)
{(void)service;scan_count++;if(state.scanning)return -EBUSY;state.scanning=true;*accepted=++state.scan_generation;return 0;}
int glass_ble_central_scan_stop(uint32_t generation){stopped_scan=generation;return 0;}
int glass_ble_central_connect_session(unsigned index,uint32_t scan,uint32_t *accepted)
{(void)index;(void)scan;if(state.session_count==GLASS_BLE_CENTRAL_LIMIT)return -ENOSPC;*accepted=++next_generation;
 state.sessions[state.session_count++]=(struct glass_ble_session){.generation=*accepted,.connected=true};state.connection_count++;return 0;}
int glass_ble_central_disconnect_generation(uint32_t generation)
{assert(disconnect_count<8);disconnected[disconnect_count++]=generation;return 0;}
int qpk_ble_gatt_execute(const char *op,const cJSON *args,cJSON *out,struct qpk_hw_progress *progress)
{(void)op;(void)args;(void)out;(void)progress;gatt_calls++;return 0;}
'''
tests = r'''
static int run(struct hw_platform *p,const char *op,const char *text,cJSON **reply)
{cJSON *args=cJSON_Parse(text);assert(args);*reply=cJSON_CreateObject();assert(*reply);struct qpk_hw_progress progress={0};
 int ret=ble_operation(p,op,args,*reply,&progress);cJSON_Delete(args);return ret;}
int main(void)
{
 struct hw_platform app={0},other={0};cJSON *reply;
 state.central_capacity=2;
 assert(!run(&app,"connect","{\"index\":0,\"scanGeneration\":1}",&reply));
 uint32_t first=cJSON_GetObjectItem(reply,"generation")->valueint;assert(first==11&&app.ble_links[0]==first);cJSON_Delete(reply);
 assert(!run(&app,"connect","{\"index\":1,\"scanGeneration\":1}",&reply));
 uint32_t second=cJSON_GetObjectItem(reply,"generation")->valueint;assert(second==12&&app.ble_links[1]==second);cJSON_Delete(reply);
 assert(run(&app,"disconnect","{}",&reply)==-EINVAL&&!disconnect_count);cJSON_Delete(reply);
 assert(!run(&app,"read","{\"generation\":11,\"handle\":3}",&reply)&&gatt_calls==1);cJSON_Delete(reply);
 assert(run(&app,"read","{\"generation\":99,\"handle\":3}",&reply)==-EACCES&&gatt_calls==1);cJSON_Delete(reply);
 assert(!run(&app,"disconnect","{\"generation\":11}",&reply)&&disconnected[0]==first);cJSON_Delete(reply);
 assert(!run(&app,"status","{}",&reply));assert(cJSON_GetArraySize(cJSON_GetObjectItem(reply,"sessions"))==2);
 assert(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(reply,"sessions"),1),"owned")));cJSON_Delete(reply);
 assert(run(&other,"scan","{}",&reply)==-EBUSY&&!scan_count);cJSON_Delete(reply);
 /* A foreign UI link replaces one of the app's old sessions. Ownership
  * pruning must not turn it into app ownership merely because it is focused. */
 state.sessions[0].generation=99;state.generation=99;
 assert(!run(&app,"status","{}",&reply)&&!app.ble_links[0]);cJSON_Delete(reply);
 assert(!run(&app,"scan","{}",&reply)&&app.ble_scan==1);cJSON_Delete(reply);
 assert(run(&app,"scan","{}",&reply)==-EBUSY&&app.ble_scan==1);cJSON_Delete(reply);
 assert(!run(&other,"channelStatus","{}",&reply));cJSON_Delete(reply);
 assert(!run(&app,"channelSend","{\"epoch\":7,\"data\":[1]}",&reply)&&app.channel_epoch==7);cJSON_Delete(reply);
 unsigned before=disconnect_count;ble_cleanup_owned(&app);assert(cleared_epoch==7);
 assert(stopped_scan==1&&disconnect_count==before+1&&disconnected[before]==second);
 for(unsigned i=0;i<disconnect_count;i++)assert(disconnected[i]!=99);
 puts("BLE_PLATFORM_OK: both connect identities returned; complete JSON session list; ownership enforced; ambiguous disconnect rejected; failed scan preserves ownership; cleanup keeps foreign UI connection");
#if GLASS_BLE_CENTRAL_LIMIT == 8
 memset(&state,0,sizeof(state));memset(app.ble_links,0,sizeof(app.ble_links));
 state.session_count=state.connection_count=state.restore_count=8;state.central_capacity=8;
 for(unsigned i=0;i<8;i++){
   uint32_t generation=100+i;app.ble_links[i]=generation;
   state.sessions[i]=(struct glass_ble_session){.generation=generation,.connected=true};
   state.restores[i]=(struct glass_ble_restore_result){.generation=generation,.attempted=true,.connected=i!=3,.error=i==3?-ETIMEDOUT:0};
 }
 assert(!run(&app,"status","{}",&reply));
 cJSON *sessions=cJSON_GetObjectItem(reply,"sessions"),*restores=cJSON_GetObjectItem(reply,"restores");
 assert(cJSON_GetArraySize(sessions)==8&&cJSON_GetArraySize(restores)==8);
 for(unsigned i=0;i<8;i++){
   assert(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetArrayItem(sessions,i),"owned")));
   assert(cJSON_GetObjectItem(cJSON_GetArrayItem(restores,i),"generation")->valueint==100+(int)i);
 }
 assert(cJSON_GetObjectItem(cJSON_GetArrayItem(restores,3),"error")->valueint==-ETIMEDOUT);cJSON_Delete(reply);
 state.sessions[3].generation=99;assert(!run(&app,"status","{}",&reply));cJSON_Delete(reply);
 assert(!app.ble_links[3]);disconnect_count=0;ble_cleanup_owned(&app);assert(disconnect_count==7);
 for(unsigned i=0;i<disconnect_count;i++)assert(disconnected[i]!=99);
 puts("BLE_PLATFORM_EIGHT_OK: eight session/restore JSON records; seven owned links cleaned; foreign owner preserved");
#endif
 return 0;
}
'''
(out/'test.c').write_text(code+body+tests)
cjson = a.apps/'netutils/cjson/cJSON'
subprocess.run(['gcc', '-std=gnu11', '-D_GNU_SOURCE', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-DGLASS_BLE_CENTRAL_LIMIT='+str(a.connections),
                '-Wno-misleading-indentation', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-fno-pie', '-no-pie', '-pthread', '-I'+str(desktop), '-I'+str(cjson),
                str(out/'test.c'), str(cjson/'cJSON.c'), '-o', str(out/'test')], check=True)
subprocess.run([str(out/'test')], check=True,
               env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1', 'UBSAN_OPTIONS': 'halt_on_error=1'})
