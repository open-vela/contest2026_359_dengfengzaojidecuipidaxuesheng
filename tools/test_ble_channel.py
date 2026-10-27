#!/usr/bin/env python3
"""Exercise the complete shared GATT service with deterministic NimBLE IO."""
import argparse
import os
from pathlib import Path
import subprocess
from render_all_ui import function

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
repo = Path(__file__).resolve().parents[1]
ble = repo/'app/espdl-quickapp/overlay/apps/wireless/bluetooth/nimble'
out = a.output.resolve()
(out/'host').mkdir(parents=True, exist_ok=True)
hid = (ble/'glass_ble_hid.c').read_text()
gap = function(hid, 'gap_event')
assert gap.index('default: return 0;') < gap.index('pthread_mutex_lock(&g_lock)')
assert 'glass_ble_channel_update' in gap and 'glass_ble_channel_subscribe' in gap
poll = function(hid, 'glass_ble_hid_poll')
assert poll.index('pthread_mutex_unlock(&g_lock)') < poll.index('glass_ble_channel_poll')
assert 'glass_ble_channel_update' in function(hid, 'glass_ble_hid_reset')
assert 'glass_ble_channel_init' in function(hid, 'glass_ble_hid_service_init')
header = r'''#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#define BLE_HS_CONN_HANDLE_NONE 65535
#define BLE_ATT_ERR_UNLIKELY 14
#define BLE_ATT_ERR_INSUFFICIENT_AUTHOR 8
#define BLE_ATT_ERR_INSUFFICIENT_RES 17
#define BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN 13
#define BLE_GATT_ACCESS_OP_READ_CHR 1
#define BLE_GATT_ACCESS_OP_WRITE_CHR 2
#define BLE_GATT_CHR_F_READ 1
#define BLE_GATT_CHR_F_WRITE 2
#define BLE_GATT_CHR_F_NOTIFY 4
#define BLE_GATT_CHR_F_READ_ENC 8
#define BLE_GATT_CHR_F_WRITE_ENC 16
#define BLE_GATT_SVC_TYPE_PRIMARY 1
typedef struct {uint8_t type;} ble_uuid_t;
typedef struct {ble_uuid_t u;uint8_t value[16];} ble_uuid128_t;
#define BLE_UUID128_INIT(...) {.u={128},.value={__VA_ARGS__}}
struct os_mbuf {uint16_t len;uint8_t data[512];};
#define OS_MBUF_PKTLEN(m) ((m)->len)
struct ble_gatt_access_ctxt {int op;struct os_mbuf *om;};
struct ble_gatt_chr_def {const ble_uuid_t *uuid;int (*access_cb)(uint16_t,uint16_t,struct ble_gatt_access_ctxt *,void *);void *arg;unsigned flags;uint16_t *val_handle;};
struct ble_gatt_svc_def {unsigned type;const ble_uuid_t *uuid;const struct ble_gatt_chr_def *characteristics;};
int ble_gatts_count_cfg(const struct ble_gatt_svc_def *);
int ble_gatts_add_svcs(const struct ble_gatt_svc_def *);
uint16_t ble_att_mtu(uint16_t);
int os_mbuf_append(struct os_mbuf *,const void *,size_t);
int ble_hs_mbuf_to_flat(const struct os_mbuf *,void *,uint16_t,uint16_t *);
struct os_mbuf *ble_hs_mbuf_from_flat(const void *,uint16_t);
int ble_gatts_notify_custom(uint16_t,uint16_t,struct os_mbuf *);
'''
(out/'host/ble_hs.h').write_text(header)
(out/'host/ble_hs_mbuf.h').write_text('#include "ble_hs.h"\n')
flags = ['-std=gnu11', '-D_GNU_SOURCE', '-DGLASS_BLE_CHANNEL_TIMEOUT_MS=150',
         '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation',
         '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie', '-pthread']
subprocess.run(['gcc', *flags, '-I'+str(out), '-I'+str(ble),
                str(repo/'app/espdl-quickapp/tests/ble_channel_host.c'),
                str(ble/'glass_ble_channel.c'), '-o', str(out/'test')], check=True)
subprocess.run([str(out/'test')], check=True,
               env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1', 'UBSAN_OPTIONS': 'halt_on_error=1'})
