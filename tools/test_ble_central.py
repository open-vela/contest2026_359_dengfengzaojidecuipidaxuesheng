#!/usr/bin/env python3
"""Exercise the production NimBLE boundary using deterministic host callbacks."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path
from render_all_ui import function
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
p.add_argument('--connections',type=int,choices=[2,8],default=2);a=p.parse_args()
root=Path(__file__).resolve().parents[1];out=a.output.resolve();(out/'host').mkdir(parents=True,exist_ok=True)
header='''#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#define BLE_HS_EDONE 14
#define BLE_HS_ENOTCONN 7
#define BLE_HS_ETIMEOUT 13
#define BLE_HS_EBUSY 15
#define BLE_HS_EALREADY 2
#define BLE_HS_ENOMEM 6
#define BLE_HS_EINVAL 3
#define BLE_HS_CONN_HANDLE_NONE 65535
#define BLE_ERR_REM_USER_CONN_TERM 19
#define BLE_GAP_REPEAT_PAIRING_IGNORE 0
enum {BLE_GAP_EVENT_DISC=1,BLE_GAP_EVENT_DISC_COMPLETE,BLE_GAP_EVENT_CONNECT,BLE_GAP_EVENT_DISCONNECT,BLE_GAP_EVENT_ENC_CHANGE,BLE_GAP_EVENT_MTU,BLE_GAP_EVENT_NOTIFY_RX,BLE_GAP_EVENT_REPEAT_PAIRING};
typedef struct {uint8_t type,val[6];} ble_addr_t;
typedef struct {uint8_t type;} ble_uuid_t;
typedef union {ble_uuid_t u;} ble_uuid_any_t;
struct os_mbuf {uint16_t len;uint8_t data[513];};
#define OS_MBUF_PKTLEN(m) ((m)->len)
struct ble_gap_conn_desc {uint16_t conn_handle;struct {bool encrypted,bonded;} sec_state;};
struct ble_gap_event {uint8_t type;union {
 struct {ble_addr_t addr;uint8_t event_type;int rssi;const uint8_t *data;unsigned length_data;} disc;
 struct {int status;uint16_t conn_handle;} connect,enc_change;
 struct {struct ble_gap_conn_desc conn;} disconnect;
 struct {uint16_t conn_handle,value;} mtu;
 struct {uint16_t conn_handle,attr_handle;bool indication;struct os_mbuf *om;} notify_rx;
};};
struct ble_gap_disc_params {uint8_t passive,filter_duplicates;uint16_t itvl,window;};
struct ble_gatt_error {int status;};
struct ble_gatt_svc {ble_uuid_any_t uuid;uint16_t start_handle,end_handle;};
struct ble_gatt_chr {ble_uuid_any_t uuid;uint16_t def_handle,val_handle;uint8_t properties;};
struct ble_gatt_dsc {ble_uuid_any_t uuid;uint16_t handle;};
struct ble_gatt_attr {uint16_t handle;struct os_mbuf *om;};
typedef int ble_gatt_svc_fn(uint16_t,const struct ble_gatt_error *,const struct ble_gatt_svc *,void *);
typedef int ble_gatt_chr_fn(uint16_t,const struct ble_gatt_error *,const struct ble_gatt_chr *,void *);
typedef int ble_gatt_dsc_fn(uint16_t,const struct ble_gatt_error *,uint16_t,const struct ble_gatt_dsc *,void *);
typedef int ble_gatt_attr_fn(uint16_t,const struct ble_gatt_error *,struct ble_gatt_attr *,void *);
typedef int ble_gatt_mtu_fn(uint16_t,const struct ble_gatt_error *,uint16_t,void *);
int ble_hs_id_infer_auto(int,uint8_t *);uint16_t ble_att_mtu(uint16_t);
char *ble_uuid_to_str(const ble_uuid_t *,char *);
int ble_hs_mbuf_to_flat(const struct os_mbuf *,void *,uint16_t,uint16_t *);
int ble_gap_disc(uint8_t,int32_t,const struct ble_gap_disc_params *,int(*)(struct ble_gap_event *,void *),void *);
int ble_gap_disc_cancel(void);int ble_gap_conn_cancel(void);
int ble_gap_connect(uint8_t,const ble_addr_t *,int32_t,const void *,int(*)(struct ble_gap_event *,void *),void *);
int ble_gap_terminate(uint16_t,uint8_t);int ble_gap_conn_find(uint16_t,struct ble_gap_conn_desc *);int ble_gap_security_initiate(uint16_t);
int ble_gattc_disc_all_svcs(uint16_t,ble_gatt_svc_fn *,void *);
int ble_gattc_disc_all_chrs(uint16_t,uint16_t,uint16_t,ble_gatt_chr_fn *,void *);
int ble_gattc_disc_all_dscs(uint16_t,uint16_t,uint16_t,ble_gatt_dsc_fn *,void *);
int ble_gattc_read(uint16_t,uint16_t,ble_gatt_attr_fn *,void *);
int ble_gattc_write_flat(uint16_t,uint16_t,const void *,uint16_t,ble_gatt_attr_fn *,void *);
int ble_gattc_write_no_rsp_flat(uint16_t,uint16_t,const void *,uint16_t);
int ble_gattc_exchange_mtu(uint16_t,ble_gatt_mtu_fn *,void *);
'''
(out/'host/ble_hs.h').write_text(header);(out/'host/ble_hs_mbuf.h').write_text('#include "ble_hs.h"\n')
(out/'host/ble_store.h').write_text('#include "ble_hs.h"\nint ble_store_util_bonded_peers(ble_addr_t *,int *,int);\n')
(out/'nimble').mkdir(exist_ok=True)
(out/'nimble/nimble_port.h').write_text('''#pragma once
struct ble_npl_event {void (*fn)(struct ble_npl_event *);};
extern _Bool host_context;
static inline void ble_npl_event_init(struct ble_npl_event *e,void (*fn)(struct ble_npl_event *),void *arg){(void)arg;e->fn=fn;}
static inline void *nimble_port_get_dflt_eventq(void){return 0;}
static inline void ble_npl_eventq_put(void *q,struct ble_npl_event *e){(void)q;host_context=1;e->fn(e);host_context=0;}
''')
ble=root/'app/espdl-quickapp/overlay/apps/wireless/bluetooth/nimble'
owner=(ble/'glass_ble.c').read_text()
(out/'role-under-test.inc').write_text('\n\n'.join(function(owner,name) for name in
    ['glass_ble_central_reserve','glass_ble_role_claim','glass_ble_role_release','glass_ble_capacity_read']))
flags=['-std=gnu11','-D_GNU_SOURCE','-O1','-g','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie','-pthread']
flags+=['-DGLASS_BLE_CENTRAL_LIMIT='+str(a.connections)]
harness='ble_eight_host.c' if a.connections==8 else 'ble_central_host.c'
subprocess.run(['gcc',*flags,'-I'+str(out),'-I'+str(ble),str(root/'app/espdl-quickapp/tests'/harness),str(ble/'glass_ble_central.c'),'-o',str(out/'test')],check=True)
result=subprocess.run([str(out/'test')],capture_output=True,text=True,
  env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
(out/'run.log').write_text(result.stdout+result.stderr)
print(result.stdout,end='');print(result.stderr,end='')
report={'scope':'host_simulation_not_radio','ordinary_sessions':a.connections,
        'asan_ubsan':True,'passed':result.returncode==0,'returncode':result.returncode,
        'source_sha256':hashlib.sha256((ble/'glass_ble_central.c').read_bytes()).hexdigest()}
(out/'VERIFICATION.json').write_text(json.dumps(report,indent=2)+'\n')
result.check_returncode()
