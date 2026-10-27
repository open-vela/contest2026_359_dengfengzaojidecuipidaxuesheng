#!/usr/bin/env python3
"""Run the actual bounded JPEG parser under ASan/UBSan with real JPEG files."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--fixtures',type=Path,required=True)
p.add_argument('--nuttx',type=Path,required=True)
p.add_argument('--toolchain',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();repo=Path(__file__).resolve().parents[1]
chip=repo/'app/espdl-quickapp/overlay/nuttx/arch/risc-v/src/esp32p4'
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
fixtures=a.fixtures.resolve();manifest=json.loads((fixtures/'FIXTURES.json').read_text())
for name,text in {'nuttx/config.h':'#pragma once\n#define OK 0\n','nuttx/compiler.h':'#pragma once\n#define FAR\n'}.items():
 f=out/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_text(text)
cases=['struct jpeg_case_s {const char *path;int supported,width,height,components,sampling;};',
 'static const struct jpeg_case_s jpeg_cases[] = {']
for c in manifest['cases']:
 path=fixtures/c['file'];assert hashlib.sha256(path.read_bytes()).hexdigest()==c['sha256']
 cases.append('{'+json.dumps(str(path))+','+','.join(str(int(c[k])) for k in ['supported','width','height','components','sampling'])+'},')
cases+=['};'];(out/'jpeg_cases.h').write_text('\n'.join(cases)+'\n')
source=chip/'esp32p4_jpeg_parse.c';fixture=repo/'app/espdl-quickapp/tests/p4_jpeg_parser_host.c'
cmd=['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
 '-fno-omit-frame-pointer','-fno-pie','-no-pie','-I'+str(out),'-I'+str(chip),
 str(source),str(fixture),'-o',str(out/'parser-host')]
def run(cmd,name):
 r=subprocess.run(cmd,capture_output=True,text=True,timeout=90,
 env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
 (out/(name+'.log')).write_text(r.stdout+r.stderr)
 if r.returncode:raise RuntimeError(name+' failed:\n'+r.stdout+r.stderr)
 return r.stdout
run(cmd,'compile');print(run([str(out/'parser-host')],'run'),end='')
obj=out/'parser-target.o'
run([str(a.toolchain/'riscv32-esp-elf-gcc'),'-c','-Os','-std=gnu11',
 '-march=rv32imafc_zicsr_zifencei','-mabi=ilp32f','-Wall','-Wextra','-Werror',
 '-fstack-usage','-I'+str(chip),'-I'+str(a.nuttx/'include'),str(source),'-o',str(obj)],'target-compile')
paths=[source,chip/'esp32p4_jpeg_internal.h',chip/'esp32p4_jpeg_tables.h',fixture,Path(__file__).resolve(),fixtures/'FIXTURES.json']
(out/'RESULT.json').write_text(json.dumps({'passed':True,'hardware_tested':False,
 'scope':'Actual native parser; real Pillow/official JPEG fixtures, truncation, malicious markers, deterministic mutation/random input; no hardware decode',
 'fixture_cases':len(manifest['cases']),'sanitizers':['address','undefined'],
 'target_object_sha256':hashlib.sha256(obj.read_bytes()).hexdigest(),
 'source_sha256':{str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in paths}},indent=2)+'\n')
print('P4_JPEG_PARSE_TARGET_COMPILE_OK')
