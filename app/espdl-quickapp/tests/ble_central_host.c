/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include "glass_ble.h"
#include "glass_ble_central.h"
#include "host/ble_hs.h"
static unsigned role,calls,terminations;static bool stall,encrypted;
static int termination_error;
static _Thread_local bool host_context;
static atomic_bool stop_host;
static atomic_uint host_dispatches;
static atomic_uint hid_polls;
void glass_ble_hid_poll(void){assert(host_context);atomic_fetch_add(&hid_polls,1);}
static void *host_loop(void *arg){(void)arg;host_context=true;
  while(!atomic_load(&stop_host)){
    glass_ble_central_wait();atomic_fetch_add(&host_dispatches,1);glass_ble_central_poll();
  }return NULL;}
static atomic_bool *cancel_now;
static void *gap_arg;
static int (*gap_cb)(struct ble_gap_event *,void *);
int ble_store_util_bonded_peers(ble_addr_t *peers,int *count,int capacity)
{assert(capacity>=1);memset(peers,0,sizeof(*peers));peers[0].val[0]=42;*count=1;return 0;}
int glass_ble_host_wait(void){return glass_ble_central_init();}
int glass_ble_role_claim(unsigned r){if(role&&role!=r)return -EBUSY;role=r;return 0;}
void glass_ble_role_release(unsigned r){if(role==r)role=0;}
int ble_hs_id_infer_auto(int a,uint8_t *b){(void)a;*b=0;return 0;}
uint16_t ble_att_mtu(uint16_t c){(void)c;return 23;}
char *ble_uuid_to_str(const ble_uuid_t *u,char *out){(void)u;strcpy(out,"180f");return out;}
int ble_hs_mbuf_to_flat(const struct os_mbuf *m,void *out,uint16_t bytes,uint16_t *copied)
{if(m->len>bytes)return 1;memcpy(out,m->data,m->len);*copied=m->len;return 0;}
int ble_gap_disc(uint8_t a,int32_t b,const struct ble_gap_disc_params *p,int(*cb)(struct ble_gap_event *,void *),void *arg)
{assert(host_context);(void)a;(void)b;(void)p;gap_arg=arg;gap_cb=cb;calls++;return 0;}
int ble_gap_disc_cancel(void){calls++;return 0;}
int ble_gap_conn_cancel(void){struct ble_gap_event e={.type=BLE_GAP_EVENT_CONNECT};e.connect.status=1;return gap_cb(&e,gap_arg);}
int ble_gap_connect(uint8_t own,const ble_addr_t *peer,int32_t timeout,const void *params,int(*cb)(struct ble_gap_event *,void *),void *arg)
{(void)own;(void)peer;(void)timeout;(void)params;gap_arg=arg;calls++;gap_cb=cb;return 0;}
int ble_gap_terminate(uint16_t c,uint8_t why)
{(void)why;terminations++;if(termination_error)return termination_error;struct ble_gap_event e={.type=BLE_GAP_EVENT_DISCONNECT};e.disconnect.conn.conn_handle=c;gap_cb(&e,gap_arg);return 0;}
int ble_gap_conn_find(uint16_t c,struct ble_gap_conn_desc *d)
{d->conn_handle=c;d->sec_state.encrypted=encrypted;d->sec_state.bonded=encrypted;return 0;}
int ble_gap_security_initiate(uint16_t c)
{encrypted=true;struct ble_gap_event e={.type=BLE_GAP_EVENT_ENC_CHANGE};e.enc_change.conn_handle=c;gap_cb(&e,gap_arg);return 0;}
int ble_gattc_disc_all_svcs(uint16_t c,ble_gatt_svc_fn *cb,void *arg)
{calls++;struct ble_gatt_error e={0};struct ble_gatt_svc v={.start_handle=1,.end_handle=20};
 for(unsigned i=0;i<40;i++)cb(c,&e,&v,arg);e.status=BLE_HS_EDONE;return cb(c,&e,NULL,arg);}
int ble_gattc_disc_all_chrs(uint16_t c,uint16_t a,uint16_t b,ble_gatt_chr_fn *cb,void *arg)
{assert(a==1&&b==20);struct ble_gatt_error e={0};struct ble_gatt_chr v={.def_handle=2,.val_handle=3,.properties=0x12};cb(c,&e,&v,arg);e.status=BLE_HS_EDONE;return cb(c,&e,NULL,arg);}
int ble_gattc_disc_all_dscs(uint16_t c,uint16_t a,uint16_t b,ble_gatt_dsc_fn *cb,void *arg)
{(void)a;(void)b;struct ble_gatt_error e={0};struct ble_gatt_dsc v={.handle=4};cb(c,&e,3,&v,arg);e.status=BLE_HS_EDONE;return cb(c,&e,3,NULL,arg);}
int ble_gattc_read(uint16_t c,uint16_t handle,ble_gatt_attr_fn *cb,void *arg)
{calls++;if(stall){if(cancel_now)atomic_store(cancel_now,true);return 0;}
 struct os_mbuf mb={.len=2,.data={42,99}};struct ble_gatt_attr a={.handle=handle,.om=&mb};struct ble_gatt_error e={0};return cb(c,&e,&a,arg);}
int ble_gattc_write_flat(uint16_t c,uint16_t h,const void *data,uint16_t bytes,ble_gatt_attr_fn *cb,void *arg)
{assert(h==4||h==3);assert(bytes==2&&((const uint8_t *)data)[0]==1);calls++;struct ble_gatt_error e={0};return cb(c,&e,NULL,arg);}
int ble_gattc_write_no_rsp_flat(uint16_t c,uint16_t h,const void *d,uint16_t n)
{(void)c;(void)h;(void)d;(void)n;calls++;return 0;}
int ble_gattc_exchange_mtu(uint16_t c,ble_gatt_mtu_fn *cb,void *arg)
{struct ble_gatt_error e={0};return cb(c,&e,100,arg);}
static void connected(void){struct ble_gap_event e={.type=BLE_GAP_EVENT_CONNECT};e.connect.conn_handle=7;gap_cb(&e,gap_arg);}
static void advertising_metadata(void)
{
  assert(!glass_ble_central_scan());
  struct ble_gap_event e={.type=BLE_GAP_EVENT_DISC};
  e.disc.event_type=3;e.disc.rssi=-60;
  for(unsigned i=0;i<GLASS_BLE_LIMIT;i++){e.disc.addr.val[0]=i+1;gap_cb(&e,gap_arg);}
  uint8_t fields[]={5,9,'K','e','y','s',3,0x19,0xc1,3,3,3,0x12,0x18};
  e.disc.addr.val[0]=90;e.disc.event_type=0;e.disc.data=fields;e.disc.length_data=sizeof(fields);gap_cb(&e,gap_arg);
  struct glass_ble_state s={0};glass_ble_central_read(&s);
  assert(s.count==32&&s.scan_truncated&&s.devices[0].address[0]==90);
  assert(s.devices[0].connectable_known&&s.devices[0].connectable&&s.devices[0].appearance==0x03c1&&s.devices[0].service_hint==0x1812);
  /* Scan response without name/connectability must retain both properties;
   * malformed lengths must not create a spurious service/appearance. */
  uint8_t malformed[]={5,0x19,1};e.disc.event_type=4;e.disc.data=malformed;e.disc.length_data=sizeof(malformed);gap_cb(&e,gap_arg);
  glass_ble_central_read(&s);assert(s.devices[0].connectable&&!strcmp(s.devices[0].name,"Keys")&&s.devices[0].appearance==0x03c1);
  e.disc.addr.val[0]=42;e.disc.data=NULL;e.disc.length_data=0;e.disc.event_type=0;gap_cb(&e,gap_arg);
  glass_ble_central_read(&s);assert(s.devices[1].address[0]==42&&s.devices[1].bonded);
  /* An equal-priority anonymous packet cannot churn a retained slot. */
  e.disc.addr.val[0]=100;e.disc.event_type=3;gap_cb(&e,gap_arg);
  glass_ble_central_read(&s);assert(s.devices[2].address[0]==3);
  assert(!glass_ble_central_cancel());assert(glass_ble_central_connect(2,s.scan_generation)==-ENOTSUP);
  assert(!role);
  puts("BLE_ADVERTISING_OK: connectability/name response merge; bounded parsing; appearance/service hints; persistent identity bonds; priority admission at capacity; broadcast-only connection rejected");
}
int main(void)
{
  pthread_t host;assert(!pthread_create(&host,NULL,host_loop,NULL));
  assert(glass_ble_central_init()==0);role=2;assert(glass_ble_central_scan()==-EBUSY);role=0;
  usleep(100000);assert(!atomic_load(&host_dispatches));
  extern void glass_ble_central_hid_wake(void);
  glass_ble_central_hid_wake();
  for(unsigned i=0;i<2000&&!atomic_load(&hid_polls);i++)usleep(1000);
  assert(atomic_load(&hid_polls)==1);
  advertising_metadata();
  assert(!glass_ble_central_scan());assert(glass_ble_central_scan()==-EBUSY);
  uint8_t adv[]={5,9,'T','e','s','t'};struct ble_gap_event e={.type=BLE_GAP_EVENT_DISC};e.disc.addr.val[0]=1;e.disc.data=adv;e.disc.length_data=sizeof(adv);e.disc.rssi=-40;gap_cb(&e,gap_arg);
  struct glass_ble_state state={0};glass_ble_central_read(&state);assert(state.count==1&&!strcmp(state.devices[0].name,"Test"));
  assert(!glass_ble_central_cancel());assert(!role);glass_ble_central_read(&state);assert(!state.scanning);
  void *old_scan_arg=gap_arg;
  assert(!glass_ble_central_scan());
  struct ble_gap_event late={.type=BLE_GAP_EVENT_DISC_COMPLETE};
  gap_cb(&late,old_scan_arg);glass_ble_central_read(&state);assert(state.scanning&&role==1);
  gap_cb(&e,gap_arg);assert(!glass_ble_central_cancel());glass_ble_central_read(&state);
  assert(glass_ble_central_connect(0,state.scan_generation+1)==-ESTALE);
  assert(!glass_ble_central_connect(0,state.scan_generation));
  glass_ble_central_read(&state);assert(state.connecting&&state.peer_valid&&!strcmp(state.peer.name,"Test")&&state.peer.address[0]==1);
  connected();glass_ble_central_read(&state);assert(state.connected&&state.peer_valid&&!state.disconnecting&&state.generation&&role==1);
  termination_error=BLE_HS_EBUSY;assert(glass_ble_central_disconnect()==-EBUSY);
  glass_ble_central_read(&state);assert(state.connected&&state.peer_valid&&!state.disconnecting);
  termination_error=0;
  struct glass_gatt_request r={.generation=state.generation,.operation=GLASS_GATT_SERVICES};struct glass_gatt_result out;
  atomic_bool cancel=false;assert(!glass_ble_gatt(&r,&out,&cancel));assert(out.count==32&&out.truncated);
  r.operation=GLASS_GATT_CHARACTERISTICS;r.start=1;r.end=20;assert(!glass_ble_gatt(&r,&out,&cancel)&&out.items[0].value==3);
  r.operation=GLASS_GATT_DESCRIPTORS;assert(!glass_ble_gatt(&r,&out,&cancel)&&out.items[0].start==4);
  r.operation=GLASS_GATT_READ;r.handle=3;assert(!glass_ble_gatt(&r,&out,&cancel)&&out.length==2&&out.data[0]==42);
  r.generation++;unsigned before=calls;assert(glass_ble_gatt(&r,&out,&cancel)==-ESTALE&&calls==before);r.generation--;
  atomic_store(&cancel,true);assert(glass_ble_gatt(&r,&out,&cancel)==-ECANCELED&&calls==before);atomic_store(&cancel,false);
  r.operation=GLASS_GATT_SUBSCRIBE;r.handle=4;r.length=2;r.data[0]=1;assert(!glass_ble_gatt(&r,&out,&cancel));
  for(unsigned i=0;i<10;i++){struct os_mbuf mb={.len=1,.data={i}};memset(&e,0,sizeof(e));e.type=BLE_GAP_EVENT_NOTIFY_RX;e.notify_rx.conn_handle=7;e.notify_rx.attr_handle=3;e.notify_rx.om=&mb;gap_cb(&e,gap_arg);}
  assert(!glass_ble_notification(r.generation,&out)&&out.data[0]==2&&out.dropped==2);
  assert(glass_ble_notification(r.generation+1,&out)==-ESTALE);
  r.operation=GLASS_GATT_PAIR;assert(!glass_ble_gatt(&r,&out,&cancel));glass_ble_central_read(&state);assert(state.encrypted&&state.bonded);
  r.operation=GLASS_GATT_MTU;assert(!glass_ble_gatt(&r,&out,&cancel)&&out.mtu==100);
  r.operation=GLASS_GATT_READ;r.handle=3;stall=true;cancel_now=&cancel;
  unsigned previous_terminations=terminations;
  assert(glass_ble_gatt(&r,&out,&cancel)==-ECANCELED&&terminations==previous_terminations+1);glass_ble_central_read(&state);assert(!state.connected&&!role);
  assert(!state.peer_valid&&!state.disconnecting);
  assert(glass_ble_notification(r.generation,&out)==-ENOTCONN);
  for(unsigned i=0;i<100;i++){assert(!glass_ble_central_scan());assert(!glass_ble_central_cancel());}
  unsigned idle=atomic_load(&host_dispatches);usleep(100000);
  assert(atomic_load(&host_dispatches)==idle);
  /* Wake the sleeping worker with a final real command before joining. */
  atomic_store(&stop_host,true);assert(!glass_ble_central_scan());pthread_join(host,NULL);
  glass_ble_central_reset();
  puts("BLE_COMMAND_IDLE_OK: zero empty dispatches; 200 back-to-back scan/cancel commands without lost wakeups");
  puts("BLE_CENTRAL_OK: real scan/connect/GATT state, role conflict, stale generation, bounded discovery/notify, pairing, MTU and cancellation cleanup");
  return 0;
}
