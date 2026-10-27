/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "glass_ble_channel.h"
#include "host/ble_hs.h"
static const struct ble_gatt_svc_def *registered;
static atomic_bool running=true,paused,wakeup,fail_alloc;
static pthread_t worker;
static unsigned mtu=23,notifications,live_packets;
static int notify_error;
static uint8_t last_sent[GLASS_BLE_CHANNEL_BYTES];
static size_t last_length;
int ble_gatts_count_cfg(const struct ble_gatt_svc_def *defs){assert(defs[0].type==1&&!defs[1].type);return 0;}
int ble_gatts_add_svcs(const struct ble_gatt_svc_def *defs)
{registered=defs;assert(defs[0].characteristics[1].val_handle);*defs[0].characteristics[1].val_handle=20;return 0;}
uint16_t ble_att_mtu(uint16_t connection){assert(connection==7);return mtu;}
int os_mbuf_append(struct os_mbuf *om,const void *data,size_t length)
{if(length+om->len>sizeof(om->data))return 1;memcpy(om->data+om->len,data,length);om->len+=length;return 0;}
int ble_hs_mbuf_to_flat(const struct os_mbuf *om,void *out,uint16_t capacity,uint16_t *copied)
{if(om->len>capacity)return 1;memcpy(out,om->data,om->len);*copied=om->len;return 0;}
struct os_mbuf *ble_hs_mbuf_from_flat(const void *data,uint16_t length)
{assert(pthread_equal(worker,pthread_self()));if(atomic_load(&fail_alloc))return NULL;
 struct os_mbuf *m=calloc(1,sizeof(*m));assert(m);m->len=length;memcpy(m->data,data,length);live_packets++;return m;}
int ble_gatts_notify_custom(uint16_t connection,uint16_t attribute,struct os_mbuf *packet)
{assert(pthread_equal(worker,pthread_self())&&connection==7&&attribute==20);notifications++;
 last_length=packet->len;memcpy(last_sent,packet->data,last_length);free(packet);live_packets--;return notify_error;}
void glass_ble_central_hid_wake(void){atomic_store(&wakeup,true);}
static void *host(void *arg)
{(void)arg;while(atomic_load(&running)){if(!atomic_load(&paused)&&atomic_exchange(&wakeup,false))glass_ble_channel_poll();usleep(1000);}return NULL;}
static void wait_wake(void)
{for(unsigned i=0;i<1000&&!atomic_load(&wakeup);i++)usleep(1000);assert(atomic_load(&wakeup));}
static int service_access(unsigned kind,unsigned connection,int op,struct os_mbuf *om)
{const struct ble_gatt_chr_def *v=&registered[0].characteristics[kind];
 struct ble_gatt_access_ctxt ctx={.op=op,.om=om};return v->access_cb(connection,0,&ctx,v->arg);}
struct query {uint32_t epoch;uint8_t bytes[20];atomic_bool cancel;int result;};
static void *send_query(void *arg)
{struct query *q=arg;q->result=glass_ble_channel_send(q->epoch,q->bytes,sizeof(q->bytes),&q->cancel);return NULL;}
int main(void)
{
  assert(!pthread_create(&worker,NULL,host,NULL));uint8_t data[245]={0};size_t length=sizeof(data);
  assert(glass_ble_channel_send(1,data,20,NULL)==-ENOTCONN);
  assert(!glass_ble_channel_init());assert(registered[0].characteristics[0].flags&BLE_GATT_CHR_F_WRITE_ENC);
  assert(registered[0].characteristics[1].flags&BLE_GATT_CHR_F_READ_ENC);
  static const uint8_t expected_uuid[]={0xbc,0x46,0xa0,0x16,0xcf,0x7a,0x03,0x8b,0x64,0x45,0x2a,0x5e,0x9d,0x4e,0xd1,0x69};
  assert(!memcmp(((const ble_uuid128_t *)registered[0].uuid)->value,expected_uuid,16));
  glass_ble_channel_update(7,1,true,false);struct os_mbuf om={.len=20};
  assert(service_access(0,7,BLE_GATT_ACCESS_OP_WRITE_CHR,&om)==BLE_ATT_ERR_INSUFFICIENT_AUTHOR);
  assert(glass_ble_channel_send(1,data,20,NULL)==-EACCES);
  glass_ble_channel_update(7,1,true,true);
  assert(service_access(0,8,BLE_GATT_ACCESS_OP_WRITE_CHR,&om)==BLE_ATT_ERR_INSUFFICIENT_AUTHOR);
  om.len=0;assert(service_access(1,8,BLE_GATT_ACCESS_OP_READ_CHR,&om)==BLE_ATT_ERR_INSUFFICIENT_AUTHOR);
  for(unsigned i=0;i<4;i++){om.len=20;om.data[0]=i;assert(!service_access(0,7,BLE_GATT_ACCESS_OP_WRITE_CHR,&om));}
  assert(service_access(0,7,BLE_GATT_ACCESS_OP_WRITE_CHR,&om)==BLE_ATT_ERR_INSUFFICIENT_RES);
  om.len=245;assert(service_access(0,7,BLE_GATT_ACCESS_OP_WRITE_CHR,&om)==BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
  om.len=0;assert(service_access(0,7,BLE_GATT_ACCESS_OP_WRITE_CHR,&om)==BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
  struct glass_ble_channel_state state;glass_ble_channel_read(&state);assert(state.queued==4&&state.received==4&&state.rejected==3);
  assert(glass_ble_channel_receive(2,data,&length)==-ESTALE);
  length=1;assert(glass_ble_channel_receive(1,data,&length)==-EMSGSIZE&&length==20);
  for(unsigned i=0;i<4;i++){length=sizeof(data);assert(!glass_ble_channel_receive(1,data,&length)&&length==20&&data[0]==i);}
  assert(glass_ble_channel_receive(1,data,&length)==-EAGAIN);
  assert(glass_ble_channel_send(1,data,20,NULL)==-EAGAIN);
  glass_ble_channel_subscribe(8,20,true);glass_ble_channel_subscribe(7,21,true);
  glass_ble_channel_read(&state);assert(!state.subscribed);glass_ble_channel_subscribe(7,20,true);
  assert(glass_ble_channel_send(1,data,21,NULL)==-EMSGSIZE);
  assert(!glass_ble_channel_send(1,data,20,NULL)&&last_length==20&&notifications==1&&!live_packets);
  om.len=0;assert(!service_access(1,7,BLE_GATT_ACCESS_OP_READ_CHR,&om)&&om.len==20&&!memcmp(data,om.data,20));
  assert(!glass_ble_channel_clear(1));om.len=0;assert(!service_access(1,7,BLE_GATT_ACCESS_OP_READ_CHR,&om)&&!om.len);
  notify_error=1;assert(glass_ble_channel_send(1,data,20,NULL)==-EIO&&!live_packets);notify_error=0;
  atomic_store(&fail_alloc,true);assert(glass_ble_channel_send(1,data,20,NULL)==-ENOMEM&&!live_packets);atomic_store(&fail_alloc,false);
  puts("BLE_CHANNEL_ACCESS_OK: full production service; encrypted owner only; bounded RX FIFO/backpressure; MTU limit; notify/readback; mbuf ownership and errors");
  atomic_store(&paused,true);struct query q={.epoch=1,.bytes={42}};pthread_t caller;
  assert(!pthread_create(&caller,NULL,send_query,&q));wait_wake();
  assert(glass_ble_channel_send(1,data,20,NULL)==-EBUSY);q.bytes[0]=99;atomic_store(&paused,false);pthread_join(caller,NULL);
  assert(!q.result&&last_sent[0]==42);
  atomic_store(&paused,true);memset(&q,0,sizeof(q));q.epoch=1;
  assert(!pthread_create(&caller,NULL,send_query,&q));wait_wake();atomic_store(&q.cancel,true);pthread_join(caller,NULL);assert(q.result==-ECANCELED);
  unsigned before=notifications;atomic_store(&paused,false);usleep(20000);assert(notifications==before);
  atomic_store(&paused,true);assert(glass_ble_channel_send(1,data,20,NULL)==-ETIMEDOUT);
  atomic_store(&paused,false);usleep(20000);assert(notifications==before);
  atomic_store(&paused,true);memset(&q,0,sizeof(q));q.epoch=1;
  assert(!pthread_create(&caller,NULL,send_query,&q));wait_wake();glass_ble_channel_update(65535,2,false,false);
  glass_ble_channel_update(7,3,true,true);pthread_join(caller,NULL);assert(q.result==-ENOTCONN);
  glass_ble_channel_read(&state);assert(!state.subscribed&&!state.queued&&!state.received&&!state.sent);
  assert(glass_ble_channel_send(1,data,20,NULL)==-ESTALE);assert(glass_ble_channel_receive(1,data,&length)==-ESTALE);
  atomic_store(&paused,false);usleep(20000);assert(notifications==before);
  mtu=256;glass_ble_channel_update(7,3,true,true);glass_ble_channel_subscribe(7,20,true);
  assert(!glass_ble_channel_send(3,data,244,NULL)&&last_length==244);
  assert(glass_ble_channel_send(3,data,245,NULL)==-EMSGSIZE);
  om.len=0;assert(!service_access(2,7,BLE_GATT_ACCESS_OP_READ_CHR,&om)&&om.len==8&&om.data[0]==1&&om.data[2]==244);
  glass_ble_channel_update(65535,4,false,false);assert(glass_ble_channel_send(3,data,20,NULL)==-ENOTCONN);
  atomic_store(&running,false);pthread_join(worker,NULL);assert(!live_packets);
  puts("BLE_CHANNEL_MAILBOX_OK: persistent worker only; owned copy; busy; cancellation/timeout no replay; reset wakes pending caller; epoch and handle reuse reject stale sends; 244-byte negotiated path");
  return 0;
}
