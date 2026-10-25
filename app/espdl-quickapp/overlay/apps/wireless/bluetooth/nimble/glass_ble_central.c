/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_ble.h"
#include "glass_ble_central.h"
#include "glass_ble_hid_internal.h"
#include "glass_ble_name.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_store.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CENTRAL_ROLE 1
#define NOTIFICATIONS 8
struct notification {uint16_t handle,length;bool indication;uint8_t data[GLASS_GATT_BYTES];};
struct central {
  bool scanning,connecting,connected,encrypted,bonded,abandoned,pending,closing;
  int error,result;
  uint16_t conn,mtu;
  uint32_t generation,scan_generation,sequence,token,gap_token;
  unsigned count,head,queued,dropped;
  struct glass_ble_device devices[GLASS_BLE_LIMIT];
  struct glass_ble_device peer;
  bool peer_valid;
  bool scan_truncated;
  struct glass_gatt_result reply;
  struct notification notifications[NOTIFICATIONS];
  struct glass_gatt_request command_request;
  ble_addr_t command_peer;
  unsigned command;
  uint16_t command_conn;
  uint32_t command_token;
  bool command_pending,command_running;
  int command_result;
};
static struct central *g;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER,api=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t done=PTHREAD_COND_INITIALIZER;
static pthread_cond_t command_ready=PTHREAD_COND_INITIALIZER;
static pthread_mutex_t command_api=PTHREAD_MUTEX_INITIALIZER;
static bool hid_pending;
enum {HOST_SCAN=1,HOST_CONNECT,HOST_SCAN_CANCEL,HOST_CONNECT_CANCEL,HOST_TERMINATE,HOST_GATT};
static int gap(struct ble_gap_event *,void *);
static int gatt_issue(const struct glass_gatt_request *,uint16_t,void *);
void glass_ble_central_wait(void)
{
  pthread_mutex_lock(&lock);
  while(!hid_pending&&(!g||!g->command_pending||g->command_running))
    pthread_cond_wait(&command_ready,&lock);
  pthread_mutex_unlock(&lock);
}
void glass_ble_central_poll(void)
{
  pthread_mutex_lock(&lock);bool hid=hid_pending;hid_pending=false;pthread_mutex_unlock(&lock);
  if(hid)glass_ble_hid_poll();
  pthread_mutex_lock(&lock);
  if(!g||!g->command_pending||g->command_running){pthread_mutex_unlock(&lock);return;}
  g->command_running=true;unsigned op=g->command;uint16_t conn=g->command_conn;
  uint32_t token=g->command_token;ble_addr_t peer=g->command_peer;
  struct glass_gatt_request request=g->command_request;
  int ret=0;uint8_t own;
  if((op==HOST_SCAN&&(!g->scanning||token!=g->gap_token))||
     (op==HOST_CONNECT&&(!g->connecting||g->abandoned||token!=g->gap_token))||
     (op==HOST_GATT&&(!g->pending||!g->connected||g->closing||g->conn!=conn||request.generation!=g->generation)))ret=-ECANCELED;
  pthread_mutex_unlock(&lock);
  if(!ret&&(op==HOST_SCAN||op==HOST_CONNECT))ret=ble_hs_id_infer_auto(0,&own);
  if(!ret)switch(op){
  case HOST_SCAN:{struct ble_gap_disc_params p={.passive=0,.itvl=0x30,.window=0x20,.filter_duplicates=0};
    ret=ble_gap_disc(own,10000,&p,gap,(void *)(uintptr_t)token);break;}
  case HOST_CONNECT:ret=ble_gap_connect(own,&peer,10000,NULL,gap,(void *)(uintptr_t)token);break;
  case HOST_SCAN_CANCEL:ret=ble_gap_disc_cancel();break;
  case HOST_CONNECT_CANCEL:ret=ble_gap_conn_cancel();break;
  case HOST_TERMINATE:ret=ble_gap_terminate(conn,BLE_ERR_REM_USER_CONN_TERM);break;
  case HOST_GATT:ret=gatt_issue(&request,conn,(void *)(uintptr_t)token);break;
  default:ret=-EINVAL;break;
  }
  pthread_mutex_lock(&lock);g->command_result=ret;g->command_pending=g->command_running=false;
  pthread_cond_broadcast(&done);pthread_mutex_unlock(&lock);
}
void glass_ble_central_hid_wake(void)
{
  pthread_mutex_lock(&lock);hid_pending=true;
  pthread_cond_signal(&command_ready);pthread_mutex_unlock(&lock);
}
/* NuttX socket descriptors belong to the persistent host task group. Only
 * its event loop may issue GAP/GATT commands; caller-owned pointers are copied. */
static int dispatch(unsigned op,uint16_t conn,uint32_t token,const ble_addr_t *peer,const struct glass_gatt_request *request)
{
  pthread_mutex_lock(&command_api);pthread_mutex_lock(&lock);
  if(!g){pthread_mutex_unlock(&lock);pthread_mutex_unlock(&command_api);return -ENODEV;}
  g->command=op;g->command_conn=conn;g->command_token=token;
  if(peer)g->command_peer=*peer;if(request)g->command_request=*request;
  g->command_pending=true;g->command_running=false;
  pthread_cond_signal(&command_ready);
  /* The NPL event queue uses task-local mq descriptors. A persistent host
   * pthread drains this mailbox; callers never post through another
   * task group's descriptor table. */
  struct timespec deadline;clock_gettime(CLOCK_REALTIME,&deadline);deadline.tv_sec+=3;
  while(g->command_pending){int rc=pthread_cond_timedwait(&done,&lock,&deadline);
    if(rc==ETIMEDOUT&&!g->command_running){g->command_pending=false;g->command_result=-ETIMEDOUT;break;}
    if(rc==ETIMEDOUT){clock_gettime(CLOCK_REALTIME,&deadline);deadline.tv_sec+=3;}}
  int ret=g->command_result;pthread_mutex_unlock(&lock);pthread_mutex_unlock(&command_api);return ret;
}

static int posix_error(int rc)
{
  if(rc<0)return rc;
  if(!rc||rc==BLE_HS_EDONE)return 0;
  if(rc==BLE_HS_ENOTCONN)return -ENOTCONN;
  if(rc==BLE_HS_ETIMEOUT)return -ETIMEDOUT;
  if(rc==BLE_HS_EBUSY||rc==BLE_HS_EALREADY)return -EBUSY;
  if(rc==BLE_HS_ENOMEM)return -ENOMEM;
  if(rc==BLE_HS_EINVAL)return -EINVAL;
  return -EIO;
}
int glass_ble_central_init(void)
{
  pthread_mutex_lock(&lock);
  if(!g){g=calloc(1,sizeof(*g));if(g){g->conn=BLE_HS_CONN_HANDLE_NONE;g->mtu=23;}}
  int ret=g?0:-ENOMEM;pthread_mutex_unlock(&lock);return ret;
}
static void complete_locked(int result)
{g->result=result;g->pending=false;pthread_cond_broadcast(&done);}
void glass_ble_central_reset(void)
{
  pthread_mutex_lock(&lock);
  if(g){g->scanning=g->connecting=g->connected=g->encrypted=g->bonded=false;
    g->closing=false;g->peer_valid=false;g->gap_token++;
    g->conn=BLE_HS_CONN_HANDLE_NONE;g->generation++;g->queued=0;g->error=-EIO;
    if(g->pending)complete_locked(-EIO);}
  pthread_mutex_unlock(&lock);glass_ble_role_release(CENTRAL_ROLE);
}
void glass_ble_central_read(struct glass_ble_state *s)
{
  pthread_mutex_lock(&lock);
  if(g){s->scanning=g->scanning;s->connecting=g->connecting;s->connected=g->connected;
    if(!s->error)s->error=g->error;s->count=g->count;s->scan_truncated=g->scan_truncated;
    memcpy(s->devices,g->devices,sizeof(g->devices));
    s->generation=g->generation;s->scan_generation=g->scan_generation;
    s->mtu=g->mtu;s->encrypted=g->encrypted;s->bonded=g->bonded;
    s->peer_valid=g->peer_valid;s->peer=g->peer;s->disconnecting=g->closing;}
  pthread_mutex_unlock(&lock);
}
static int gap(struct ble_gap_event *event,void *arg)
{
  bool release=false,terminate=false;uint16_t conn=BLE_HS_CONN_HANDLE_NONE;
  pthread_mutex_lock(&lock);if(!g){pthread_mutex_unlock(&lock);return 0;}
  if((uint32_t)(uintptr_t)arg!=g->gap_token){
    pthread_mutex_unlock(&lock);
    if(event->type==BLE_GAP_EVENT_CONNECT&&!event->connect.status)
      (void)ble_gap_terminate(event->connect.conn_handle,BLE_ERR_REM_USER_CONN_TERM);
    return 0;
  }
  switch(event->type){
  case BLE_GAP_EVENT_DISC:
    if(g->scanning){
      unsigned i;for(i=0;i<g->count;i++)if(g->devices[i].type==event->disc.addr.type&&
        !memcmp(g->devices[i].address,event->disc.addr.val,6))break;
      struct glass_ble_device candidate={0};
      if(i<g->count)candidate=g->devices[i];
      else{
        candidate.type=event->disc.addr.type;memcpy(candidate.address,event->disc.addr.val,6);
        /* Called only by the persistent host callback, including controller-
         * resolved identity addresses. Never infer a bond from a name or RPA. */
        ble_addr_t peers[3];int count=0;
        int rc=ble_store_util_bonded_peers(peers,&count,3);
        if(!rc||rc==BLE_HS_ENOMEM)for(int n=0;n<count&&n<3;n++)
          if((peers[n].type&1)==(candidate.type&1)&&!memcmp(peers[n].val,candidate.address,6))
            candidate.bonded=true;
      }
      candidate.rssi=event->disc.rssi;
      glass_ble_update_advertising(&candidate,event->disc.event_type,event->disc.data,event->disc.length_data);
      if(i==g->count&&g->count==GLASS_BLE_LIMIT){
        g->scan_truncated=true;
        unsigned victim=0;
        for(unsigned n=1;n<g->count;n++)
          if(glass_ble_device_priority(g->devices+n)<glass_ble_device_priority(g->devices+victim))victim=n;
        if(!g->devices[victim].bonded&&glass_ble_device_priority(&candidate)>
           glass_ble_device_priority(g->devices+victim))i=victim;
      }
      if(i<GLASS_BLE_LIMIT){if(i==g->count)g->count++;g->devices[i]=candidate;}
    }break;
  case BLE_GAP_EVENT_DISC_COMPLETE:
    g->scanning=false;release=!g->connecting&&!g->connected;break;
  case BLE_GAP_EVENT_CONNECT:
    g->connecting=false;
    if(event->connect.status){g->error=g->abandoned?-ECANCELED:posix_error(event->connect.status);g->peer_valid=false;release=true;}
    else if(g->abandoned){terminate=true;conn=event->connect.conn_handle;g->conn=conn;g->closing=true;}
    else {g->conn=event->connect.conn_handle;g->connected=true;g->generation++;
      g->encrypted=g->bonded=false;g->mtu=ble_att_mtu(g->conn);g->queued=g->head=g->dropped=0;g->error=0;}
    break;
  case BLE_GAP_EVENT_DISCONNECT:
    if(event->disconnect.conn.conn_handle==g->conn){
      g->connecting=g->connected=g->encrypted=g->bonded=false;g->conn=BLE_HS_CONN_HANDLE_NONE;
      g->closing=false;
      g->peer_valid=false;
      g->generation++;g->queued=0;if(g->pending)complete_locked(-ENOTCONN);release=true;}
    break;
  case BLE_GAP_EVENT_ENC_CHANGE:
    if(event->enc_change.conn_handle==g->conn){struct ble_gap_conn_desc desc;
      int rc=ble_gap_conn_find(g->conn,&desc);
      if(!rc){g->encrypted=desc.sec_state.encrypted;g->bonded=desc.sec_state.bonded;
        if(g->peer_valid&&g->bonded){g->peer.bonded=true;
          for(unsigned i=0;i<g->count;i++)if(g->devices[i].type==g->peer.type&&
            !memcmp(g->devices[i].address,g->peer.address,6))g->devices[i].bonded=true;}}
      if(g->pending&&g->token==0)complete_locked(event->enc_change.status?-EACCES:(!rc&&g->encrypted?0:-EACCES));
    }break;
  case BLE_GAP_EVENT_MTU:
    if(event->mtu.conn_handle==g->conn)g->mtu=event->mtu.value;break;
  case BLE_GAP_EVENT_NOTIFY_RX:
    if(g->connected&&event->notify_rx.conn_handle==g->conn){
      unsigned length=OS_MBUF_PKTLEN(event->notify_rx.om);
      if(length>GLASS_GATT_BYTES){g->dropped++;break;}
      if(g->queued==NOTIFICATIONS){g->head=(g->head+1)%NOTIFICATIONS;g->queued--;g->dropped++;}
      struct notification *n=&g->notifications[(g->head+g->queued)%NOTIFICATIONS];uint16_t copied=0;
      if(!ble_hs_mbuf_to_flat(event->notify_rx.om,n->data,sizeof(n->data),&copied)){
        n->handle=event->notify_rx.attr_handle;n->length=copied;n->indication=event->notify_rx.indication;g->queued++;}
      else g->dropped++;
    }break;
  case BLE_GAP_EVENT_REPEAT_PAIRING:
    /* Never silently delete an existing bond to accept a new identity. */
    pthread_mutex_unlock(&lock);return BLE_GAP_REPEAT_PAIRING_IGNORE;
  default:break;
  }
  pthread_mutex_unlock(&lock);
  if(terminate){int rc=ble_gap_terminate(conn,BLE_ERR_REM_USER_CONN_TERM);if(rc)release=true;}
  if(release)glass_ble_role_release(CENTRAL_ROLE);
  return 0;
}
int glass_ble_central_scan(void)
{
  int ret=glass_ble_host_wait();if(ret)return ret;
  if(pthread_mutex_trylock(&api))return -EBUSY;
  ret=glass_ble_role_claim(CENTRAL_ROLE);if(ret)goto out;
  pthread_mutex_lock(&lock);
  if(!g||g->scanning||g->connecting||g->connected||g->closing){ret=-EBUSY;pthread_mutex_unlock(&lock);goto out;}
  memset(g->devices,0,sizeof(g->devices));g->count=0;g->scan_truncated=false;g->scan_generation++;g->scanning=true;g->error=0;
  uint32_t token=++g->gap_token;
  pthread_mutex_unlock(&lock);
  ret=dispatch(HOST_SCAN,0,token,NULL,NULL);
  if(ret){pthread_mutex_lock(&lock);g->scanning=false;g->error=posix_error(ret);pthread_mutex_unlock(&lock);glass_ble_role_release(CENTRAL_ROLE);ret=posix_error(ret);}
out:pthread_mutex_unlock(&api);return ret;
}
int glass_ble_central_connect(unsigned index,uint32_t scan_generation)
{
  int ret=glass_ble_host_wait();if(ret)return ret;
  if(pthread_mutex_trylock(&api))return -EBUSY;
  ret=glass_ble_role_claim(CENTRAL_ROLE);if(ret)goto out;
  ble_addr_t peer;pthread_mutex_lock(&lock);
  uint32_t token=0;
  if(!g||g->scanning||g->connecting||g->connected||g->closing)ret=-EBUSY;
  else if(index>=g->count)ret=-EINVAL;
  else if(scan_generation&&scan_generation!=g->scan_generation)ret=-ESTALE;
  else if(g->devices[index].connectable_known&&!g->devices[index].connectable)ret=-ENOTSUP;
  else{peer.type=g->devices[index].type;memcpy(peer.val,g->devices[index].address,6);
    g->peer=g->devices[index];g->peer_valid=true;
    g->connecting=true;g->abandoned=false;g->error=0;token=++g->gap_token;}
  bool busy=g&&(g->scanning||g->connecting||g->connected||g->closing);pthread_mutex_unlock(&lock);
  if(ret){if(!busy)glass_ble_role_release(CENTRAL_ROLE);goto out;}
  ret=dispatch(HOST_CONNECT,0,token,&peer,NULL);
  if(ret){pthread_mutex_lock(&lock);g->connecting=false;g->peer_valid=false;g->error=posix_error(ret);pthread_mutex_unlock(&lock);glass_ble_role_release(CENTRAL_ROLE);ret=posix_error(ret);}
out:pthread_mutex_unlock(&api);return ret;
}
int glass_ble_central_cancel(void)
{
  pthread_mutex_lock(&lock);
  bool scan=g&&g->scanning,connect=g&&g->connecting;
  bool pending=g&&g->pending;
  if(g)g->abandoned=true;uint16_t conn=g&&pending?g->conn:BLE_HS_CONN_HANDLE_NONE;
  if(scan){g->scanning=false;g->gap_token++;}
  if(pending)g->closing=true;
  if(g&&g->pending)complete_locked(-ECANCELED);
  pthread_mutex_unlock(&lock);
  if(scan){(void)dispatch(HOST_SCAN_CANCEL,0,0,NULL,NULL);
    if(!connect)glass_ble_role_release(CENTRAL_ROLE);}
  if(connect)(void)dispatch(HOST_CONNECT_CANCEL,0,0,NULL,NULL);
  if(conn!=BLE_HS_CONN_HANDLE_NONE)(void)dispatch(HOST_TERMINATE,conn,0,NULL,NULL);
  return 0;
}
int glass_ble_central_disconnect(void)
{
  glass_ble_central_cancel();pthread_mutex_lock(&lock);
  uint16_t conn=g&&!g->closing?g->conn:BLE_HS_CONN_HANDLE_NONE;
  uint32_t generation=g?g->generation:0;
  if(g&&conn!=BLE_HS_CONN_HANDLE_NONE){g->closing=true;g->error=0;}pthread_mutex_unlock(&lock);
  int ret=conn==BLE_HS_CONN_HANDLE_NONE?0:posix_error(dispatch(HOST_TERMINATE,conn,0,NULL,NULL));
  if(ret){
    pthread_mutex_lock(&lock);
    if(g&&g->conn==conn&&g->generation==generation){g->closing=false;g->error=ret;}
    pthread_mutex_unlock(&lock);
  }
  return ret;
}
static bool callback_locked(uint16_t conn,void *arg)
{return g&&g->pending&&g->connected&&g->conn==conn&&g->token==(uint32_t)(uintptr_t)arg&&g->reply.generation==g->generation;}
static int discovery(uint16_t conn,const struct ble_gatt_error *error,
                     const ble_uuid_t *uuid,uint16_t start,uint16_t end,uint16_t value,uint8_t properties,void *arg)
{
  pthread_mutex_lock(&lock);
  if(callback_locked(conn,arg)){
    if(error->status)complete_locked(posix_error(error->status));
    else if(uuid){if(g->reply.count<GLASS_GATT_ITEMS){struct glass_gatt_item *i=g->reply.items+g->reply.count++;
      i->start=start;i->end=end;i->value=value;i->properties=properties;ble_uuid_to_str(uuid,i->uuid);}
      else g->reply.truncated=true;}
  }
  pthread_mutex_unlock(&lock);return 0;
}
static int service_cb(uint16_t c,const struct ble_gatt_error *e,const struct ble_gatt_svc *v,void *arg)
{return discovery(c,e,v?&v->uuid.u:NULL,v?v->start_handle:0,v?v->end_handle:0,0,0,arg);}
static int characteristic_cb(uint16_t c,const struct ble_gatt_error *e,const struct ble_gatt_chr *v,void *arg)
{return discovery(c,e,v?&v->uuid.u:NULL,v?v->def_handle:0,0,v?v->val_handle:0,v?v->properties:0,arg);}
static int descriptor_cb(uint16_t c,const struct ble_gatt_error *e,uint16_t value,const struct ble_gatt_dsc *v,void *arg)
{return discovery(c,e,v?&v->uuid.u:NULL,v?v->handle:0,0,value,0,arg);}
static int attribute_cb(uint16_t conn,const struct ble_gatt_error *error,struct ble_gatt_attr *attr,void *arg)
{
  pthread_mutex_lock(&lock);
  if(callback_locked(conn,arg)){
    int ret=posix_error(error->status);
    if(!ret&&attr&&attr->om){uint16_t copied=0;unsigned length=OS_MBUF_PKTLEN(attr->om);
      if(length>sizeof(g->reply.data))ret=-EMSGSIZE;
      else if(ble_hs_mbuf_to_flat(attr->om,g->reply.data,sizeof(g->reply.data),&copied))ret=-EIO;
      else{g->reply.length=copied;g->reply.handle=attr->handle;}}
    complete_locked(ret);
  }
  pthread_mutex_unlock(&lock);return 0;
}
static int mtu_cb(uint16_t conn,const struct ble_gatt_error *error,uint16_t mtu,void *arg)
{
  pthread_mutex_lock(&lock);if(callback_locked(conn,arg)){if(!error->status)g->mtu=mtu;
    complete_locked(posix_error(error->status));}pthread_mutex_unlock(&lock);return 0;
}
static int gatt_issue(const struct glass_gatt_request *r,uint16_t conn,void *arg)
{
  int ret=0;
  switch(r->operation){
  case GLASS_GATT_SERVICES:ret=ble_gattc_disc_all_svcs(conn,service_cb,arg);break;
  case GLASS_GATT_CHARACTERISTICS:ret=ble_gattc_disc_all_chrs(conn,r->start,r->end,characteristic_cb,arg);break;
  case GLASS_GATT_DESCRIPTORS:ret=ble_gattc_disc_all_dscs(conn,r->start,r->end,descriptor_cb,arg);break;
  case GLASS_GATT_READ:ret=ble_gattc_read(conn,r->handle,attribute_cb,arg);break;
  case GLASS_GATT_WRITE:
    if(!r->response){ret=ble_gattc_write_no_rsp_flat(conn,r->handle,r->data,r->length);
      pthread_mutex_lock(&lock);complete_locked(posix_error(ret));pthread_mutex_unlock(&lock);}
    else ret=ble_gattc_write_flat(conn,r->handle,r->data,r->length,attribute_cb,arg);break;
  case GLASS_GATT_SUBSCRIBE:ret=ble_gattc_write_flat(conn,r->handle,r->data,2,attribute_cb,arg);break;
  case GLASS_GATT_PAIR:
    pthread_mutex_lock(&lock);bool encrypted=g->encrypted;pthread_mutex_unlock(&lock);
    if(encrypted){pthread_mutex_lock(&lock);complete_locked(0);pthread_mutex_unlock(&lock);}
    else ret=ble_gap_security_initiate(conn);break;
  case GLASS_GATT_MTU:ret=ble_gattc_exchange_mtu(conn,mtu_cb,arg);break;
  }
  return ret;
}
int glass_ble_gatt(const struct glass_gatt_request *r,struct glass_gatt_result *out,atomic_bool *cancel)
{
  if(cancel&&atomic_load(cancel))return -ECANCELED;
  if(!r||!out||r->operation>GLASS_GATT_MTU||!r->generation||r->length>GLASS_GATT_BYTES)return -EINVAL;
  if((r->operation==GLASS_GATT_CHARACTERISTICS||r->operation==GLASS_GATT_DESCRIPTORS)&&(!r->start||r->start>r->end))return -EINVAL;
  if((r->operation==GLASS_GATT_READ||r->operation==GLASS_GATT_WRITE||r->operation==GLASS_GATT_SUBSCRIBE)&&!r->handle)return -EINVAL;
  if(r->operation==GLASS_GATT_SUBSCRIBE&&(r->length!=2||r->data[1]||r->data[0]>2))return -EINVAL;
  if(pthread_mutex_trylock(&api))return -EBUSY;
  pthread_mutex_lock(&lock);int ret=0;uint16_t conn=0;uint32_t token=0;
  if(!g||!g->connected||g->closing)ret=-ENOTCONN;
  else if(r->generation!=g->generation)ret=-ESTALE;
  else if(g->sequence==UINT32_MAX)ret=-EOVERFLOW;
  else{memset(&g->reply,0,sizeof(g->reply));g->reply.generation=g->generation;
    g->pending=true;g->result=-EINPROGRESS;conn=g->conn;
    token=++g->sequence;g->token=r->operation==GLASS_GATT_PAIR?0:token;}
  pthread_mutex_unlock(&lock);if(ret)goto out;
  ret=dispatch(HOST_GATT,conn,token,NULL,r);
  pthread_mutex_lock(&lock);
  if(ret&&g->pending)complete_locked(posix_error(ret));
  unsigned waits=0;
  while(g->pending){struct timespec deadline;clock_gettime(CLOCK_REALTIME,&deadline);
    deadline.tv_nsec+=50000000;if(deadline.tv_nsec>=1000000000){deadline.tv_sec++;deadline.tv_nsec-=1000000000;}
    pthread_cond_timedwait(&done,&lock,&deadline);
    if((cancel&&atomic_load(cancel))||++waits>=300){complete_locked(cancel&&atomic_load(cancel)?-ECANCELED:-ETIMEDOUT);
      /* Invalidate callback identity before cancelling the host procedure. */
      g->token=UINT32_MAX;g->closing=true;pthread_mutex_unlock(&lock);(void)dispatch(HOST_TERMINATE,conn,0,NULL,NULL);pthread_mutex_lock(&lock);break;}
  }
  ret=g->result;g->reply.mtu=g->mtu;if(!ret)*out=g->reply;pthread_mutex_unlock(&lock);
out:pthread_mutex_unlock(&api);return ret;
}
int glass_ble_notification(uint32_t generation,struct glass_gatt_result *out)
{
  if(!out||!generation)return -EINVAL;pthread_mutex_lock(&lock);int ret=0;
  if(!g||!g->connected)ret=-ENOTCONN;else if(generation!=g->generation)ret=-ESTALE;
  else if(!g->queued)ret=-EAGAIN;
  else{struct notification *n=g->notifications+g->head;memset(out,0,sizeof(*out));
    out->generation=g->generation;out->handle=n->handle;out->length=n->length;out->indication=n->indication;
    out->dropped=g->dropped;memcpy(out->data,n->data,n->length);g->head=(g->head+1)%NOTIFICATIONS;g->queued--;}
  pthread_mutex_unlock(&lock);return ret;
}
