/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include "glass_ble.h"
#include "glass_ble_central.h"
#include "glass_ble_hid_internal.h"
#include "host/ble_hs.h"
static unsigned calls,terminations,capacity=3,g_roles,g_central_reservations;
static int termination_error,scan_cancel_error;
static unsigned restore_fail_peer;
static unsigned restore_hold_peer;
static uint16_t pending_handle;
static atomic_bool defer_connect_cancel,cancel_requested;
static uint16_t next_handle=7;
static pthread_mutex_t g_host_lock=PTHREAD_MUTEX_INITIALIZER;
unsigned glass_ble_capacity(void){return capacity;}
#include "role-under-test.inc"
#define role g_roles
static _Thread_local bool host_context;
static atomic_bool stop_host,stall,auto_connect;
static atomic_uint host_dispatches,hid_polls;
void glass_ble_hid_poll(void){assert(host_context);atomic_fetch_add(&hid_polls,1);}
static void *host_loop(void *arg){(void)arg;host_context=true;
  while(!atomic_load(&stop_host)){glass_ble_central_wait();atomic_fetch_add(&host_dispatches,1);glass_ble_central_poll();}return NULL;}
static void *gap_arg;
static int (*gap_cb)(struct ble_gap_event *,void *);
struct remote {int (*gap)(struct ble_gap_event *,void *);void *arg;bool encrypted;
  ble_gatt_attr_fn *read;void *read_arg;uint16_t handle;};
static struct remote remotes[16];
static pthread_mutex_t remote_lock=PTHREAD_MUTEX_INITIALIZER;
int ble_store_util_bonded_peers(ble_addr_t *p,int *n,int capacity)
{assert(capacity>=1);memset(p,0,sizeof(*p));p[0].val[0]=42;*n=1;return 0;}
int glass_ble_host_wait(void){return glass_ble_central_init();}
int ble_hs_id_infer_auto(int a,uint8_t *b){(void)a;*b=0;return 0;}
uint16_t ble_att_mtu(uint16_t c){(void)c;return 23;}
char *ble_uuid_to_str(const ble_uuid_t *u,char *out){(void)u;strcpy(out,"180f");return out;}
int ble_hs_mbuf_to_flat(const struct os_mbuf *m,void *out,uint16_t bytes,uint16_t *copied)
{if(m->len>bytes)return 1;memcpy(out,m->data,m->len);*copied=m->len;return 0;}
int ble_gap_disc(uint8_t a,int32_t b,const struct ble_gap_disc_params *p,int(*cb)(struct ble_gap_event *,void *),void *arg)
{assert(host_context);(void)a;(void)b;(void)p;gap_arg=arg;gap_cb=cb;calls++;return 0;}
int ble_gap_disc_cancel(void){assert(host_context);calls++;return scan_cancel_error;}
int ble_gap_connect(uint8_t own,const ble_addr_t *peer,int32_t timeout,const void *params,int(*cb)(struct ble_gap_event *,void *),void *arg)
{assert(host_context);(void)own;(void)timeout;(void)params;
 bool automatic=atomic_load(&auto_connect);uint16_t handle=automatic?(uint16_t)(peer->val[0]+6):next_handle;
 assert(handle<16);remotes[handle]=(struct remote){.gap=cb,.arg=arg};calls++;
 pending_handle=handle;
 if(automatic&&peer->val[0]==restore_hold_peer)return 0;
 if(automatic){struct ble_gap_event e={.type=BLE_GAP_EVENT_CONNECT};e.connect.conn_handle=handle;
   if(peer->val[0]==restore_fail_peer)e.connect.status=BLE_HS_ETIMEOUT;
   return cb(&e,arg);}return 0;}
int ble_gap_conn_cancel(void){assert(host_context);if(atomic_load(&defer_connect_cancel)){atomic_store(&cancel_requested,true);return 0;}
 struct ble_gap_event e={.type=BLE_GAP_EVENT_CONNECT};e.connect.status=1;return remotes[pending_handle].gap(&e,remotes[pending_handle].arg);}
int ble_gap_terminate(uint16_t c,uint8_t why)
{assert(host_context);(void)why;terminations++;if(termination_error)return termination_error;
 struct ble_gap_event e={.type=BLE_GAP_EVENT_DISCONNECT};e.disconnect.conn.conn_handle=c;return remotes[c].gap(&e,remotes[c].arg);}
int ble_gap_conn_find(uint16_t c,struct ble_gap_conn_desc *d)
{d->conn_handle=c;d->sec_state.encrypted=d->sec_state.bonded=remotes[c].encrypted;return 0;}
int ble_gap_security_initiate(uint16_t c)
{assert(host_context);remotes[c].encrypted=true;struct ble_gap_event e={.type=BLE_GAP_EVENT_ENC_CHANGE};e.enc_change.conn_handle=c;return remotes[c].gap(&e,remotes[c].arg);}
int ble_gattc_disc_all_svcs(uint16_t c,ble_gatt_svc_fn *cb,void *arg)
{assert(host_context);calls++;struct ble_gatt_error e={0};struct ble_gatt_svc v={.start_handle=1,.end_handle=20};for(unsigned i=0;i<40;i++)cb(c,&e,&v,arg);e.status=BLE_HS_EDONE;return cb(c,&e,NULL,arg);}
int ble_gattc_disc_all_chrs(uint16_t c,uint16_t a,uint16_t b,ble_gatt_chr_fn *cb,void *arg)
{assert(host_context&&a==1&&b==20);struct ble_gatt_error e={0};struct ble_gatt_chr v={.def_handle=2,.val_handle=3,.properties=0x12};cb(c,&e,&v,arg);e.status=BLE_HS_EDONE;return cb(c,&e,NULL,arg);}
int ble_gattc_disc_all_dscs(uint16_t c,uint16_t a,uint16_t b,ble_gatt_dsc_fn *cb,void *arg)
{assert(host_context);(void)a;(void)b;struct ble_gatt_error e={0};struct ble_gatt_dsc v={.handle=4};cb(c,&e,3,&v,arg);e.status=BLE_HS_EDONE;return cb(c,&e,3,NULL,arg);}
int ble_gattc_read(uint16_t c,uint16_t handle,ble_gatt_attr_fn *cb,void *arg)
{assert(host_context);calls++;if(atomic_load(&stall)){
 pthread_mutex_lock(&remote_lock);remotes[c].read=cb;remotes[c].read_arg=arg;remotes[c].handle=handle;pthread_mutex_unlock(&remote_lock);return 0;}
 struct os_mbuf mb={.len=2,.data={c,99}};struct ble_gatt_attr a={.handle=handle,.om=&mb};struct ble_gatt_error e={0};return cb(c,&e,&a,arg);}
int ble_gattc_write_flat(uint16_t c,uint16_t h,const void *data,uint16_t bytes,ble_gatt_attr_fn *cb,void *arg)
{assert(host_context&&(h==3||h==4)&&bytes==2&&((const uint8_t *)data)[0]==1);calls++;struct ble_gatt_error e={0};return cb(c,&e,NULL,arg);}
int ble_gattc_write_no_rsp_flat(uint16_t c,uint16_t h,const void *d,uint16_t n)
{assert(host_context);(void)c;(void)h;(void)d;(void)n;calls++;return 0;}
int ble_gattc_exchange_mtu(uint16_t c,ble_gatt_mtu_fn *cb,void *arg)
{assert(host_context);struct ble_gatt_error e={0};return cb(c,&e,100,arg);}
static void connected(void){struct ble_gap_event e={.type=BLE_GAP_EVENT_CONNECT};e.connect.conn_handle=next_handle;remotes[next_handle].gap(&e,remotes[next_handle].arg);}
static void notify(uint16_t c,unsigned count,unsigned value)
{for(unsigned i=0;i<count;i++){struct os_mbuf mb={.len=1,.data={value+i}};struct ble_gap_event e={.type=BLE_GAP_EVENT_NOTIFY_RX};e.notify_rx.conn_handle=c;e.notify_rx.attr_handle=3;e.notify_rx.om=&mb;remotes[c].gap(&e,remotes[c].arg);}}
struct query {struct glass_gatt_request r;struct glass_gatt_result out;atomic_bool cancel,finished;int result;};
static void *query_thread(void *arg)
{struct query *q=arg;q->result=glass_ble_gatt(&q->r,&q->out,&q->cancel);atomic_store(&q->finished,true);return NULL;}
static struct remote wait_read(uint16_t c)
{for(unsigned i=0;i<3000;i++){
 pthread_mutex_lock(&remote_lock);struct remote copy=remotes[c];pthread_mutex_unlock(&remote_lock);
 if(copy.read)return copy;usleep(1000);}assert(false);return remotes[c];}
static void answer(uint16_t c,struct remote copy)
{struct os_mbuf mb={.len=2,.data={c,77}};struct ble_gatt_attr a={.handle=copy.handle,.om=&mb};struct ble_gatt_error e={0};copy.read(c,&e,&a,copy.read_arg);}
static void clear_read(uint16_t c)
{pthread_mutex_lock(&remote_lock);remotes[c].read=NULL;pthread_mutex_unlock(&remote_lock);}
static void advertise_peer(unsigned address,const char *name)
{uint8_t bytes[32]={0,9};size_t n=strlen(name);assert(n<29);bytes[0]=n+1;memcpy(bytes+2,name,n);
 struct ble_gap_event e={.type=BLE_GAP_EVENT_DISC};e.disc.addr.val[0]=address;e.disc.data=bytes;e.disc.length_data=n+2;e.disc.rssi=-40;gap_cb(&e,gap_arg);}
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
static void filtered_advertising(void)
{
  uint8_t uuid[16];for(unsigned i=0;i<16;i++)uuid[i]=i+1;
  assert(!glass_ble_central_scan_service(uuid));
  /* The API owns the filter copy, not the caller's buffer. */
  uuid[0]=99;
  uint8_t fields[36]={17,7};for(unsigned i=0;i<16;i++)fields[i+2]=i+1;
  struct ble_gap_event e={.type=BLE_GAP_EVENT_DISC};
  e.disc.event_type=0;e.disc.addr.val[0]=81;e.disc.data=fields;e.disc.length_data=17;
  gap_cb(&e,gap_arg);struct glass_ble_state s={0};glass_ble_central_read(&s);assert(!s.count);
  e.disc.length_data=18;fields[2]=9;gap_cb(&e,gap_arg);glass_ble_central_read(&s);assert(!s.count);
  fields[2]=1;gap_cb(&e,gap_arg);glass_ble_central_read(&s);assert(s.count==1&&s.devices[0].service128);
  uint8_t name[]={5,9,'O','w','n','d'};e.disc.event_type=4;e.disc.data=name;e.disc.length_data=sizeof(name);
  gap_cb(&e,gap_arg);glass_ble_central_read(&s);assert(!strcmp(s.devices[0].name,"Ownd")&&s.devices[0].connectable);
  /* Incomplete lists and service data use the same little-endian UUID. */
  e.disc.addr.val[0]=82;e.disc.event_type=0;e.disc.data=fields;e.disc.length_data=18;fields[1]=6;
  gap_cb(&e,gap_arg);e.disc.addr.val[0]=83;fields[0]=19;fields[1]=0x21;e.disc.length_data=20;
  gap_cb(&e,gap_arg);glass_ble_central_read(&s);assert(s.count==3);
  /* A UUID embedded in the payload or a misaligned list cannot match. */
  e.disc.addr.val[0]=84;fields[1]=7;gap_cb(&e,gap_arg);glass_ble_central_read(&s);assert(s.count==3);
  fields[1]=0x21;fields[2]=0;gap_cb(&e,gap_arg);glass_ble_central_read(&s);assert(s.count==3);
  assert(!glass_ble_central_scan_stop(s.scan_generation)&&!role);
  puts("BLE_SERVICE_FILTER_OK: owned UUID copy; complete/incomplete lists and service data; truncated/misaligned/nonmatching data rejected; scan response merged");
}
int main(void)
{
  pthread_t host;assert(!pthread_create(&host,NULL,host_loop,NULL));assert(!glass_ble_central_init());
  usleep(100000);assert(!atomic_load(&host_dispatches));
  glass_ble_central_hid_wake();for(unsigned i=0;i<2000&&!atomic_load(&hid_polls);i++)usleep(1000);assert(atomic_load(&hid_polls)==1);
  assert(glass_ble_role_claim(0)==-EINVAL&&glass_ble_role_claim(3)==-EINVAL);
  capacity=1;assert(!glass_ble_role_claim(GLASS_BLE_ROLE_HID));assert(glass_ble_central_scan()==-EBUSY);
  glass_ble_role_release(GLASS_BLE_ROLE_HID);capacity=2;assert(!glass_ble_role_claim(GLASS_BLE_ROLE_HID));assert(glass_ble_central_scan()==-EBUSY);
  struct glass_ble_capacity_state slots;glass_ble_capacity_read(&slots);assert(slots.reserved==1&&!slots.central_reservations);
  glass_ble_central_reset();assert(role==GLASS_BLE_ROLE_HID);glass_ble_role_release(GLASS_BLE_ROLE_HID);capacity=3;
  assert(glass_ble_central_reserve(3)==-EINVAL);
  puts("BLE_ROLE_CAPACITY_OK: real owner functions; capacity 1/2/3; exact central counts; independent HID reset/release");
  advertising_metadata();filtered_advertising();
  assert(!glass_ble_central_scan());advertise_peer(1,"One");advertise_peer(2,"Two");advertise_peer(3,"Three");
  struct glass_ble_state s={0};glass_ble_central_read(&s);uint32_t scan=s.scan_generation;
  scan_cancel_error=BLE_HS_EBUSY;assert(glass_ble_central_scan_stop(scan)==-EBUSY);glass_ble_central_read(&s);assert(s.scanning);
  scan_cancel_error=0;assert(!glass_ble_central_scan_stop(scan));
  uint32_t ga=0,gb=0,gc=0,gd=0;assert(glass_ble_central_connect_session(0,scan+1,&ga)==-ESTALE&&!ga);
  assert(!glass_ble_central_connect_session(0,scan,&ga)&&ga);glass_ble_central_read(&s);assert(s.connecting&&s.generation==ga);
  assert(glass_ble_central_connect(1,scan)==-EBUSY);connected();
  assert(glass_ble_central_connect(0,scan)==-EALREADY);assert(glass_ble_role_claim(GLASS_BLE_ROLE_HID)==-EBUSY);
  next_handle=8;assert(!glass_ble_central_connect_session(1,scan,&gb)&&gb!=ga);connected();
  glass_ble_central_read(&s);assert(s.session_count==2&&s.connection_count==2&&s.central_capacity==2&&s.generation==gb);
  glass_ble_capacity_read(&slots);assert(slots.reserved==2&&slots.central_reservations==2&&!slots.hid_reserved);
  assert(glass_ble_central_connect(2,scan)==-ENOSPC);
  struct glass_gatt_request r={.generation=ga,.operation=GLASS_GATT_SERVICES};struct glass_gatt_result out;
  assert(!glass_ble_gatt(&r,&out,NULL)&&out.count==32&&out.truncated);
  r.operation=GLASS_GATT_CHARACTERISTICS;r.start=1;r.end=20;assert(!glass_ble_gatt(&r,&out,NULL)&&out.items[0].value==3);
  r.operation=GLASS_GATT_DESCRIPTORS;assert(!glass_ble_gatt(&r,&out,NULL)&&out.items[0].start==4);
  r.operation=-1;assert(glass_ble_gatt(&r,&out,NULL)==-EINVAL);r.operation=GLASS_GATT_MTU+1;assert(glass_ble_gatt(&r,&out,NULL)==-EINVAL);
  assert(!glass_ble_central_scan());glass_ble_central_read(&s);assert(s.count==2&&s.connection_count==2);scan=s.scan_generation;
  r.operation=GLASS_GATT_READ;r.handle=3;assert(!glass_ble_gatt(&r,&out,NULL)&&out.data[0]==7);
  r.generation=gb;assert(!glass_ble_gatt(&r,&out,NULL)&&out.data[0]==8);
  assert(glass_ble_central_scan_stop(scan+1)==-ESTALE);assert(!glass_ble_central_scan_stop(scan));
  notify(7,10,0);notify(8,1,99);
  assert(!glass_ble_notification(ga,&out)&&out.data[0]==2&&out.dropped==2);
  assert(!glass_ble_notification(gb,&out)&&out.data[0]==99&&!out.dropped);
  r.generation=ga;r.operation=GLASS_GATT_PAIR;assert(!glass_ble_gatt(&r,&out,NULL));
  r.operation=GLASS_GATT_MTU;assert(!glass_ble_gatt(&r,&out,NULL)&&out.mtu==100);
  glass_ble_central_read(&s);assert(s.sessions[0].encrypted&&!s.sessions[1].encrypted&&s.sessions[1].mtu==23);
  r.operation=GLASS_GATT_SUBSCRIBE;r.handle=4;r.length=2;r.data[0]=1;assert(!glass_ble_gatt(&r,&out,NULL));
  assert(glass_ble_central_disconnect_generation(UINT32_MAX)==-ESTALE);
  struct remote old=remotes[7];termination_error=BLE_HS_EBUSY;
  assert(glass_ble_central_disconnect_generation(ga)==-EBUSY);glass_ble_central_read(&s);
  assert(s.sessions[0].disconnecting&&s.sessions[1].connected&&s.session_count==2);
  assert(glass_ble_notification(ga,&out)==-ENOTCONN);termination_error=0;
  assert(!glass_ble_central_disconnect_generation(ga));glass_ble_capacity_read(&slots);assert(slots.reserved==1&&slots.central_reservations==1);
  unsigned before=terminations;struct ble_gap_event late={.type=BLE_GAP_EVENT_CONNECT};late.connect.conn_handle=8;
  old.gap(&late,old.arg);assert(terminations==before);
  next_handle=7;assert(!glass_ble_central_connect_session(0,scan,&gc)&&gc!=ga);connected();
  assert(!glass_ble_central_select(gb));glass_ble_central_read(&s);assert(s.generation==gb&&s.connection_count==2);
  r.generation=ga;r.operation=GLASS_GATT_READ;r.length=0;assert(glass_ble_gatt(&r,&out,NULL)==-ESTALE);
  assert(glass_ble_notification(ga,&out)==-ESTALE);
  puts("BLE_MULTI_LINK_OK: two ordinary peers; HID excluded; duplicate/third admission rejected; per-link discovery/read/notify/MTU/pairing; exact disconnect; late GAP cannot terminate another owner");
  atomic_store(&stall,true);struct query qa={.r={.generation=gc,.operation=GLASS_GATT_READ,.handle=3}};
  struct query qb={.r={.generation=gb,.operation=GLASS_GATT_READ,.handle=3}};pthread_t ta,tb;
  assert(!pthread_create(&ta,NULL,query_thread,&qa));struct remote stale=wait_read(7);
  assert(!pthread_create(&tb,NULL,query_thread,&qb));struct remote rb=wait_read(8);
  assert(glass_ble_gatt(&qa.r,&out,NULL)==-EBUSY);answer(8,rb);pthread_join(tb,NULL);
  assert(!qb.result&&qb.out.data[0]==8&&!atomic_load(&qa.finished));atomic_store(&qa.cancel,true);pthread_join(ta,NULL);
  assert(qa.result==-ECANCELED);glass_ble_central_read(&s);assert(s.connection_count==1&&s.sessions[0].generation==gb);
  assert(!glass_ble_central_connect_session(0,scan,&gd)&&gd!=gc);connected();clear_read(7);
  struct query qd={.r={.generation=gd,.operation=GLASS_GATT_READ,.handle=3}};
  assert(!pthread_create(&ta,NULL,query_thread,&qd));struct remote rd=wait_read(7);
  answer(7,stale);usleep(60000);assert(!atomic_load(&qd.finished));answer(7,rd);pthread_join(ta,NULL);assert(!qd.result&&qd.out.data[0]==7);
  clear_read(7);clear_read(8);struct query qreset={.r={.generation=gd,.operation=GLASS_GATT_READ,.handle=3}};
  memset(&qb,0,sizeof(qb));qb.r=(struct glass_gatt_request){.generation=gb,.operation=GLASS_GATT_READ,.handle=3};
  assert(!pthread_create(&ta,NULL,query_thread,&qreset));wait_read(7);assert(!pthread_create(&tb,NULL,query_thread,&qb));wait_read(8);
  glass_ble_central_reset();pthread_join(ta,NULL);pthread_join(tb,NULL);assert(qreset.result==-EIO&&qb.result==-EIO);
  assert(!role);glass_ble_central_read(&s);assert(!s.connection_count&&!s.session_count);
  glass_ble_role_release(GLASS_BLE_ROLE_HID);
  puts("BLE_MULTI_GATT_OK: independent GATT waiters; cancellation isolation; reused handle rejects late data; reset wakes both");
  atomic_store(&stall,false);
  assert(!glass_ble_central_scan());advertise_peer(1,"One");advertise_peer(2,"Two");
  glass_ble_central_read(&s);scan=s.scan_generation;assert(!glass_ble_central_scan_stop(scan));
  next_handle=7;assert(!glass_ble_central_connect_session(0,scan,&ga));connected();
  next_handle=8;assert(!glass_ble_central_connect_session(1,scan,&gb));connected();
  r=(struct glass_gatt_request){.generation=ga,.operation=GLASS_GATT_PAIR};assert(!glass_ble_gatt(&r,&out,NULL));
  struct remote suspended=remotes[7];
  termination_error=BLE_HS_EBUSY;
  assert(glass_ble_central_mouse_pause()==-EBUSY);
  assert(glass_ble_role_claim(GLASS_BLE_ROLE_HID)==-EBUSY);
  assert(glass_ble_central_mouse_resume()==-EBUSY);
  termination_error=0;assert(!glass_ble_central_mouse_pause());
  atomic_store(&auto_connect,true);next_handle=7;
  /* Only one physical initiator is allowed at a time. Cancel the second
   * logical owner before restoring this fault-recovery case. */
  assert(!glass_ble_central_disconnect_generation(gb));
  assert(!glass_ble_central_mouse_resume());atomic_store(&auto_connect,false);
  suspended=remotes[7];
  next_handle=8;assert(!glass_ble_central_connect_session(1,scan,&gb));connected();
  atomic_store(&stall,true);struct query qpause={.r={.generation=ga,.operation=GLASS_GATT_READ,.handle=3}};
  assert(!pthread_create(&ta,NULL,query_thread,&qpause));struct remote paused_read=wait_read(7);
  assert(!glass_ble_central_mouse_pause());pthread_join(ta,NULL);assert(qpause.result==-ECANCELED);
  glass_ble_central_read(&s);assert(s.mouse_paused&&s.session_count==2&&!s.connection_count);
  assert(s.sessions[0].mouse_paused&&s.sessions[1].mouse_paused);
  glass_ble_capacity_read(&slots);assert(!slots.reserved);
  assert(!glass_ble_role_claim(GLASS_BLE_ROLE_HID));assert(glass_ble_central_scan()==-EBUSY);
  assert(glass_ble_central_mouse_resume()==-EBUSY);
  assert(!glass_ble_central_disconnect_generation(gb)); /* App exits while paused. */
  glass_ble_role_release(GLASS_BLE_ROLE_HID);atomic_store(&auto_connect,true);next_handle=7;
  assert(!glass_ble_central_mouse_resume());atomic_store(&auto_connect,false);
  glass_ble_central_read(&s);assert(!s.mouse_paused&&s.connection_count==1&&s.session_count==1);
  assert(s.generation==ga&&s.encrypted); /* Logical owner survives, security restored. */
  struct ble_gap_event old_disconnect={.type=BLE_GAP_EVENT_DISCONNECT};old_disconnect.disconnect.conn.conn_handle=7;
  suspended.gap(&old_disconnect,suspended.arg);glass_ble_central_read(&s);assert(s.connected);
  clear_read(7);struct query qrestored={.r={.generation=ga,.operation=GLASS_GATT_READ,.handle=3}};
  assert(!pthread_create(&ta,NULL,query_thread,&qrestored));struct remote restored_read=wait_read(7);
  answer(7,paused_read);usleep(60000);assert(!atomic_load(&qrestored.finished));
  answer(7,restored_read);pthread_join(ta,NULL);assert(!qrestored.result);
  atomic_store(&stall,false);next_handle=8;
  assert(!glass_ble_central_connect_session(1,scan,&gb));connected();
  assert(!glass_ble_central_mouse_pause());atomic_store(&auto_connect,true);
  assert(!glass_ble_central_mouse_resume());atomic_store(&auto_connect,false);
  glass_ble_central_read(&s);assert(!s.restoring&&s.restore_count==2&&s.connection_count==2);
  assert(s.restores[0].generation==ga&&s.restores[1].generation==gb);
  for(unsigned i=0;i<2;i++)assert(s.restores[i].attempted&&s.restores[i].connected&&!s.restores[i].error);
  r=(struct glass_gatt_request){.generation=ga,.operation=GLASS_GATT_READ,.handle=3};
  assert(!glass_ble_gatt(&r,&out,NULL)&&out.data[0]==7);
  r.generation=gb;assert(!glass_ble_gatt(&r,&out,NULL)&&out.data[0]==8);
  assert(!glass_ble_central_mouse_pause());restore_fail_peer=1;atomic_store(&auto_connect,true);
  assert(glass_ble_central_mouse_resume()==-ETIMEDOUT);atomic_store(&auto_connect,false);restore_fail_peer=0;
  glass_ble_central_read(&s);assert(!s.restoring&&!s.mouse_paused&&s.restore_count==2&&s.connection_count==1);
  assert(s.restores[0].generation==ga&&s.restores[0].attempted&&!s.restores[0].connected&&s.restores[0].error==-ETIMEDOUT);
  assert(s.restores[1].generation==gb&&s.restores[1].attempted&&s.restores[1].connected&&!s.restores[1].error);
  assert(s.session_count==1&&s.sessions[0].generation==gb);
  r.generation=gb;assert(!glass_ble_gatt(&r,&out,NULL)&&out.data[0]==8);
  puts("BLE_MULTI_RESTORE_OK: both peers restored with independent outcomes; first peer failure preserves second peer and its GATT");
  assert(!glass_ble_central_mouse_pause());glass_ble_central_reset();
  assert(!glass_ble_central_mouse_resume());glass_ble_central_read(&s);assert(!s.session_count&&!s.mouse_paused);
  puts("BLE_MOUSE_HANDOFF_OK: pause cancels GATT; HID excludes central; owner exit cancels restore; same logical owner and encryption restored; stale GAP/GATT rejected; reset drops restore");
  for(unsigned i=0;i<100;i++){assert(!glass_ble_central_scan());assert(!glass_ble_central_scan_stop(0));}
  unsigned idle=atomic_load(&host_dispatches);usleep(100000);assert(atomic_load(&host_dispatches)==idle);
  atomic_store(&stop_host,true);assert(!glass_ble_central_scan());pthread_join(host,NULL);glass_ble_central_reset();
  puts("BLE_COMMAND_IDLE_OK: no empty dispatches; 200 scan/stop commands without lost wakeups");
  puts("BLE_CENTRAL_OK: production multi-session central and real capacity owner exercised under ASan/UBSan");return 0;
}
