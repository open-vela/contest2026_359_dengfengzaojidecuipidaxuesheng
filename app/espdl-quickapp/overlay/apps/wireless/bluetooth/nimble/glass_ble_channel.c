/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_ble_channel.h"
#include "glass_ble_hid_internal.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef GLASS_BLE_CHANNEL_TIMEOUT_MS
#define GLASS_BLE_CHANNEL_TIMEOUT_MS 3000
#endif
struct channel_frame {size_t length;uint8_t data[GLASS_BLE_CHANNEL_BYTES];};
struct channel {
  struct glass_ble_channel_state state;
  unsigned head;
  struct channel_frame rx[GLASS_BLE_CHANNEL_QUEUE],last_tx,request;
  bool pending;
  uint32_t request_epoch;
  int result;
};
static struct channel *g;
static uint16_t tx_handle;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t api=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t done=PTHREAD_COND_INITIALIZER;

static const ble_uuid128_t service_uuid=BLE_UUID128_INIT(
  0xbc,0x46,0xa0,0x16,0xcf,0x7a,0x03,0x8b,0x64,0x45,0x2a,0x5e,0x9d,0x4e,0xd1,0x69);
static const ble_uuid128_t rx_uuid=BLE_UUID128_INIT(
  0x2f,0xae,0xe8,0x21,0x51,0x09,0xc3,0xae,0x80,0x4b,0x85,0xba,0x09,0x5b,0x29,0xfe);
static const ble_uuid128_t tx_uuid=BLE_UUID128_INIT(
  0x38,0x81,0x7e,0x5f,0x37,0x0f,0x7a,0x8f,0x97,0x47,0xc1,0xb5,0x59,0x91,0xc8,0x9f);
static const ble_uuid128_t info_uuid=BLE_UUID128_INIT(
  0x54,0x9a,0x9c,0xe9,0x94,0x1d,0x13,0xb5,0xa9,0x4e,0x66,0x05,0xea,0xb6,0x6c,0xe8);

static int current_locked(uint32_t epoch)
{
  if(!g||!g->state.connected)return -ENOTCONN;
  if(epoch!=g->state.epoch)return -ESTALE;
  return g->state.encrypted?0:-EACCES;
}
static void clear_locked(void)
{
  g->head=g->state.queued=0;
  memset(g->rx,0,sizeof(g->rx));memset(&g->last_tx,0,sizeof(g->last_tx));
}
static int channel_access(uint16_t connection,uint16_t attribute,
                           struct ble_gatt_access_ctxt *ctxt,void *arg)
{
  (void)attribute;uintptr_t kind=(uintptr_t)arg;int ret=BLE_ATT_ERR_UNLIKELY;
  static const uint8_t info[8]={1,0,244,0,4,0,3,0};
  pthread_mutex_lock(&lock);
  if(!g||!g->state.connected||connection!=g->state.connection||!g->state.encrypted){
    pthread_mutex_unlock(&lock);return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
  }
  if(ctxt->op==BLE_GATT_ACCESS_OP_READ_CHR&&(kind==2||kind==3)){
    const void *data=kind==3?(const void *)info:g->last_tx.data;
    size_t length=kind==3?sizeof(info):g->last_tx.length;
    ret=os_mbuf_append(ctxt->om,data,length)?BLE_ATT_ERR_INSUFFICIENT_RES:0;
  }else if(ctxt->op==BLE_GATT_ACCESS_OP_WRITE_CHR&&kind==1){
    unsigned length=OS_MBUF_PKTLEN(ctxt->om);uint16_t copied=0;
    if(!length||length>GLASS_BLE_CHANNEL_BYTES)ret=BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    else if(g->state.queued==GLASS_BLE_CHANNEL_QUEUE)ret=BLE_ATT_ERR_INSUFFICIENT_RES;
    else{
      struct channel_frame *f=&g->rx[(g->head+g->state.queued)%GLASS_BLE_CHANNEL_QUEUE];
      if(ble_hs_mbuf_to_flat(ctxt->om,f->data,sizeof(f->data),&copied)||copied!=length)
        ret=BLE_ATT_ERR_UNLIKELY;
      else{f->length=copied;g->state.queued++;g->state.received++;ret=0;}
    }
    if(ret)g->state.rejected++;
  }
  pthread_mutex_unlock(&lock);return ret;
}
static const struct ble_gatt_chr_def characteristics[]={
  {.uuid=&rx_uuid.u,.access_cb=channel_access,.arg=(void *)(uintptr_t)1,
   .flags=BLE_GATT_CHR_F_WRITE|BLE_GATT_CHR_F_WRITE_ENC},
  {.uuid=&tx_uuid.u,.access_cb=channel_access,.arg=(void *)(uintptr_t)2,
   .flags=BLE_GATT_CHR_F_READ|BLE_GATT_CHR_F_READ_ENC|BLE_GATT_CHR_F_NOTIFY,.val_handle=&tx_handle},
  {.uuid=&info_uuid.u,.access_cb=channel_access,.arg=(void *)(uintptr_t)3,
   .flags=BLE_GATT_CHR_F_READ|BLE_GATT_CHR_F_READ_ENC},{0}
};
static const struct ble_gatt_svc_def services[]={
  {.type=BLE_GATT_SVC_TYPE_PRIMARY,.uuid=&service_uuid.u,.characteristics=characteristics},{0}
};
int glass_ble_channel_init(void)
{
  pthread_mutex_lock(&lock);
  if(g&&g->state.ready){pthread_mutex_unlock(&lock);return 0;}
  if(!g)g=calloc(1,sizeof(*g));
  if(!g){pthread_mutex_unlock(&lock);return -ENOMEM;}
  g->state.connection=BLE_HS_CONN_HANDLE_NONE;g->state.mtu=23;g->state.payload=20;
  pthread_mutex_unlock(&lock);
  int ret=ble_gatts_count_cfg(services);if(!ret)ret=ble_gatts_add_svcs(services);
  pthread_mutex_lock(&lock);g->state.ready=ret==0;pthread_mutex_unlock(&lock);return ret;
}
void glass_ble_channel_update(uint16_t connection,uint32_t epoch,bool connected,bool encrypted)
{
  pthread_mutex_lock(&lock);
  if(g){
    if(!connected||g->state.connection!=connection||g->state.epoch!=epoch){
      clear_locked();g->state.subscribed=false;
      if(g->pending){g->pending=false;g->result=-ENOTCONN;pthread_cond_broadcast(&done);}
      g->state.received=g->state.sent=g->state.rejected=0;g->state.error=0;
    }
    g->state.connection=connected?connection:BLE_HS_CONN_HANDLE_NONE;
    g->state.epoch=epoch;g->state.connected=connected;g->state.encrypted=connected&&encrypted;
    if(connected){unsigned mtu=ble_att_mtu(connection);g->state.mtu=mtu;
      g->state.payload=mtu>3?mtu-3:0;if(g->state.payload>GLASS_BLE_CHANNEL_BYTES)g->state.payload=GLASS_BLE_CHANNEL_BYTES;}
  }
  pthread_mutex_unlock(&lock);
}
void glass_ble_channel_subscribe(uint16_t connection,uint16_t attribute,bool notify)
{
  pthread_mutex_lock(&lock);
  if(g&&g->state.connected&&g->state.connection==connection&&attribute==tx_handle)
    g->state.subscribed=notify;
  pthread_mutex_unlock(&lock);
}
void glass_ble_channel_read(struct glass_ble_channel_state *state)
{
  if(!state)return;memset(state,0,sizeof(*state));pthread_mutex_lock(&lock);
  if(g)*state=g->state;pthread_mutex_unlock(&lock);
}
int glass_ble_channel_clear(uint32_t epoch)
{
  pthread_mutex_lock(&lock);int ret=current_locked(epoch);
  if(!ret)clear_locked();pthread_mutex_unlock(&lock);return ret;
}
int glass_ble_channel_receive(uint32_t epoch,uint8_t *data,size_t *length)
{
  if(!data||!length||!epoch)return -EINVAL;
  pthread_mutex_lock(&lock);int ret=current_locked(epoch);
  if(!ret){
    if(!g->state.queued)ret=-EAGAIN;
    else{struct channel_frame *f=g->rx+g->head;
      if(*length<f->length){*length=f->length;ret=-EMSGSIZE;}
      else{*length=f->length;memcpy(data,f->data,f->length);memset(f,0,sizeof(*f));
        g->head=(g->head+1)%GLASS_BLE_CHANNEL_QUEUE;g->state.queued--;}}
  }
  pthread_mutex_unlock(&lock);return ret;
}
/* Called only by the persistent host-group worker/watchdog. notify_custom
 * owns the mbuf even on failure. NOTIFY_TX is ignored by HID's GAP callback,
 * so synchronous completion cannot reenter this state lock. */
void glass_ble_channel_poll(void)
{
  pthread_mutex_lock(&lock);
  if(g&&g->pending){
    int ret=current_locked(g->request_epoch);
    if(!ret&&!g->state.subscribed)ret=-EAGAIN;
    unsigned mtu=!ret?ble_att_mtu(g->state.connection):0;
    if(!ret&&(mtu<=3||g->request.length>mtu-3))ret=-EMSGSIZE;
    if(!ret){struct os_mbuf *packet=ble_hs_mbuf_from_flat(g->request.data,g->request.length);
      if(!packet)ret=-ENOMEM;
      else{int rc=ble_gatts_notify_custom(g->state.connection,tx_handle,packet);ret=rc?-EIO:0;}}
    if(!ret){g->last_tx=g->request;g->state.sent++;}
    g->state.error=ret;g->result=ret;g->pending=false;memset(&g->request,0,sizeof(g->request));
    pthread_cond_broadcast(&done);
  }
  pthread_mutex_unlock(&lock);
}
int glass_ble_channel_send(uint32_t epoch,const uint8_t *data,size_t length,atomic_bool *cancel)
{
  if(cancel&&atomic_load(cancel))return -ECANCELED;
  if(!epoch||!data||!length)return -EINVAL;
  if(length>GLASS_BLE_CHANNEL_BYTES)return -EMSGSIZE;
  if(pthread_mutex_trylock(&api))return -EBUSY;
  pthread_mutex_lock(&lock);int ret=current_locked(epoch);
  if(!ret&&!g->state.subscribed)ret=-EAGAIN;
  if(!ret&&length>g->state.payload)ret=-EMSGSIZE;
  if(!ret){
    g->request.length=length;memcpy(g->request.data,data,length);g->request_epoch=epoch;
    g->result=-EINPROGRESS;g->pending=true;
    pthread_mutex_unlock(&lock);glass_ble_central_hid_wake();pthread_mutex_lock(&lock);
    struct timespec until={0};bool monotonic=clock_gettime(CLOCK_MONOTONIC,&until)==0;unsigned waits=0;
    until.tv_sec+=GLASS_BLE_CHANNEL_TIMEOUT_MS/1000;until.tv_nsec+=(GLASS_BLE_CHANNEL_TIMEOUT_MS%1000)*1000000;
    if(until.tv_nsec>=1000000000){until.tv_sec++;until.tv_nsec-=1000000000;}
    while(g->pending){
      struct timespec deadline,now;clock_gettime(CLOCK_REALTIME,&deadline);
      deadline.tv_nsec+=50000000;if(deadline.tv_nsec>=1000000000){deadline.tv_sec++;deadline.tv_nsec-=1000000000;}
      pthread_cond_timedwait(&done,&lock,&deadline);
      bool expired=monotonic&&clock_gettime(CLOCK_MONOTONIC,&now)==0?
        now.tv_sec>until.tv_sec||(now.tv_sec==until.tv_sec&&now.tv_nsec>=until.tv_nsec):
        ++waits>=(GLASS_BLE_CHANNEL_TIMEOUT_MS+49)/50;
      if(g->pending&&((cancel&&atomic_load(cancel))||expired)){
        g->pending=false;g->result=cancel&&atomic_load(cancel)?-ECANCELED:-ETIMEDOUT;
        memset(&g->request,0,sizeof(g->request));
      }
    }
    ret=g->result;
  }
  pthread_mutex_unlock(&lock);pthread_mutex_unlock(&api);return ret;
}
