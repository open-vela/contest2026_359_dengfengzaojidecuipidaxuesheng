#!/usr/bin/env python3
"""Fault-test production persistence with the pinned SDK RAM store and types."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
from prepare_nimble_store import apply
from render_all_ui import function
from ble_candidate_identity import create

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--apps', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
repo = Path(__file__).resolve().parents[1]
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
create(out/'identity-apps',out/'identity')
shutil.copy2(out/'identity-apps/wireless/bluetooth/nimble/glass_ble_candidate.h',out/'glass_ble_candidate.h')
sdk = a.apps.resolve() / 'wireless/bluetooth/nimble/mynewt-nimble'
for folder in ['host', 'sysinit', 'syscfg', 'os']:
    (out / folder).mkdir(exist_ok=True)
(out / 'syscfg/syscfg.h').write_text('''#pragma once
#define MYNEWT_VAL(x) MYNEWT_VAL_ ## x
#define MYNEWT_VAL_BLE_STORE_MAX_BONDS 3
#define MYNEWT_VAL_BLE_STORE_MAX_CCCDS 8
#define MYNEWT_VAL_BLE_MULTI_ADV_INSTANCES 0
#define MYNEWT_VAL_BLE_LL_CFG_FEAT_LL_PRIVACY 0
#define MYNEWT_VAL_BLE_LL_CFG_FEAT_LL_EXT_ADV 0
''')
(out / 'os/os.h').write_text('#pragma once\n')
(out / 'sysinit/sysinit.h').write_text('''#pragma once
extern int sysinit_active,sysinit_calls;
#define SYSINIT_ASSERT_ACTIVE() do {assert(sysinit_active);sysinit_calls++;} while(0)
''')
hs = (sdk / 'nimble/host/include/host/ble_hs.h').read_text()
errors = '\n'.join(line for line in hs.splitlines() if line.startswith('#define BLE_HS_E'))
(out / 'host/ble_hs.h').write_text('''#pragma once
#include <assert.h>
#include "host/ble_store.h"
''' + errors + '''
struct hs_config {
 int (*store_read_cb)(int,const union ble_store_key *,union ble_store_value *);
 int (*store_write_cb)(int,const union ble_store_value *);
 int (*store_delete_cb)(int,const union ble_store_key *);
};
extern struct hs_config ble_hs_cfg;
#define BLE_HS_LOG(...) ((void)0)
static inline void ble_hs_log_flat_buf(const void *p,int n){(void)p;(void)n;}
''')
original = sdk / 'nimble/host/store/ram/src/ble_store_ram.c'
target = out / 'sdk/wireless/bluetooth/nimble/mynewt-nimble/nimble/host/store/ram/src/ble_store_ram.c'
target.parent.mkdir(parents=True, exist_ok=True)
shutil.copy2(original, target)
report = apply(out / 'sdk')
# A second application must leave the file identical.
assert apply(out / 'sdk') == report
store = (sdk / 'nimble/host/src/ble_store.c').read_text()
keys = '\n'.join('void\n' + function(store, n) for n in
                 ['ble_store_key_from_value_sec', 'ble_store_key_from_value_cccd'])
(out / 'sdk_keys.inc').write_text(keys)
source = repo / 'app/espdl-quickapp/tests/ble_store_host.c'
ble = repo / 'app/espdl-quickapp/overlay/apps/wireless/bluetooth/nimble'
flags = ['gcc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-pthread', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
         '-fno-pie', '-no-pie', '-I' + str(out), '-I' + str(ble),
         '-I' + str(sdk / 'nimble/include'),
         '-I' + str(sdk / 'nimble/host/include'),
         '-I' + str(sdk / 'nimble/host/store/ram/include'),
         '-DSDK_RAM_FILE="' + str(target) + '"', str(source)]
subprocess.run(flags + ['-o', str(out / 'test')], check=True)
env = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1', 'UBSAN_OPTIONS': 'halt_on_error=1'}
r = subprocess.run([str(out / 'test')], check=True, capture_output=True, text=True, env=env)
(out / 'test.log').write_text(r.stdout + r.stderr)
print(r.stdout, end='')
# The baseline defect must be caught by the same multi-record test.
baseline = original.read_text().replace(
    'memmove(dst, src, (size_t)move_count * (size_t)value_size);',
    'memmove(dst, src, move_count);')
assert 'memmove(dst, src, move_count);' in baseline
target.write_text(baseline)
subprocess.run(flags + ['-o', str(out / 'negative')], check=True)
r = subprocess.run([str(out / 'negative'), 'compaction'], capture_output=True, text=True, env=env)
assert r.returncode != 0, 'Unpatched SDK must fail the record-preservation test'
(out / 'negative.log').write_text(r.stdout + r.stderr)
print('BLE_STORE_NEGATIVE_OK: unpatched SDK record compaction rejected')
apply(out / 'sdk')
(out / 'sdk-proof.txt').write_text(str(report) + '\n' +
    'input_sha256=' + hashlib.sha256(original.read_bytes()).hexdigest() + '\n')
