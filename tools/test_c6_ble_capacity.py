#!/usr/bin/env python3
"""Test the prepared real C6 initializer with API stubs; never access hardware."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

from prepare_c6_ble_capacity import BEGIN, END, GUARD, apply, configure, patch_source
from render_all_ui import function


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--project',type=Path,required=True)
    p.add_argument('--baseline-config',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    source=(a.project/'main/slave_bt.c').read_text()
    assert patch_source(source)==source
    guard=source[source.index(BEGIN):source.index(END)+len(END)]
    assert guard==GUARD.rstrip()
    base=a.baseline_config.read_text();updated=configure(base)
    assert configure(updated)==updated
    assert updated.replace('CONFIG_BT_LE_MAX_CONNECTIONS=8','CONFIG_BT_LE_MAX_CONNECTIONS=3')==base
    rejected=0
    for bad in [base.replace('CONFIG_BT_LE_MAX_CONNECTIONS=3','CONFIG_BT_LE_MAX_CONNECTIONS=5'),
                base+'\nCONFIG_BT_LE_MAX_CONNECTIONS=3\n',
                base.replace('CONFIG_IDF_TARGET="esp32c6"','CONFIG_IDF_TARGET="esp32c3"'),
                base+'\nCONFIG_BT_NIMBLE_ENABLED=y\n',
                base.replace('CONFIG_BT_CONTROLLER_ONLY=y','CONFIG_BT_CONTROLLER_ONLY=n')]:
        try:configure(bad)
        except ValueError:rejected+=1
        else:raise AssertionError('Invalid configuration accepted')
    with tempfile.TemporaryDirectory() as temp:
        project=Path(temp);(project/'main').mkdir()
        target=project/'main/slave_bt.c';target.write_text(source)
        config=project/'sdkconfig';config.write_text('invalid config\n')
        try:apply(project,config)
        except ValueError:pass
        else:raise AssertionError('Invalid config was written')
        assert target.read_text()==source and config.read_text()=='invalid config\n'
    prefix=r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef TEST_CONNECTIONS
#define TEST_CONNECTIONS 8
#endif
#define CONFIG_IDF_TARGET_ESP32C6 1
#define CONFIG_BT_ENABLED 1
#define CONFIG_BT_CONTROLLER_ONLY 1
#define CONFIG_BT_CONTROLLER_ENABLED 1
#define CONFIG_BT_LE_MAX_CONNECTIONS TEST_CONNECTIONS
#define DEFAULT_BT_LE_MAX_CONNECTIONS CONFIG_BT_LE_MAX_CONNECTIONS
#ifdef WRONG_HOST
#define CONFIG_BT_NIMBLE_ENABLED 1
#endif
#define BSSID_BYTES_SIZE 6
#define BLUETOOTH_BLE 1
#define BLUETOOTH_HCI 1
#define SOC_ESP_NIMBLE_CONTROLLER 1
#define ESP_IDF_VERSION_VAL(a,b,c) (((a)<<16)|((b)<<8)|(c))
#define ESP_IDF_VERSION ESP_IDF_VERSION_VAL(5,5,0)
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 259
#define ESP_ERR_NO_MEM 257
#define ESP_MAC_BT 1
#define ESP_BT_MODE_BLE 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
typedef int esp_err_t;
typedef struct {uint16_t nimble_max_connections;uint32_t marker;} esp_bt_controller_config_t;
#ifdef BAD_INITIALIZER
#define BT_CONTROLLER_INIT_CONFIG_DEFAULT() {.nimble_max_connections=7,.marker=0x5a}
#else
#define BT_CONTROLLER_INIT_CONFIG_DEFAULT() {.nimble_max_connections=DEFAULT_BT_LE_MAX_CONNECTIONS,.marker=0x5a}
#endif
static unsigned init_calls,enable_calls,registered,given,guard_errors;
static int mode,callback;
static void *vhci_send_sem;
#define vhci_host_cb callback
/* The production error macro aborts; this substitute records the rejection
 * so the fixture can assert that controller initialization never occurred. */
#define ESP_ERROR_CHECK(x) do{int result=(x);if(result){guard_errors++;return result;}}while(0)
static int esp_read_mac(uint8_t *mac,int type){assert(type==ESP_MAC_BT);for(unsigned i=0;i<6;i++)mac[i]=i;return 0;}
static int esp_bt_controller_init(const esp_bt_controller_config_t *c){assert(c->nimble_max_connections==8&&c->marker==0x5a);init_calls++;return 0;}
static int esp_bt_controller_enable(int m){assert(init_calls==1&&m==ESP_BT_MODE_BLE);enable_calls++;return 0;}
static int esp_vhci_host_register_callback(void *cb){assert(cb==&callback&&enable_calls==1);registered++;return mode==1?-77:0;}
static void *xSemaphoreCreateBinary(void){assert(registered==1);return mode==2?NULL:&callback;}
static int xSemaphoreGive(void *sem){assert(sem==&callback);given++;return 1;}
'''
    tests=r'''
int main(int argc,char **argv){assert(argc==2);mode=atoi(argv[1]);int result=initialise_bluetooth();
#ifdef BAD_INITIALIZER
 assert(result==ESP_ERR_INVALID_STATE&&guard_errors==1&&!init_calls&&!enable_calls&&!registered&&!given);
#else
 assert(!guard_errors&&init_calls==1&&enable_calls==1&&registered==1);
 if(mode==0)assert(!result&&given==1);
 else if(mode==1)assert(result==-77&&!given);
 else assert(mode==2&&result==ESP_ERR_NO_MEM&&!given);
#endif
 puts("C6_INITIALIZER_CASE_OK");return 0;}
'''
    test=out/'initializer.c';test.write_text(prefix+guard+'\n'+function(source,'initialise_bluetooth')+tests)
    flags=['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
           '-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie']
    env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'}
    results=[]
    for name,extra,cases in [('eight',[],[0,1,2]),('bad-initializer',['-DBAD_INITIALIZER'],[0])]:
        binary=out/name
        subprocess.run([*flags,*extra,str(test),'-o',str(binary)],check=True)
        for case in cases:
            result=subprocess.run([str(binary),str(case)],env=env,capture_output=True,text=True)
            (out/f'{name}-{case}.log').write_text(result.stdout+result.stderr)
            result.check_returncode();results.append({'case':name,'mode':case,'passed':True})
    for name,extra in [('three-rejected',['-DTEST_CONNECTIONS=3']),('second-host-rejected',['-DWRONG_HOST'])]:
        result=subprocess.run([*flags,*extra,'-c',str(test),'-o',str(out/(name+'.o'))],capture_output=True,text=True)
        (out/(name+'.log')).write_text(result.stdout+result.stderr)
        assert result.returncode!=0,'Invalid configuration compiled'
        assert 'C6 must configure eight' in result.stderr or 'Host belongs to P4' in result.stderr
        results.append({'case':name,'expected_compile_rejection':True,'passed':True})
    report={'passed':True,'scope':'actual_initializer_API_stubs_not_radio',
            'source_sha256':hashlib.sha256(source.encode()).hexdigest(),
            'rejected_configurations':rejected,'results':results,'device_actions':False}
    (out/'VERIFICATION.json').write_text(json.dumps(report,indent=2)+'\n')
    print('C6_EIGHT_INIT_OK: real initializer; eight reaches controller; invalid runtime initializer rejected; callback/alloc errors; 3-slot and second-host builds rejected; ASan/UBSan')


if __name__=='__main__':main()
