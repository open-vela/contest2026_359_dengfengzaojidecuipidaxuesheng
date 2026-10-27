#!/usr/bin/env python3
"""Compile the production BLE console parser and verify bounded dispatch."""
import argparse
import os
from pathlib import Path
import subprocess

from render_all_ui import function

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
ble = root / 'app/espdl-quickapp/overlay/apps/wireless/bluetooth/nimble'
source = (root / 'app/espdl-quickapp/overlay/apps/system/c6ble/ble_main.c').read_text()
parsed = '\n'.join(function(source, n) for n in ['number', 'service_uuid', 'central_command', 'channel_command', 'input_command'])
harness = r'''
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "glass_ble.h"
#include "glass_ble_central.h"
#include "glass_ble_channel.h"
static unsigned calls, index_seen;
static uint32_t generation_seen;
static uint8_t filter[16];
static struct glass_gatt_request request_seen;
int glass_ble_hid_input_begin(uint32_t epoch,uint32_t *session){assert(epoch==4);*session=9;calls++;return 0;}
int glass_ble_hid_input_end(uint32_t session){assert(session==9);calls++;return 0;}
int glass_ble_hid_input_keepalive(uint32_t session){assert(session==9);calls++;return 0;}
int glass_ble_hid_input_report(uint32_t session,int b,int x,int y,int w)
{assert(session==9&&b==0&&x==-1024&&y==1024&&w==0);calls++;return 0;}
void glass_ble_channel_read(struct glass_ble_channel_state *s){memset(s,0,sizeof(*s));s->ready=true;s->epoch=4;}
int glass_ble_channel_send(uint32_t epoch,const uint8_t *data,size_t n,atomic_bool *cancel)
{assert(!cancel&&epoch==4&&n==2&&data[0]==0xab&&data[1]==0x12);calls++;return 0;}
int glass_ble_channel_receive(uint32_t epoch,uint8_t *data,size_t *n)
{assert(epoch==4&&*n>=2);data[0]=42;data[1]=99;*n=2;calls++;return 0;}

int glass_ble_central_scan(void){calls++;return 0;}
int glass_ble_central_scan_service(const uint8_t uuid[16]){calls++;memcpy(filter,uuid,16);return 0;}
int glass_ble_central_scan_stop(uint32_t g){calls++;generation_seen=g;return 0;}
int glass_ble_central_connect_session(unsigned i,uint32_t g,uint32_t *accepted){calls++;index_seen=i;generation_seen=g;*accepted=91;return 0;}
int glass_ble_central_select(uint32_t g){calls++;generation_seen=g;return 0;}
int glass_ble_central_disconnect_generation(uint32_t g){calls++;generation_seen=g;return -ESTALE;}
void glass_ble_read(struct glass_ble_state *s){calls++;s->count=GLASS_BLE_LIMIT+1;s->scan_generation=7;}
int glass_ble_gatt(const struct glass_gatt_request *r,struct glass_gatt_result *out,atomic_bool *cancel)
{assert(!cancel);calls++;request_seen=*r;out->generation=r->generation;out->length=2;out->data[0]=42;out->data[1]=99;return 0;}
int glass_ble_notification(uint32_t g,struct glass_gatt_result *out)
{calls++;generation_seen=g;out->generation=g;return -EAGAIN;}
'''
cases = r'''
int main(void)
{
  uint8_t uuid[16];
  assert(!service_uuid("00112233-4455-6677-8899-AABBCCDDEEFF",uuid));
  for(unsigned i=0;i<16;i++)assert(uuid[i]==(15-i)*17);
  assert(service_uuid("00112233-4455-6677-8899-AABBCCDDEEFG",uuid)==-EINVAL);
  assert(service_uuid("00112233_4455-6677-8899-AABBCCDDEEFF",uuid)==-EINVAL);
  assert(service_uuid("short",uuid)==-EINVAL);
  char *scan[]={"c6ble","central","scan","00112233-4455-6677-8899-aabbccddeeff"};
  assert(!central_command(4,scan));for(unsigned i=0;i<16;i++)assert(filter[i]==(15-i)*17);
  char *connect[]={"c6ble","central","connect","31","7"};
  assert(!central_command(5,connect)&&index_seen==31&&generation_seen==7);
  unsigned before=calls;connect[3]="32";assert(central_command(5,connect)&&calls==before);
  connect[3]="0";connect[4]="0";assert(central_command(5,connect)&&calls==before);
  char *stop[]={"c6ble","central","stop","12"};
  assert(!central_command(4,stop)&&generation_seen==12);
  char *disconnect[]={"c6ble","central","disconnect","13"};
  assert(central_command(4,disconnect)&&generation_seen==13);
  char *read[]={"c6ble","central","read","14","65535"};
  assert(!central_command(5,read)&&request_seen.generation==14&&request_seen.operation==GLASS_GATT_READ&&request_seen.handle==65535);
  before=calls;read[4]="65536";assert(central_command(5,read)&&calls==before);
  read[4]="3trailing";assert(central_command(5,read)&&calls==before);
  read[4]="3";read[3]="2147483648";assert(central_command(5,read)&&calls==before);
  char *chars[]={"c6ble","central","characteristics","15","2","10"};
  assert(!central_command(6,chars)&&request_seen.start==2&&request_seen.end==10&&request_seen.operation==GLASS_GATT_CHARACTERISTICS);
  before=calls;chars[5]="1";assert(central_command(6,chars)&&calls==before);
  char *other[]={"c6ble","central","unknown","15"};
  assert(central_command(4,other)&&calls==before);
  other[2]="select";assert(!central_command(4,other)&&generation_seen==15);
  other[2]="pair";assert(!central_command(4,other)&&request_seen.operation==GLASS_GATT_PAIR);
  other[2]="services";assert(!central_command(4,other)&&request_seen.operation==GLASS_GATT_SERVICES);
  other[2]="mtu";assert(!central_command(4,other)&&request_seen.operation==GLASS_GATT_MTU);
  other[2]="notification";assert(central_command(4,other)&&generation_seen==15);
  other[2]="devices";assert(!central_command(3,other));
  char *channel[]={"c6ble","channel","send","4","Ab12"};
  assert(!channel_command(5,channel));before=calls;channel[4]="xyz";assert(channel_command(5,channel)&&calls==before);
  channel[4]="abgg";assert(channel_command(5,channel)&&calls==before);
  channel[4]="";assert(channel_command(5,channel)&&calls==before);
  channel[2]="receive";assert(!channel_command(4,channel));
  channel[3]="0";before=calls;assert(channel_command(4,channel)&&calls==before);
  channel[2]="status";assert(!channel_command(3,channel));
  char *input[]={"c6ble","mouse","input","begin","4","0","-1024","1024","0"};
  assert(!input_command(5,input));input[3]="report";input[4]="9";
  assert(!input_command(9,input));before=calls;input[6]="-1025";
  assert(input_command(9,input)&&calls==before);input[6]="-1024";input[5]="8";
  assert(input_command(9,input)&&calls==before);input[5]="0";input[4]="0";
  assert(input_command(9,input)&&calls==before);input[4]="2147483648";
  assert(input_command(9,input)&&calls==before);input[4]="9";input[3]="keepalive";
  assert(!input_command(5,input));input[3]="end";assert(!input_command(5,input));
  puts("BLE_CLI_OK: real parser; UUID byte order; bounded arguments/list; generation-bound dispatch; read hash only; unsupported commands rejected");
  return 0;
}
'''
(out / 'test.c').write_text(harness + parsed + cases)
flags = ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']
subprocess.run(['gcc', *flags, '-I'+str(ble), str(out/'test.c'), '-o', str(out/'test')], check=True)
subprocess.run([str(out/'test')], check=True,
               env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1', 'UBSAN_OPTIONS': 'halt_on_error=1'})
