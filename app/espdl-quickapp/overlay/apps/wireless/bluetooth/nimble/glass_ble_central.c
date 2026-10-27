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

#define NOTIFICATIONS 8
struct notification {uint16_t handle,length;bool indication;uint8_t data[GLASS_GATT_BYTES];};
struct link {
  pthread_mutex_t api;
  bool active,connecting,connected,encrypted,bonded,abandoned,pending,closing,busy;
  bool mouse_paused,restore_security;
  int error,result;
  uint16_t conn,mtu;
  uint32_t generation,token,gap_token;
  enum glass_gatt_operation operation;
  struct glass_ble_device peer;
  struct glass_gatt_result reply;
  struct notification notifications[NOTIFICATIONS];
  unsigned head,queued,dropped;
};
struct central {
  bool scanning,scan_abandoned,scan_filter_valid,scan_truncated;
  bool mouse_paused;
  bool restoring;
  unsigned restore_count;
  struct glass_ble_restore_result restores[GLASS_BLE_CENTRAL_LIMIT];
  int error;
  uint32_t sequence,scan_generation,scan_token;
  unsigned count,selected;
  uint8_t scan_service[16];
  struct glass_ble_device devices[GLASS_BLE_LIMIT];
  struct link links[GLASS_BLE_CENTRAL_LIMIT];
  struct glass_gatt_request command_request;
  ble_addr_t command_peer;
  unsigned command;
  uint16_t command_conn;
  uint32_t command_token;
  bool command_pending,command_running;
  int command_result;
};
/* Large reply/notification storage is zero-initialized in the user heap. */
static struct central *g;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t command_api=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t scan_api=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t connect_api=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mouse_api=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t done=PTHREAD_COND_INITIALIZER;
static pthread_cond_t command_ready=PTHREAD_COND_INITIALIZER;
static bool hid_pending;
enum {HOST_SCAN=1,HOST_CONNECT,HOST_SCAN_CANCEL,HOST_CONNECT_CANCEL,HOST_TERMINATE,HOST_GATT};
static int gap(struct ble_gap_event *,void *);
static int scan_gap(struct ble_gap_event *,void *);
static int gatt_issue(const struct glass_gatt_request *,uint16_t,void *);

static uint32_t next_locked(void)
{return g->sequence==UINT32_MAX?0:++g->sequence;}
static struct link *link_locked(uint32_t generation)
{
  if(g&&generation)for(unsigned i=0;i<GLASS_BLE_CENTRAL_LIMIT;i++)
    if(g->links[i].generation==generation)return &g->links[i];
  return NULL;
}
static unsigned active_locked(void)
{
  unsigned n=0;
  if(g)for(unsigned i=0;i<GLASS_BLE_CENTRAL_LIMIT;i++)n+=g->links[i].active;
  return n;
}
static unsigned capacity_locked(void)
{
  unsigned n=glass_ble_capacity();
  /* HID and ordinary peers are mutually exclusive. The ordinary role can
   * use the entire host pool while HID is stopped. */
  return n<GLASS_BLE_CENTRAL_LIMIT?n:GLASS_BLE_CENTRAL_LIMIT;
}
static int reserve_locked(unsigned extra)
{
  unsigned n=extra;
  if(g)for(unsigned i=0;i<GLASS_BLE_CENTRAL_LIMIT;i++){
    const struct link *l=&g->links[i];
    n+=l->active&&(!l->mouse_paused||l->connecting||l->connected);
  }
  if(!n&&g&&g->scanning)n=1;
  return glass_ble_central_reserve(n);
}
static bool same_peer(const struct glass_ble_device *a,const struct glass_ble_device *b)
{return (a->type&1)==(b->type&1)&&!memcmp(a->address,b->address,6);}
static bool peer_active_locked(const struct glass_ble_device *peer)
{
  for(unsigned i=0;i<GLASS_BLE_CENTRAL_LIMIT;i++)
    if(g->links[i].active&&same_peer(peer,&g->links[i].peer))return true;
  return false;
}
static struct link *focused_locked(void)
{
  if(!g)return NULL;
  if(!g->links[g->selected].active)for(unsigned i=0;i<GLASS_BLE_CENTRAL_LIMIT;i++)
    if(g->links[i].active){g->selected=i;break;}
  return &g->links[g->selected];
}
static void complete_locked(struct link *l,int result)
{l->result=result;l->pending=false;pthread_cond_broadcast(&done);}
static void retire_locked(struct link *l,int error)
{
  l->active=l->connecting=l->connected=l->encrypted=l->bonded=l->closing=false;
  l->mouse_paused=false;
  l->restore_security=false;
  l->conn=BLE_HS_CONN_HANDLE_NONE;l->queued=0;l->error=error;
  if(l->pending)complete_locked(l,error?error:-ENOTCONN);
  reserve_locked(0);
  pthread_cond_broadcast(&done);
}
static void paused_locked(struct link *l)
{
  l->connecting=l->connected=l->encrypted=l->bonded=l->closing=false;
  l->conn=BLE_HS_CONN_HANDLE_NONE;l->queued=l->head=0;
  if(l->pending)complete_locked(l,-ECANCELED);
  reserve_locked(0);pthread_cond_broadcast(&done);
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
void glass_ble_central_wait(void)
{
  pthread_mutex_lock(&lock);
  while(!hid_pending&&(!g||!g->command_pending||g->command_running))
    pthread_cond_wait(&command_ready,&lock);
  pthread_mutex_unlock(&lock);
}
void glass_ble_central_hid_wake(void)
{
  pthread_mutex_lock(&lock);hid_pending=true;
  pthread_cond_signal(&command_ready);pthread_mutex_unlock(&lock);
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
  struct link *l=link_locked(op==HOST_GATT?request.generation:token);
  uint32_t gap_token=l?l->gap_token:0;
  int ret=0;uint8_t own=0;
  if((op==HOST_SCAN&&(!g->scanning||g->scan_abandoned||token!=g->scan_token))||
     (op==HOST_SCAN_CANCEL&&token!=g->scan_token)||
     (op==HOST_CONNECT&&(!l||!l->active||!l->connecting||l->abandoned))||
     (op==HOST_CONNECT_CANCEL&&(!l||!l->active||!l->connecting))||
     (op==HOST_TERMINATE&&(!l||!l->active||l->conn!=conn||!l->connected))||
     (op==HOST_GATT&&(!l||!l->pending||!l->connected||l->closing||l->conn!=conn||l->token!=token)))ret=-ECANCELED;
  pthread_mutex_unlock(&lock);
  if(!ret&&(op==HOST_SCAN||op==HOST_CONNECT))ret=ble_hs_id_infer_auto(0,&own);
  if(!ret)switch(op){
  case HOST_SCAN:{struct ble_gap_disc_params p={.passive=0,.itvl=0x30,.window=0x20,.filter_duplicates=0};
    ret=ble_gap_disc(own,10000,&p,scan_gap,(void *)(uintptr_t)token);break;}
  case HOST_CONNECT:ret=ble_gap_connect(own,&peer,10000,NULL,gap,(void *)(uintptr_t)gap_token);break;
  case HOST_SCAN_CANCEL:ret=ble_gap_disc_cancel();if(ret==BLE_HS_EALREADY)ret=0;break;
  case HOST_CONNECT_CANCEL:ret=ble_gap_conn_cancel();if(ret==BLE_HS_EALREADY)ret=0;break;
  case HOST_TERMINATE:ret=ble_gap_terminate(conn,BLE_ERR_REM_USER_CONN_TERM);if(ret==BLE_HS_EALREADY)ret=0;break;
  case HOST_GATT:ret=gatt_issue(&request,conn,(void *)(uintptr_t)token);break;
  default:ret=-EINVAL;break;
  }
  pthread_mutex_lock(&lock);g->command_result=ret;g->command_pending=g->command_running=false;
  pthread_cond_broadcast(&done);pthread_mutex_unlock(&lock);
}
/* Only the persistent host-group worker signs GAP/GATT commands. A bounded
 * owned copy crosses the mailbox; no caller pointers or NPL descriptors do. */
static int dispatch(unsigned op,uint16_t conn,uint32_t token,const ble_addr_t *peer,const struct glass_gatt_request *request)
{
  pthread_mutex_lock(&command_api);pthread_mutex_lock(&lock);
  if(!g){pthread_mutex_unlock(&lock);pthread_mutex_unlock(&command_api);return -ENODEV;}
  g->command=op;g->command_conn=conn;g->command_token=token;
  if(peer)g->command_peer=*peer;if(request)g->command_request=*request;
  g->command_pending=true;g->command_running=false;pthread_cond_signal(&command_ready);
  struct timespec deadline;clock_gettime(CLOCK_REALTIME,&deadline);deadline.tv_sec+=3;
  while(g->command_pending){int rc=pthread_cond_timedwait(&done,&lock,&deadline);
    if(rc==ETIMEDOUT&&!g->command_running){g->command_pending=false;g->command_result=-ETIMEDOUT;break;}
    if(rc==ETIMEDOUT){clock_gettime(CLOCK_REALTIME,&deadline);deadline.tv_sec+=3;}}
  int ret=g->command_result;pthread_mutex_unlock(&lock);pthread_mutex_unlock(&command_api);return ret;
}
int glass_ble_central_init(void)
{
  pthread_mutex_lock(&lock);int ret=0;
  if(!g){
    struct central *fresh=calloc(1,sizeof(*fresh));
    if(!fresh)ret=-ENOMEM;
    else{
      unsigned i=0;
      for(;i<GLASS_BLE_CENTRAL_LIMIT;i++){
        ret=pthread_mutex_init(&fresh->links[i].api,NULL);if(ret)break;
        fresh->links[i].conn=BLE_HS_CONN_HANDLE_NONE;fresh->links[i].mtu=23;
      }
      if(ret){while(i)pthread_mutex_destroy(&fresh->links[--i].api);free(fresh);ret=-ret;}
      else g=fresh;
    }
  }
  pthread_mutex_unlock(&lock);return ret;
}
void glass_ble_central_reset(void)
{
  pthread_mutex_lock(&lock);
  if(g){g->mouse_paused=false;
    if(g->restoring)for(unsigned i=0;i<g->restore_count;i++){
      g->restores[i].connected=false;g->restores[i].error=-EIO;}
    g->restoring=false;
    g->scanning=false;g->scan_abandoned=true;g->scan_token=next_locked();g->error=-EIO;
    for(unsigned i=0;i<GLASS_BLE_CENTRAL_LIMIT;i++)retire_locked(&g->links[i],-EIO);}
  reserve_locked(0);pthread_mutex_unlock(&lock);
}
void glass_ble_central_read(struct glass_ble_state *s)
{
  if(!s)return;
  pthread_mutex_lock(&lock);
  if(g){
    s->scanning=g->scanning;s->mouse_paused=g->mouse_paused;if(!s->error)s->error=g->error;
    s->restoring=g->restoring;s->restore_count=g->restore_count;
    memcpy(s->restores,g->restores,sizeof(s->restores));
    s->count=g->count;s->scan_truncated=g->scan_truncated;s->scan_generation=g->scan_generation;
    memcpy(s->devices,g->devices,sizeof(g->devices));s->central_capacity=capacity_locked();
    s->session_count=s->connection_count=0;
    memset(s->sessions,0,sizeof(s->sessions));
    for(unsigned i=0;i<GLASS_BLE_CENTRAL_LIMIT;i++){
      const struct link *l=&g->links[i];if(!l->active)continue;
      struct glass_ble_session *v=&s->sessions[s->session_count++];
      v->generation=l->generation;v->mtu=l->mtu;v->connecting=l->connecting;v->connected=l->connected;
      v->encrypted=l->encrypted;v->bonded=l->bonded;v->disconnecting=l->closing;v->peer=l->peer;v->error=l->error;
      v->mouse_paused=l->mouse_paused;
      s->connection_count+=l->connected;
    }
    const struct link *l=focused_locked();
    s->connecting=l->connecting;s->connected=l->connected;s->generation=l->generation;s->mtu=l->mtu;
    s->encrypted=l->encrypted;s->bonded=l->bonded;s->disconnecting=l->closing;s->peer_valid=l->active;s->peer=l->peer;
    if(!s->error)s->error=l->error;
  }
  pthread_mutex_unlock(&lock);
}
static int scan_gap(struct ble_gap_event *event,void *arg)
{
  bool release=false;
  pthread_mutex_lock(&lock);
  if(!g||(uint32_t)(uintptr_t)arg!=g->scan_token){pthread_mutex_unlock(&lock);return 0;}
  switch(event->type){
  case BLE_GAP_EVENT_DISC:
    if(g->scanning&&!g->scan_abandoned){
      unsigned i;for(i=0;i<g->count;i++)if(g->devices[i].type==event->disc.addr.type&&
        !memcmp(g->devices[i].address,event->disc.addr.val,6))break;
      if(i==g->count&&g->scan_filter_valid&&
         !glass_ble_has_service128(event->disc.data,event->disc.length_data,g->scan_service))break;
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
        unsigned victim=GLASS_BLE_LIMIT;
        for(unsigned n=0;n<g->count;n++)
          if(!peer_active_locked(g->devices+n)&&(victim==GLASS_BLE_LIMIT||
             glass_ble_device_priority(g->devices+n)<glass_ble_device_priority(g->devices+victim)))victim=n;
        if(victim<GLASS_BLE_LIMIT&&!g->devices[victim].bonded&&glass_ble_device_priority(&candidate)>
           glass_ble_device_priority(g->devices+victim))i=victim;
      }
      if(i<GLASS_BLE_LIMIT){if(i==g->count)g->count++;g->devices[i]=candidate;}
    }break;
  case BLE_GAP_EVENT_DISC_COMPLETE:
    g->scanning=false;release=true;break;
  default:break;
  }
  if(release)reserve_locked(0);
  pthread_mutex_unlock(&lock);
  return 0;
}
/* Connection callbacks carry a non-reused physical GAP token, never a slot
 * index. An old success callback must not terminate a handle now owned by a
 * different central session or the HID service. Cancelled connects retain
 * their record until their actual terminal event has arrived. */
static int gap(struct ble_gap_event *event,void *arg)
{
  uint32_t generation=0;
  bool terminate=false,secure=false;uint16_t conn=BLE_HS_CONN_HANDLE_NONE;
  pthread_mutex_lock(&lock);struct link *l=NULL;
  if(g)for(unsigned i=0;i<GLASS_BLE_CENTRAL_LIMIT;i++)
    if(g->links[i].active&&g->links[i].gap_token==(uint32_t)(uintptr_t)arg)
      {l=&g->links[i];generation=l->generation;break;}
  if(!l||!l->active){pthread_mutex_unlock(&lock);return 0;}
  switch(event->type){
  case BLE_GAP_EVENT_CONNECT:
    if(!l->connecting)break;
    l->connecting=false;
    if(event->connect.status){
      int error=l->abandoned?-ECANCELED:posix_error(event->connect.status);
      if(l->mouse_paused)paused_locked(l);
      else {g->error=error;retire_locked(l,error);}
    }else{
      l->conn=event->connect.conn_handle;l->connected=true;l->mtu=ble_att_mtu(l->conn);
      l->encrypted=l->bonded=false;l->queued=l->head=l->dropped=0;l->error=0;
      if(l->abandoned){terminate=true;conn=l->conn;l->closing=true;}
      else if(l->restore_security){secure=true;conn=l->conn;l->restore_security=false;}
    }
    break;
  case BLE_GAP_EVENT_DISCONNECT:
    if(l->conn==event->disconnect.conn.conn_handle){
      if(l->mouse_paused)paused_locked(l);else retire_locked(l,0);}
    break;
  case BLE_GAP_EVENT_ENC_CHANGE:
    if(l->connected&&l->conn==event->enc_change.conn_handle){
      struct ble_gap_conn_desc desc;int rc=ble_gap_conn_find(l->conn,&desc);
      if(!rc){l->encrypted=desc.sec_state.encrypted;l->bonded=desc.sec_state.bonded;
        if(l->bonded){l->peer.bonded=true;
          for(unsigned i=0;i<g->count;i++)if(same_peer(g->devices+i,&l->peer))g->devices[i].bonded=true;}}
      if(l->pending&&l->operation==GLASS_GATT_PAIR)
        complete_locked(l,event->enc_change.status||rc||!l->encrypted?-EACCES:0);
    }break;
  case BLE_GAP_EVENT_MTU:
    if(l->conn==event->mtu.conn_handle)l->mtu=event->mtu.value;break;
  case BLE_GAP_EVENT_NOTIFY_RX:
    if(l->connected&&!l->closing&&l->conn==event->notify_rx.conn_handle){
      unsigned length=OS_MBUF_PKTLEN(event->notify_rx.om);
      if(length>GLASS_GATT_BYTES){l->dropped++;break;}
      if(l->queued==NOTIFICATIONS){l->head=(l->head+1)%NOTIFICATIONS;l->queued--;l->dropped++;}
      struct notification *n=&l->notifications[(l->head+l->queued)%NOTIFICATIONS];uint16_t copied=0;
      if(!ble_hs_mbuf_to_flat(event->notify_rx.om,n->data,sizeof(n->data),&copied)){
        n->handle=event->notify_rx.attr_handle;n->length=copied;n->indication=event->notify_rx.indication;l->queued++;}
      else l->dropped++;
    }break;
  case BLE_GAP_EVENT_REPEAT_PAIRING:
    pthread_mutex_unlock(&lock);return BLE_GAP_REPEAT_PAIRING_IGNORE;
  default:break;
  }
  pthread_cond_broadcast(&done);
  pthread_mutex_unlock(&lock);
  if(terminate){int rc=ble_gap_terminate(conn,BLE_ERR_REM_USER_CONN_TERM);
    if(rc){pthread_mutex_lock(&lock);l=link_locked(generation);
      if(l&&l->active&&l->conn==conn)l->error=posix_error(rc);pthread_mutex_unlock(&lock);}}
  if(secure){int rc=ble_gap_security_initiate(conn);
    if(rc){pthread_mutex_lock(&lock);l=link_locked(generation);
      if(l&&l->active&&l->conn==conn)l->error=posix_error(rc);pthread_mutex_unlock(&lock);}}
  return 0;
}
int glass_ble_central_scan_session(const uint8_t service[16],uint32_t *generation)
{
  if(generation)*generation=0;
  int ret=glass_ble_host_wait();if(ret)return ret;
  if(pthread_mutex_trylock(&scan_api))return -EBUSY;
  pthread_mutex_lock(&lock);
  if(!g||g->mouse_paused||g->scanning){ret=-EBUSY;goto unlock;}
  for(unsigned i=0;i<GLASS_BLE_CENTRAL_LIMIT;i++)if(g->links[i].connecting){ret=-EBUSY;goto unlock;}
  if(g->sequence==UINT32_MAX||g->scan_generation==UINT32_MAX){ret=-EOVERFLOW;goto unlock;}
  ret=glass_ble_central_reserve(active_locked()?active_locked():1);if(ret)goto unlock;
  memset(g->devices,0,sizeof(g->devices));g->count=0;g->scan_truncated=false;
  /* Connected devices may no longer advertise. Keep them selectable. */
  for(unsigned i=0;i<GLASS_BLE_CENTRAL_LIMIT;i++)if(g->links[i].active)
    g->devices[g->count++]=g->links[i].peer;
  g->scan_generation++;if(generation)*generation=g->scan_generation;g->scanning=true;g->scan_abandoned=false;g->error=0;
  g->scan_filter_valid=service!=NULL;if(service)memcpy(g->scan_service,service,16);
  uint32_t token=g->scan_token=next_locked();pthread_mutex_unlock(&lock);
  ret=dispatch(HOST_SCAN,0,token,NULL,NULL);
  if(ret){pthread_mutex_lock(&lock);
    if(g->scan_token==token){g->scanning=false;g->error=posix_error(ret);reserve_locked(0);}
    pthread_mutex_unlock(&lock);ret=posix_error(ret);}
  pthread_mutex_unlock(&scan_api);return ret;
unlock:pthread_mutex_unlock(&lock);pthread_mutex_unlock(&scan_api);return ret;
}
int glass_ble_central_scan_service(const uint8_t service[16])
{return glass_ble_central_scan_session(service,NULL);}
int glass_ble_central_scan(void)
{return glass_ble_central_scan_service(NULL);}
int glass_ble_central_scan_stop(uint32_t generation)
{
  if(pthread_mutex_trylock(&scan_api))return -EBUSY;
  pthread_mutex_lock(&lock);
  if(g&&generation&&generation!=g->scan_generation){pthread_mutex_unlock(&lock);pthread_mutex_unlock(&scan_api);return -ESTALE;}
  if(!g||!g->scanning){pthread_mutex_unlock(&lock);pthread_mutex_unlock(&scan_api);return 0;}
  uint32_t token=g->scan_token;g->scan_abandoned=true;pthread_mutex_unlock(&lock);
  int rc=posix_error(dispatch(HOST_SCAN_CANCEL,0,token,NULL,NULL));
  pthread_mutex_lock(&lock);
  if(g&&g->scan_token==token){
    if(!rc||!g->scanning){g->scanning=false;rc=0;}
    if(rc)g->error=rc;reserve_locked(0);
  }
  pthread_mutex_unlock(&lock);pthread_mutex_unlock(&scan_api);return rc;
}
int glass_ble_central_connect_session(unsigned index,uint32_t scan_generation,uint32_t *generation)
{
  if(generation)*generation=0;
  int ret=glass_ble_host_wait();if(ret)return ret;
  if(pthread_mutex_trylock(&connect_api))return -EBUSY;
  ble_addr_t peer;uint32_t token=0;struct link *l=NULL;
  pthread_mutex_lock(&lock);
  if(!g||g->mouse_paused||g->scanning){ret=-EBUSY;goto unlock;}
  if(scan_generation&&scan_generation!=g->scan_generation){ret=-ESTALE;goto unlock;}
  if(index>=g->count){ret=-EINVAL;goto unlock;}
  if(g->devices[index].connectable_known&&!g->devices[index].connectable){ret=-ENOTSUP;goto unlock;}
  if(peer_active_locked(g->devices+index)){ret=-EALREADY;goto unlock;}
  for(unsigned i=0;i<GLASS_BLE_CENTRAL_LIMIT;i++){
    if(g->links[i].connecting){ret=-EBUSY;goto unlock;}
    if(!g->links[i].active&&!g->links[i].busy&&!l)l=&g->links[i];
  }
  if(!l||active_locked()>=capacity_locked()){ret=-ENOSPC;goto unlock;}
  if(g->sequence==UINT32_MAX){ret=-EOVERFLOW;goto unlock;}
  ret=reserve_locked(1);if(ret)goto unlock;
  l->generation=token=next_locked();l->gap_token=token;l->mouse_paused=l->restore_security=false;
  l->active=l->connecting=true;
  l->connected=l->encrypted=l->bonded=l->abandoned=l->pending=l->closing=false;
  l->error=l->result=0;l->token=0;l->conn=BLE_HS_CONN_HANDLE_NONE;l->mtu=23;l->queued=l->head=l->dropped=0;
  l->peer=g->devices[index];g->selected=(unsigned)(l-g->links);g->error=0;
  peer.type=l->peer.type;memcpy(peer.val,l->peer.address,6);
  if(generation)*generation=token;
  pthread_mutex_unlock(&lock);
  ret=dispatch(HOST_CONNECT,0,token,&peer,NULL);
  if(ret){pthread_mutex_lock(&lock);l=link_locked(token);
    if(l&&l->active&&l->connecting){g->error=posix_error(ret);retire_locked(l,g->error);}
    pthread_mutex_unlock(&lock);ret=posix_error(ret);}
  pthread_mutex_unlock(&connect_api);return ret;
unlock:pthread_mutex_unlock(&lock);pthread_mutex_unlock(&connect_api);return ret;
}
int glass_ble_central_connect(unsigned index,uint32_t scan_generation)
{return glass_ble_central_connect_session(index,scan_generation,NULL);}
int glass_ble_central_select(uint32_t generation)
{
  pthread_mutex_lock(&lock);struct link *l=link_locked(generation);int ret=0;
  if(!l)ret=-ESTALE;else if(!l->active)ret=-ENOTCONN;
  else g->selected=(unsigned)(l-g->links);
  pthread_mutex_unlock(&lock);return ret;
}
static int disconnect_generation(uint32_t generation,bool for_mouse)
{
  pthread_mutex_lock(&lock);struct link *l=generation?link_locked(generation):focused_locked();
  if(!l){pthread_mutex_unlock(&lock);return generation?-ESTALE:0;}
  if(!l->active){pthread_mutex_unlock(&lock);return 0;}
  if(for_mouse&&!l->connected&&!l->connecting){pthread_mutex_unlock(&lock);return 0;}
  if(l->mouse_paused&&!for_mouse){
    l->mouse_paused=false;
    if(!l->connected&&!l->connecting){retire_locked(l,0);pthread_mutex_unlock(&lock);return 0;}
  }
  bool connecting=l->connecting;uint16_t conn=l->conn;generation=l->generation;
  l->abandoned=l->closing=true;l->error=0;if(l->pending)complete_locked(l,-ECANCELED);
  pthread_mutex_unlock(&lock);
  int ret=posix_error(dispatch(connecting?HOST_CONNECT_CANCEL:HOST_TERMINATE,conn,generation,NULL,NULL));
  if(ret){pthread_mutex_lock(&lock);l=link_locked(generation);
    if(l&&l->active)l->error=ret;pthread_mutex_unlock(&lock);}
  return ret;
}
int glass_ble_central_disconnect_generation(uint32_t generation)
{return disconnect_generation(generation,false);}

int glass_ble_central_mouse_pause(void)
{
  uint32_t generations[GLASS_BLE_CENTRAL_LIMIT]={0};
  unsigned count=0;
  int ret=0;
  pthread_mutex_lock(&mouse_api);
  pthread_mutex_lock(&lock);
  if(!g){ret=-ENODEV;goto unlock;}
  g->mouse_paused=true; /* Close admission before collecting sessions. */
  uint32_t scan=g->scanning?g->scan_generation:0;
  for(unsigned i=0;i<GLASS_BLE_CENTRAL_LIMIT;i++){
    struct link *l=&g->links[i];
    if(!l->active)continue;
    if(!l->mouse_paused&&!l->closing&&!l->abandoned){
      l->mouse_paused=true;l->restore_security=l->encrypted&&l->bonded;}
    if(l->pending)complete_locked(l,-ECANCELED);
    l->queued=l->head=0;
    generations[count++]=l->generation;
  }
  pthread_mutex_unlock(&lock);
  if(scan)ret=glass_ble_central_scan_stop(scan);
  for(unsigned i=0;i<count;i++){
    int rc=disconnect_generation(generations[i],true);
    if(!ret)ret=rc;
  }
  pthread_mutex_lock(&lock);
  if(!ret){
    struct timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_sec+=3;
    for(;;){
      bool active=g->scanning;
      for(unsigned i=0;i<GLASS_BLE_CENTRAL_LIMIT;i++)
        active|=g->links[i].active&&(g->links[i].connecting||g->links[i].connected);
      if(!active)break;
      int rc=pthread_cond_timedwait(&done,&lock,&until);
      if(rc){ret=-rc;break;}
    }
    if(!g->mouse_paused&&!ret)ret=-ECANCELED; /* Controller reset. */
  }
  if(ret)g->error=ret;
unlock:
  pthread_mutex_unlock(&lock);pthread_mutex_unlock(&mouse_api);return ret;
}

int glass_ble_central_mouse_resume(void)
{
  int ret=0;
  pthread_mutex_lock(&mouse_api);
  pthread_mutex_lock(&lock);
  if(!g||!g->mouse_paused)goto unlock;
  /* HID must have physically stopped, including advertising. */
  struct glass_ble_capacity_state capacity;
  glass_ble_capacity_read(&capacity);
  if(capacity.hid_reserved){ret=-EBUSY;goto unlock;}
  g->restoring=true;g->restore_count=0;
  memset(g->restores,0,sizeof(g->restores));
  for(unsigned i=0;i<GLASS_BLE_CENTRAL_LIMIT;i++)
    if(g->links[i].active&&g->links[i].mouse_paused){
      struct glass_ble_restore_result *r=&g->restores[g->restore_count++];
      r->generation=g->links[i].generation;r->error=-EAGAIN;
    }
  for(unsigned i=0;i<g->restore_count;i++){
    struct glass_ble_restore_result *result=&g->restores[i];
    struct link *l=link_locked(result->generation);
    if(!l||!l->active||!l->mouse_paused){result->error=-ECANCELED;if(!ret)ret=-ECANCELED;continue;}
    if(l->connecting||l->connected){result->error=-EBUSY;if(!ret)ret=-EBUSY;break;}
    /* Never overlap an initiator left pending by a previous cancel/timeout. */
    bool initiating=false;
    for(unsigned n=0;n<GLASS_BLE_CENTRAL_LIMIT;n++)initiating|=g->links[n].connecting;
    if(initiating){result->error=-EBUSY;if(!ret)ret=-EBUSY;break;}
    if(g->sequence==UINT32_MAX){result->error=-EOVERFLOW;retire_locked(l,-EOVERFLOW);if(!ret)ret=-EOVERFLOW;continue;}
    /* Keep logical generation for its owner, but use a fresh physical GAP
     * token so callbacks from the suspended link cannot touch this one. */
    l->mouse_paused=false;l->connecting=true;l->closing=l->abandoned=false;
    l->gap_token=next_locked();l->error=0;l->mtu=23;
    int rc=reserve_locked(0);
    uint32_t generation=l->generation;
    ble_addr_t peer={.type=l->peer.type};memcpy(peer.val,l->peer.address,6);
    if(rc){result->error=rc;retire_locked(l,rc);if(!ret)ret=rc;continue;}
    result->attempted=true;
    pthread_mutex_unlock(&lock);
    rc=posix_error(dispatch(HOST_CONNECT,0,generation,&peer,NULL));
    pthread_mutex_lock(&lock);
    if(rc&&l->active&&l->generation==generation)retire_locked(l,rc);
    if(!rc){
      struct timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_sec+=11;
      while(l->active&&l->generation==generation&&l->connecting){
        int wait=pthread_cond_timedwait(&done,&lock,&until);
        if(wait){rc=-wait;break;}
      }
      if(!rc&&(!l->active||!l->connected))rc=l->error?l->error:-ECANCELED;
    }
    if(rc&&l->active&&l->generation==generation&&l->connecting){
      l->abandoned=l->closing=true;
      pthread_mutex_unlock(&lock);
      (void)dispatch(HOST_CONNECT_CANCEL,0,generation,NULL,NULL);
      pthread_mutex_lock(&lock);
      struct timespec cancelled;clock_gettime(CLOCK_REALTIME,&cancelled);cancelled.tv_sec+=3;
      while(l->active&&l->generation==generation&&(l->connecting||l->connected)){
        int wait=pthread_cond_timedwait(&done,&lock,&cancelled);
        if(wait)break;
      }
    }
    /* Reset may have replaced the batch outcome while dispatch was unlocked. */
    if(!g->restoring){if(!ret)ret=-EIO;break;}
    result->error=rc;
    result->connected=l->active&&l->generation==generation&&l->connected&&!l->closing;
    if(rc&&!ret)ret=rc;
    if(l->active&&l->generation==generation&&l->closing)break;
  }
  /* A failed pause can leave a physical disconnect still pending. Keep its
   * restore record and admission gate for an explicit stop retry. */
  bool paused=false;
  for(unsigned i=0;i<GLASS_BLE_CENTRAL_LIMIT;i++)
    paused|=g->links[i].active&&g->links[i].mouse_paused;
  g->mouse_paused=paused;
  g->restoring=false;
  g->error=ret;
unlock:
  pthread_mutex_unlock(&lock);pthread_mutex_unlock(&mouse_api);return ret;
}
int glass_ble_central_disconnect(void)
{return glass_ble_central_disconnect_generation(0);}
int glass_ble_central_cancel(void)
{
  pthread_mutex_lock(&lock);struct link *l=focused_locked();
  uint32_t generation=l&&l->active&&(l->connecting||l->pending)?l->generation:0;
  uint32_t scan=g&&g->scanning?g->scan_generation:0;pthread_mutex_unlock(&lock);
  int ret=scan?glass_ble_central_scan_stop(scan):0;
  if(generation){int rc=glass_ble_central_disconnect_generation(generation);if(!ret)ret=rc;}
  return ret;
}
/* Every GATT callback matches both the HCI handle and its unique operation
 * token. Reusing a pool slot or HCI handle never makes old data current. */
static struct link *callback_locked(uint16_t conn,void *arg)
{
  if(g)for(unsigned i=0;i<GLASS_BLE_CENTRAL_LIMIT;i++){
    struct link *l=&g->links[i];
    if(l->active&&l->pending&&l->connected&&!l->closing&&l->conn==conn&&
       l->token==(uint32_t)(uintptr_t)arg)return l;
  }
  return NULL;
}
static int discovery(uint16_t conn,const struct ble_gatt_error *error,
                     const ble_uuid_t *uuid,uint16_t start,uint16_t end,uint16_t value,uint8_t properties,void *arg)
{
  pthread_mutex_lock(&lock);struct link *l=callback_locked(conn,arg);
  if(l){
    if(error->status)complete_locked(l,posix_error(error->status));
    else if(uuid){if(l->reply.count<GLASS_GATT_ITEMS){struct glass_gatt_item *i=l->reply.items+l->reply.count++;
      i->start=start;i->end=end;i->value=value;i->properties=properties;ble_uuid_to_str(uuid,i->uuid);}
      else l->reply.truncated=true;}
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
  pthread_mutex_lock(&lock);struct link *l=callback_locked(conn,arg);
  if(l){int ret=posix_error(error->status);
    if(!ret&&attr&&attr->om){uint16_t copied=0;unsigned length=OS_MBUF_PKTLEN(attr->om);
      if(length>sizeof(l->reply.data))ret=-EMSGSIZE;
      else if(ble_hs_mbuf_to_flat(attr->om,l->reply.data,sizeof(l->reply.data),&copied))ret=-EIO;
      else{l->reply.length=copied;l->reply.handle=attr->handle;}}
    complete_locked(l,ret);
  }
  pthread_mutex_unlock(&lock);return 0;
}
static int mtu_cb(uint16_t conn,const struct ble_gatt_error *error,uint16_t mtu,void *arg)
{
  pthread_mutex_lock(&lock);struct link *l=callback_locked(conn,arg);
  if(l){if(!error->status)l->mtu=mtu;complete_locked(l,posix_error(error->status));}
  pthread_mutex_unlock(&lock);return 0;
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
      pthread_mutex_lock(&lock);struct link *l=callback_locked(conn,arg);if(l)complete_locked(l,posix_error(ret));pthread_mutex_unlock(&lock);}
    else ret=ble_gattc_write_flat(conn,r->handle,r->data,r->length,attribute_cb,arg);break;
  case GLASS_GATT_SUBSCRIBE:ret=ble_gattc_write_flat(conn,r->handle,r->data,2,attribute_cb,arg);break;
  case GLASS_GATT_PAIR:{
    pthread_mutex_lock(&lock);struct link *l=callback_locked(conn,arg);bool encrypted=l&&l->encrypted;
    if(encrypted)complete_locked(l,0);pthread_mutex_unlock(&lock);
    if(!encrypted)ret=ble_gap_security_initiate(conn);break;}
  case GLASS_GATT_MTU:ret=ble_gattc_exchange_mtu(conn,mtu_cb,arg);break;
  default:ret=-EINVAL;break;
  }
  return ret;
}
int glass_ble_gatt(const struct glass_gatt_request *r,struct glass_gatt_result *out,atomic_bool *cancel)
{
  if(cancel&&atomic_load(cancel))return -ECANCELED;
  if(!r||!out||(unsigned)r->operation>GLASS_GATT_MTU||!r->generation||r->length>GLASS_GATT_BYTES)return -EINVAL;
  if((r->operation==GLASS_GATT_CHARACTERISTICS||r->operation==GLASS_GATT_DESCRIPTORS)&&(!r->start||r->start>r->end))return -EINVAL;
  if((r->operation==GLASS_GATT_READ||r->operation==GLASS_GATT_WRITE||r->operation==GLASS_GATT_SUBSCRIBE)&&!r->handle)return -EINVAL;
  if(r->operation==GLASS_GATT_SUBSCRIBE&&(r->length!=2||r->data[1]||r->data[0]>2))return -EINVAL;
  pthread_mutex_lock(&lock);struct link *l=link_locked(r->generation);
  if(!l){pthread_mutex_unlock(&lock);return -ESTALE;}
  /* Acquire in one owner critical section: a slot cannot be reused while
   * this caller owns its API mutex, even after disconnect/reset. */
  if(l->busy||pthread_mutex_trylock(&l->api)){pthread_mutex_unlock(&lock);return -EBUSY;}
  l->busy=true;int ret=0;uint16_t conn=0;uint32_t token=0;
  if(!l->active||!l->connected||l->closing||l->mouse_paused||g->mouse_paused)ret=-ENOTCONN;
  else if(g->sequence==UINT32_MAX)ret=-EOVERFLOW;
  else{memset(&l->reply,0,sizeof(l->reply));l->reply.generation=l->generation;
    l->pending=true;l->result=-EINPROGRESS;conn=l->conn;token=l->token=next_locked();l->operation=r->operation;}
  pthread_mutex_unlock(&lock);if(ret)goto out;
  ret=dispatch(HOST_GATT,conn,token,NULL,r);
  pthread_mutex_lock(&lock);if(ret&&l->pending)complete_locked(l,posix_error(ret));
  unsigned waits=0;struct timespec until={0};bool monotonic=clock_gettime(CLOCK_MONOTONIC,&until)==0;until.tv_sec+=15;
  while(l->pending){struct timespec deadline;clock_gettime(CLOCK_REALTIME,&deadline);
    deadline.tv_nsec+=50000000;if(deadline.tv_nsec>=1000000000){deadline.tv_sec++;deadline.tv_nsec-=1000000000;}
    pthread_cond_timedwait(&done,&lock,&deadline);struct timespec now;
    bool expired=monotonic&&clock_gettime(CLOCK_MONOTONIC,&now)==0?
      now.tv_sec>until.tv_sec||(now.tv_sec==until.tv_sec&&now.tv_nsec>=until.tv_nsec):++waits>=300;
    if((cancel&&atomic_load(cancel))||expired){
      complete_locked(l,cancel&&atomic_load(cancel)?-ECANCELED:-ETIMEDOUT);l->closing=true;
      pthread_mutex_unlock(&lock);(void)dispatch(HOST_TERMINATE,conn,r->generation,NULL,NULL);pthread_mutex_lock(&lock);break;}
  }
  ret=l->result;l->reply.mtu=l->mtu;if(!ret)*out=l->reply;pthread_mutex_unlock(&lock);
out:pthread_mutex_lock(&lock);l->busy=false;pthread_mutex_unlock(&l->api);pthread_mutex_unlock(&lock);return ret;
}
int glass_ble_notification(uint32_t generation,struct glass_gatt_result *out)
{
  if(!out||!generation)return -EINVAL;
  pthread_mutex_lock(&lock);struct link *l=link_locked(generation);int ret=0;
  if(!l)ret=-ESTALE;else if(!l->active||!l->connected||l->closing||l->mouse_paused)ret=-ENOTCONN;
  else if(!l->queued)ret=-EAGAIN;
  else{struct notification *n=&l->notifications[l->head];memset(out,0,sizeof(*out));
    out->generation=l->generation;out->handle=n->handle;out->length=n->length;out->indication=n->indication;
    out->dropped=l->dropped;memcpy(out->data,n->data,n->length);l->head=(l->head+1)%NOTIFICATIONS;l->queued--;}
  pthread_mutex_unlock(&lock);return ret;
}
