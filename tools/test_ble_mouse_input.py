#!/usr/bin/env python3
"""Run production HID queue/error/lease APIs and passive GAP observation."""
import argparse,os,subprocess
from pathlib import Path
from render_all_ui import function
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
repo=Path(__file__).resolve().parents[1];ble=repo/'app/espdl-quickapp/overlay/apps/wireless/bluetooth/nimble'
hid=(ble/'glass_ble_hid.c').read_text()
names=['hid_disconnect_record','input_cancel_locked','glass_ble_hid_input_begin','input_owned_locked','glass_ble_hid_input_report','glass_ble_hid_input_click','glass_ble_hid_input_keepalive','glass_ble_hid_input_end','glass_ble_hid_input_read','transport_send','hid_link_poll']
code=r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mouse_core.h"
#include "glass_ble.h"
#include "glass_ble_hid_link.h"
#define BLE_HS_EAGAIN 1
#define BLE_HS_EALREADY 2
#define BLE_HS_ENOMEM 6
#define BLE_HS_ENOTCONN 7
#define BLE_HS_EBUSY 15
#define BLE_HS_EAUTHEN 23
#define BLE_HS_EAUTHOR 24
#define BLE_HS_EENCRYPT 25
static pthread_mutex_t g_lock=PTHREAD_MUTEX_INITIALIZER;
static struct mouse_core g_mouse;
static struct glass_ble_hid_link g_link;
#define BLE_HS_HCI_ERR(x) (0x200+(x))
#define BLE_ERR_CONN_TERM_LOCAL 0x16
static uint32_t g_disconnects,g_unexpected_disconnects,g_last_disconnect_epoch;
static int g_last_disconnect_reason;
static unsigned g_last_parameter_attempts,g_last_parameter_updates;
static uint16_t g_last_interval;
static uint64_t g_last_secure_age_ms;
static uint32_t g_input_session,g_input_counter,g_input_epoch;
static struct {bool pending;} g_request;
static bool g_stop_pending;
static unsigned wakes;
static uint64_t ticks;
static uint64_t now_ms(void){return ticks;}
static void glass_ble_central_hid_wake(void){wakes++;}
static uint16_t g_conn=3,g_report_handle=7,g_boot_handle=8;
static int g_ble_error,notify_error;
static bool alloc_fail;
struct os_mbuf {uint8_t data[4];};
static struct os_mbuf *ble_hs_mbuf_from_flat(const void *data,uint16_t n)
{if(alloc_fail)return NULL;assert(n==3||n==4);struct os_mbuf *p=calloc(1,sizeof(*p));assert(p);memcpy(p->data,data,n);return p;}
static int ble_gatts_notify_custom(uint16_t conn,uint16_t attr,struct os_mbuf *p)
{assert(conn==3&&(attr==7||attr==8));free(p);return notify_error;}
struct ble_gap_conn_desc {uint16_t conn_itvl,conn_latency,supervision_timeout;};
struct ble_gap_upd_params {uint16_t itvl_min,itvl_max,latency,supervision_timeout,min_ce_len,max_ce_len;};
static struct ble_gap_conn_desc fixture={24,0,400};
static int parameter_error;
static unsigned parameters;
static bool inline_event;
static int ble_gap_conn_find(uint16_t handle,struct ble_gap_conn_desc *desc)
{assert(handle==3);*desc=fixture;return 0;}
static int ble_gap_update_params(uint16_t handle,const struct ble_gap_upd_params *params)
{assert(handle==3&&params->itvl_min==8&&params->itvl_max==12&&!params->latency&&params->supervision_timeout==400);parameters++;
 if(inline_event){assert(!pthread_mutex_trylock(&g_lock));fixture.conn_itvl=8;glass_ble_hid_link_changed(&g_link,3,0);pthread_mutex_unlock(&g_lock);}
 return parameter_error;}
@LINK@
@FUNCTIONS@
static unsigned sends,delivered;
static int send_error;
static uint8_t packets[128][4];
static int output(void *arg,const uint8_t *data,size_t n,bool boot)
{(void)arg;assert(n==(boot?3u:4u));sends++;if(send_error)return send_error;assert(delivered<128);memcpy(packets[delivered++],data,n);return 0;}
static void setup(void)
{memset(&g_mouse,0,sizeof(g_mouse));mouse_core_connect(&g_mouse);g_mouse.enabled=g_mouse.encrypted=g_mouse.report_notify=true;
 g_input_session=0;g_request.pending=g_stop_pending=false;ticks=0;sends=delivered=0;send_error=0;memset(packets,0,sizeof(packets));}
int main(void)
{
 (void)ble_gap_update_params; /* A call would increment the forbidden count. */
 setup();uint32_t epoch=g_mouse.epoch;
 assert(!mouse_core_enqueue(&g_mouse,epoch,0,100,0,0,0));assert(!mouse_core_enqueue(&g_mouse,epoch,0,100,0,0,1));
 assert(g_mouse.queued==1&&g_mouse.merged==1);
 assert(!mouse_core_tick(&g_mouse,2,output,NULL));assert(!mouse_core_tick(&g_mouse,12,output,NULL));
 assert(delivered==2&&(int8_t)packets[0][1]+(int8_t)packets[1][1]==200&&!g_mouse.queued);
 setup();epoch=g_mouse.epoch;
 for(unsigned i=0;i<7;i++)assert(!mouse_core_enqueue(&g_mouse,epoch,i%2?0:1,0,0,0,i));
 assert(mouse_core_enqueue(&g_mouse,epoch,2,0,0,0,8)==-ENOSPC);
 assert(!mouse_core_enqueue(&g_mouse,epoch,0,0,0,0,8)&&g_mouse.queued==8);
 for(unsigned i=0;i<8;i++)assert(!mouse_core_tick(&g_mouse,10+i*10,output,NULL));
 assert(delivered==8&&!g_mouse.buttons);for(unsigned i=0;i<7;i++)assert(packets[i][0]==(i%2?0:1));
 setup();epoch=g_mouse.epoch;assert(!mouse_core_enqueue_click(&g_mouse,epoch,4,0));
 send_error=-ENOMEM;assert(mouse_core_tick(&g_mouse,0,output,NULL)==-ENOMEM);assert(g_mouse.enabled&&!g_mouse.disconnect_required&&g_mouse.queued==2);
 send_error=0;assert(!mouse_core_tick(&g_mouse,10,output,NULL));assert(!mouse_core_tick(&g_mouse,20,output,NULL));
 assert(delivered==2&&packets[0][0]==4&&!packets[1][0]&&g_mouse.retries==1);
 setup();epoch=g_mouse.epoch;assert(!mouse_core_enqueue(&g_mouse,epoch,0,5,0,0,0));
 send_error=-EAGAIN;assert(mouse_core_tick(&g_mouse,0,output,NULL)==-EAGAIN);
 assert(mouse_core_tick(&g_mouse,250,output,NULL)==-ETIMEDOUT);assert(!g_mouse.queued&&g_mouse.enabled&&!g_mouse.disconnect_required);
 send_error=0;assert(!mouse_core_enqueue(&g_mouse,epoch,0,9,0,0,251));assert(!mouse_core_tick(&g_mouse,251,output,NULL));assert(delivered==1&&packets[0][1]==9);
 setup();epoch=g_mouse.epoch;assert(!mouse_core_move(&g_mouse,epoch,1,0,0,0,0,output,NULL));
 for(ticks=200;ticks<=10000;ticks+=200){assert(!mouse_core_keepalive(&g_mouse,epoch,ticks));assert(!mouse_core_tick(&g_mouse,ticks,output,NULL));assert(g_mouse.buttons==1);}
 send_error=-ENOMEM;assert(mouse_core_tick(&g_mouse,11500,output,NULL)==-ENOMEM);assert(g_mouse.release_pending&&g_mouse.buttons==1&&g_mouse.enabled);
 send_error=0;assert(!mouse_core_tick(&g_mouse,11510,output,NULL)&&!g_mouse.buttons&&!g_mouse.release_pending);
 setup();epoch=g_mouse.epoch;assert(!mouse_core_move(&g_mouse,epoch,1,0,0,0,0,output,NULL));send_error=-EAGAIN;
 mouse_core_cancel_input(&g_mouse,10);assert(mouse_core_tick(&g_mouse,10,output,NULL)==-EAGAIN);
 assert(mouse_core_tick(&g_mouse,510,output,NULL)==-ETIMEDOUT&&g_mouse.disconnect_required&&!g_mouse.enabled);
 setup();assert(!mouse_core_enqueue(&g_mouse,g_mouse.epoch,0,10,0,0,0));send_error=-EIO;
 assert(mouse_core_tick(&g_mouse,0,output,NULL)==-EIO&&g_mouse.disconnect_required&&!g_mouse.queued);assert(sends==1);
 setup();epoch=g_mouse.epoch;uint32_t token=0;assert(!glass_ble_hid_input_begin(epoch,&token)&&token);
 assert(!glass_ble_hid_input_report(token,1,0,0,0));assert(glass_ble_hid_input_report(token+1,0,9,0,0)==-ESTALE);
 assert(!mouse_core_tick(&g_mouse,0,output,NULL));assert(!glass_ble_hid_input_keepalive(token));
 assert(!glass_ble_hid_input_end(token)&&!g_input_session&&g_mouse.release_pending);
 assert(!mouse_core_tick(&g_mouse,10,output,NULL)&&!g_mouse.buttons);
 uint32_t next=0;assert(!glass_ble_hid_input_begin(epoch,&next)&&next!=token);assert(glass_ble_hid_input_end(token)==-ESTALE&&g_input_session==next);
 assert(!glass_ble_hid_input_end(next));g_request.pending=true;assert(glass_ble_hid_input_begin(epoch,&next)==-EBUSY);g_request.pending=false;
 assert(!glass_ble_hid_input_begin(epoch,&next));mouse_core_disconnect(&g_mouse);assert(glass_ble_hid_input_report(next,1,0,0,0)==-ESTALE);
 setup();alloc_fail=true;assert(transport_send(NULL,packets[0],4,false)==-ENOMEM);alloc_fail=false;
 int codes[]={6,15,1,7,23,24,25,99};int errors[]={-ENOMEM,-EAGAIN,-EAGAIN,-ENOTCONN,-EACCES,-EACCES,-EACCES,-EIO};
 for(unsigned i=0;i<sizeof(codes)/sizeof(*codes);i++){notify_error=codes[i];assert(transport_send(NULL,packets[0],4,false)==errors[i]);}
 notify_error=0;assert(!transport_send(NULL,packets[0],4,false));
 memset(&g_link,0,sizeof(g_link));parameters=0;parameter_error=0;fixture.conn_itvl=24;
 glass_ble_hid_link_state(&g_link,3,1,true,true,0);ticks=1;hid_link_poll();assert(!parameters&&g_link.sampled&&g_link.interval==24);
 ticks=5000;inline_event=true;hid_link_poll();assert(!parameters&&g_link.interval==24&&!g_link.result);
 fixture.conn_itvl=8;glass_ble_hid_link_changed(&g_link,3,0);assert(g_link.interval==8&&g_link.changed==1);
 glass_ble_hid_link_state(&g_link,3,2,true,true,6000);inline_event=false;parameters=0;fixture.conn_itvl=24;parameter_error=BLE_HS_EBUSY;
 for(ticks=7000;ticks<=10000;ticks+=1000)hid_link_poll();assert(!parameters&&g_link.sampled&&g_link.interval==24);
 glass_ble_hid_link_state(&g_link,3,3,true,true,11000);parameters=0;fixture.conn_itvl=6;parameter_error=0;ticks=16000;hid_link_poll();assert(!parameters&&g_link.interval==6);
 g_link.attempts=2;g_link.changed=1;g_link.interval=12;g_link.secured_at=15000;
 hid_disconnect_record(520);assert(g_disconnects==1&&g_unexpected_disconnects==1&&g_last_secure_age_ms==1000);
 glass_ble_hid_link_state(&g_link,3,4,true,true,ticks);
 struct glass_ble_hid_input_state snapshot;glass_ble_hid_input_read(&snapshot);
 assert(snapshot.last_disconnect_reason==520&&snapshot.last_parameter_attempts==2&&snapshot.last_parameter_updates==1&&snapshot.last_interval==12);
 hid_disconnect_record(534);assert(g_disconnects==2&&g_unexpected_disconnects==1);
 puts("BLE_MOUSE_INPUT_OK: bounded relative-sum merge/split; eight ordered edges and reserved release; safe congestion retry/no disconnect; stale drop/no replay; 10s live hold, lost lease release; release deadline fail-safe; session/epoch ownership; mbuf ownership/error mapping; passive GAP observation without automatic parameter updates; retained disconnect diagnostics");
}
'''
code=code.replace('@FUNCTIONS@','\n'.join(function(hid,n) for n in names))
code=code.replace('@LINK@','#include "'+(ble/'glass_ble_hid_link.c').as_posix()+'"')
inc=out/'include/host';inc.mkdir(parents=True,exist_ok=True);(inc/'ble_hs.h').write_text('#pragma once\n')
source=out/'input.c';source.write_text(code)
subprocess.run(['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie','-I'+str(out/'include'),'-I'+str(ble),str(source),str(ble/'mouse_core.c'),'-o',str(out/'test')],check=True)
result=subprocess.run([str(out/'test')],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
(out/'test.log').write_text(result.stdout+result.stderr);print(result.stdout+result.stderr,end='');assert result.returncode==0,result.returncode
