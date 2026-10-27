#!/usr/bin/env python3
"""Ensure a mouse link never masquerades as a central-device connection."""
import argparse,os,subprocess
from pathlib import Path
from render_all_ui import function

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
repo=Path(__file__).resolve().parents[1]
ble=repo/'app/espdl-quickapp/overlay/apps/wireless/bluetooth/nimble'
body=function((ble/'glass_ble.c').read_text(),'glass_ble_read')
code='''#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "glass_ble.h"
static pthread_mutex_t g_host_lock=PTHREAD_MUTEX_INITIALIZER;
static bool g_started=true,g_ready=true,device_connected,mouse_connected;
static unsigned hid_reads;
static int g_error;
static int store_error;
static int glass_ble_store_last_error(void){return store_error;}
static uint32_t generation;
void glass_ble_central_read(struct glass_ble_state *state)
{
 state->connected=device_connected;state->peer_valid=device_connected;
 state->generation=generation;state->encrypted=state->bonded=device_connected;
 state->session_count=device_connected?2:0;state->connection_count=state->session_count;
 for(unsigned i=0;i<state->session_count;i++){
  state->sessions[i].connected=state->sessions[i].encrypted=state->sessions[i].bonded=true;
  state->sessions[i].peer.bonded=true;
 }
}
void glass_ble_hid_read(struct glass_ble_hid_state *state)
{memset(state,0,sizeof(*state));state->connected=mouse_connected;hid_reads++;}
'''+body+'''
int main(void)
{
 struct glass_ble_state state;
 for(unsigned d=0;d<2;d++)for(unsigned m=0;m<2;m++){
  device_connected=d;mouse_connected=m;generation++;
  glass_ble_read(&state);
  assert(state.started&&state.ready&&state.connected==device_connected);
  assert(state.peer_valid==device_connected&&state.encrypted==device_connected);
  assert(state.generation==generation&&!hid_reads);
 }
 mouse_connected=true;device_connected=false;generation++;
 glass_ble_read(&state);assert(!state.connected&&!state.peer_valid);
 g_ready=false;g_error=-5;glass_ble_read(&state);assert(!state.ready&&state.error==-5);
 glass_ble_read(NULL);assert(!hid_reads);
 device_connected=true;store_error=-28;glass_ble_read(&state);
 assert(state.connected&&state.encrypted&&!state.bonded&&state.store_error==-28);
 assert(state.session_count==2&&!state.sessions[0].bonded&&!state.sessions[1].bonded);
 assert(!state.sessions[0].peer.bonded&&!state.sessions[1].peer.bonded);
 puts("BLE_SNAPSHOTS_OK: four link combinations; independent disconnect/generation; shared host error; NULL output; no HID snapshot dependency");
}
'''
source=out/'snapshot.c';source.write_text(code)
subprocess.run(['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
    '-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie',
    '-I'+str(ble),str(source),'-o',str(out/'test')],check=True)
r=subprocess.run([str(out/'test')],capture_output=True,text=True,check=True,
    env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
(out/'test.log').write_text(r.stdout+r.stderr);print(r.stdout,end='')
