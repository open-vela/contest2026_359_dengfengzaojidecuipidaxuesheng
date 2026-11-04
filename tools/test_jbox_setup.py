#!/usr/bin/env python3
"""Exercise native onboarding's real mailbox/worker with isolated peripherals."""
import argparse,json,os,pathlib,shutil,subprocess
root=pathlib.Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=pathlib.Path,required=True)
p.add_argument('--cjson-source',type=pathlib.Path,default=pathlib.Path('D:/esp/v5.5.2/esp-idf/components/json/cJSON/cJSON.c'))
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
if not a.cjson_source.is_file():p.error('Supply an existing cJSON.c via --cjson-source')
for name,data in {
 'nuttx/config.h':'#pragma once\n',
 'psa/crypto.h':'#pragma once\n#include <stdint.h>\ntypedef uint32_t mbedtls_svc_key_id_t;\ntypedef struct {uint32_t opaque[8];} psa_cipher_operation_t;\n',
}.items():
 f=out/'include'/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_text(data)
desktop=root/'app/espdl-quickapp/overlay/apps/system/desktop'
ble=root/'app/espdl-quickapp/overlay/apps/wireless/bluetooth/nimble'
exe=out/('test.exe' if os.name=='nt' else 'test')
command=[shutil.which('gcc') or 'gcc','-std=gnu11','-D_DEFAULT_SOURCE','-Wall','-Wextra','-Werror','-O1','-g','-pthread']
if os.name=='nt':command+=['-DESTALE=116'] # Linux errno missing from MinGW; fixture only.
else:command+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie']
for include in [out/'include',desktop,ble,root/'app/espdl-quickapp/overlay/apps/system/c6probe',root/'tools/c6',a.cjson_source.resolve().parent]:command+=['-I'+str(include)]
command += [str(root/'app/espdl-quickapp/tests/jbox_setup_host.c'),str(a.cjson_source.resolve()),'-o',str(exe)]
subprocess.run(command,check=True)
result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30,
 env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0','UBSAN_OPTIONS':'halt_on_error=1'})
# The resident worker intentionally lives until process exit. Cleanup/secret
# erasure of individual attempts is asserted by the native fixture.
(out/'run.log').write_text(result.stdout+result.stderr)
print(result.stdout,end='');print(result.stderr,end='')
passed=result.returncode==0 and 'JBOX_SETUP_HOST_OK' in result.stdout
(out/'RESULT.json').write_text(json.dumps({'passed':passed,'native_worker':True,
 'current_wifi_only':True,'peripherals':'isolated substitutes','sanitizers':os.name!='nt',
 'real_BLE_tested':False,'firmware_built':False},indent=2))
if not passed:raise SystemExit(result.returncode or 1)
