#!/usr/bin/env python3
"""Cross-link regression for legal self branches and address-dependent imports."""
import argparse
from pathlib import Path
import struct
import subprocess
from openvela_relax_module import Elf32, resolve_local
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--toolchain',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
prefix=str(a.toolchain.resolve()/'riscv32-esp-elf-')
source=out/'fixture.S'
source.write_text('''.option norelax
.text
.globl entry
entry:
 beq a0, a0, entry
 call helper
 call host_import
 lui a1, %hi(value)
 addi a1, a1, %lo(value)
 sw a0, %lo(value+16)(a1)
 ret
helper:
 beq a1, a1, helper
 ret
.data
value: .word entry
''')
raw=out/'raw.o';resolved=out/'resolved.o'
subprocess.run([prefix+'gcc','-march=rv32imafc','-mabi=ilp32f','-c',str(source),'-o',str(raw)],check=True)
report=resolve_local(raw,resolved,prefix,out)
assert report['local_control_flow_resolved']==3,report
elf=Elf32(resolved.read_bytes());kinds=[]
for s in elf.sections:
    if s[1]==4:
        kinds.extend(struct.unpack_from('<I',elf.data,x+4)[0]&255 for x in range(s[4],s[4]+s[5],12))
assert sorted(kinds)==[1,19,26,27,28],kinds
assert report['address_immediates_normalized']==2,report
raw_elf=Elf32(raw.read_bytes())
for s in elf.sections:
    if s[1]==4 and s[7]==elf.names.index('.text'):
        for p in range(s[4],s[4]+s[5],12):
            offset,info,addend=struct.unpack_from('<IIi',elf.data,p)
            if info&255==28:
                old=struct.unpack_from('<I',raw_elf.named('.text'),offset)[0]
                new=struct.unpack_from('<I',elf.named('.text'),offset)[0]
                assert addend==16 and old&0xfe000f80 and not new&0xfe000f80
                assert old&~0xfe000f80==new
assert elf.named('.text')[:4]==Elf32(raw.read_bytes()).named('.text')[:4] # legal beq +0
print('PASS: zero-distance branches resolved; external call, absolute data pointer and HI20/LO12 retained; two GNU reference bases identical')
