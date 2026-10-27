#!/usr/bin/env python3
"""Exercise the production Makefile against the pinned SDK's real syscfg."""
import argparse,json,subprocess
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--nimble',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
repo=Path(__file__).resolve().parents[1]
makefile=repo/'app/espdl-quickapp/overlay/apps/wireless/bluetooth/nimble/Makefile'
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
app=out/'apps';app.mkdir(exist_ok=True)
(app/'Make.defs').write_text('CONFIG_SYSTEM_GLASS_BLE=y\n')
(app/'Application.mk').write_text("print-flags:\n\t@printf '%s\\n' $(CFLAGS)\n")
source=out/'capacity.c'
source.write_text('''#include "syscfg/syscfg.h"
#if MYNEWT_VAL(BLE_MAX_CONNECTIONS) != EXPECTED_CAPACITY
#error Incorrect connection capacity
#endif
unsigned char capacity[MYNEWT_VAL(BLE_MAX_CONNECTIONS)];
''')
results=[]
for count in [1,2,3,8]:
    flags=subprocess.check_output(['make','--no-print-directory','-s','-f',str(makefile),
        'APPDIR='+str(app),'NIMBLE_ROOT='+str(a.nimble.resolve()),
        'CONFIG_NIMBLE_BLE_MAX_CONN='+str(count),'print-flags'],text=True).splitlines()
    # Only preprocess here: NuttX's target ABI is covered by the firmware audit.
    run=['gcc',*flags,'-DEXPECTED_CAPACITY='+str(count),'-E',str(source),'-o',str(out/f'capacity-{count}.i')]
    subprocess.run(run,check=True)
    missing=[f for f in flags if not f.startswith('-DMYNEWT_VAL_BLE_MAX_CONNECTIONS=')]
    bad=subprocess.run(['gcc',*missing,'-DEXPECTED_CAPACITY=2','-E',str(source)],
                       capture_output=True,text=True)
    assert bad.returncode!=0,'Regression fixture did not reject the original default-one behavior'
    results.append({'configured':count,'actual_syscfg_matches':True,'original_bug_rejected':True})
(out/'VERIFICATION.json').write_text(json.dumps(results,indent=2)+'\n')
print('NIMBLE_CAPACITY_OK: real Makefile + pinned syscfg; capacities 1/2/3/8; unmapped default-one rejected')
