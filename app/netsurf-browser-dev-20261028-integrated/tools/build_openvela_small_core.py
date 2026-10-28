#!/usr/bin/env python3
"""Rebuild the prepared NetSurf core for size without changing input sources."""
import argparse
import json
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--core',type=Path,required=True)
p.add_argument('--toolchain',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
out=a.output.resolve(); out.mkdir(parents=True,exist_ok=True)
makefile=out/'Makefile.port'
source=(a.core/'Makefile.port').read_text()
if 'BASE := -O2' not in source: p.error('unexpected prepared makefile')
makefile.write_text(source.replace('-O2','-Os'))
prefix=str(a.toolchain.resolve()/'riscv32-esp-elf-')
command=['make','-C',str(a.core),'-f','target-flags.mk','-f',str(makefile),'port',
         'BUILD='+str(out),'CC='+prefix+'gcc','AR='+prefix+'ar','-j6']
(out/'build-command.json').write_text(json.dumps(command,indent=2))
subprocess.run(command,check=True)
print('SIZE_CORE_BUILT',out/'libnetsurf-port.a')
