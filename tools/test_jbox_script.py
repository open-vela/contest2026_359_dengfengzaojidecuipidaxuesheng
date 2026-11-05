#!/usr/bin/env python3
"""Native compiler vs reference Python compiler and actual S3 VM validator."""
import argparse,ctypes as C,json,os,random,shutil,subprocess,sys
from pathlib import Path
root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'app/ProgrammableMatrix'))
from client.jbox_lang import compile_function
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args()
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
desktop=root/'app/espdl-quickapp/overlay/apps/system/desktop'
remote=root/'app/ProgrammableMatrix/main'
cc=shutil.which('gcc') or 'gcc'
ext='.dll' if os.name=='nt' else '.so'
flags=['-std=gnu11','-Wall','-Wextra','-Werror','-O1','-g','-shared']
if os.name!='nt':flags+=['-fPIC']
subprocess.run([cc,*flags,str(desktop/'jbox_script.c'),'-o',str(out/('compiler'+ext))],check=True)
subprocess.run([cc,*flags,'-I'+str(remote),str(remote/'script_vm.c'),'-o',str(out/('vm'+ext))],check=True)
class Program(C.Structure):
    _fields_=[('code',C.c_ubyte*4096),('count',C.c_uint),('arguments',C.c_uint),
              ('name',C.c_char*64),('parameters',(C.c_char*64)*4),('error_offset',C.c_size_t)]
class Remote(C.Structure):
    _fields_=[('argc',C.c_ubyte),('count',C.c_uint16),('code',C.c_ubyte*4096)]
lib=C.CDLL(str(out/('compiler'+ext)));vm=C.CDLL(str(out/('vm'+ext)))
lib.jbox_script_compile.argtypes=[C.c_char_p,C.POINTER(Program)]
lib.jbox_script_example.argtypes=[C.c_char_p];lib.jbox_script_example.restype=C.c_char_p
vm.script_validate.argtypes=[C.POINTER(Remote)]
def native(source):
    compiled=Program();rc=lib.jbox_script_compile(source.encode(),C.byref(compiled))
    return rc,compiled
dht=(root/'app/ProgrammableMatrix/client/programs/dht11.jbox').read_text(encoding='utf-8')
assert lib.jbox_script_example(b'dht11').decode()==dht
valid=[dht,
 'fn blink(led){output(led);repeat(2){high(led);sleep_ms(100);low(led);sleep_ms(100);}emit(1);}',
 'fn read(p){input(p);let a=sample(p);let b=measure(p,high,100);wait(p,low,100);emit(a+b);}',
 'fn t(p){open_drain(p);timed{low(p);delay_us(100);release(p);}emit(1);}',
 '# test\nfn x(a,b,c,d){let x=1;repeat(3){x=(x+2)*3;}assert(x>1);emit(x>>2);}',
 'fn c(p){open_drain(p);capture(p,1000);emit(pulse_count());emit(pulse_us(0));emit(pulse_level(0));}']
rng=random.Random(43)
for _ in range(150):
    operators=['+','-','*','<<','>>','&','|','^','>','==']
    expr=str(rng.randrange(32))
    for i in range(rng.randrange(1,8)):
        expr='('+expr+' '+rng.choice(operators)+' '+str(rng.randrange(16))+')'
    valid.append('fn arithmetic(){emit('+expr+');}')
for source in valid:
    reference=compile_function(source);rc,compiled=native(source)
    assert rc==0,(source[:100],rc,compiled.error_offset)
    assert bytes(compiled.code)[:compiled.count*4]==reference.code
    assert compiled.arguments==len(reference.parameters)
    assert compiled.name.decode()==reference.name
    assert tuple(x.value.decode() for x in compiled.parameters[:compiled.arguments])==reference.parameters
    remote_program=Remote();remote_program.argc=compiled.arguments;remote_program.count=compiled.count
    remote_program.code[:]=compiled.code[:]
    assert vm.script_validate(C.byref(remote_program))==0
invalid=['','fn empty(){}','fn x(a,a){emit(1);}','fn x(a,b,c,d,e){emit(1);}',
 'fn x(){emit(65536);}','fn x(){emit(-1);}','fn x(){emit(x);}',
 'fn x(){let a=a+1;}','fn x(){repeat(129){emit(1);}}',
 'fn x(){repeat(128){emit(1);emit(1);}}','fn x(){repeat(128){repeat(128){emit(1);}}}',
 'fn x(){timed{timed{emit(1);}}}','fn x(p){timed{input(p);}}',
 'fn x(){sleep_ms(251);}','fn x(){sleep_ms(250);sleep_ms(250);}',
 'fn x(p){delay_us(0);}','fn x(p){wait(p,high,10001);}',
 'fn x(p){timed{capture(p,1000);}}','fn x(){emit(pulse_count());}',
 'fn x(p){input(other);}','fn x(){emit(1);} extra',
 'fn x(){emit('+('('*100)+'1'+(')'*100)+');}',
 'fn '+('a'*64)+'(){emit(1);}',
 'fn x(){'+''.join('let x%d=0;'%i for i in range(17))+'}',
 'x'*32769,'fn x(){emit(1/2);}']
for source in invalid:
    rc,compiled=native(source);assert rc<0,source[:100]
    assert compiled.count==compiled.arguments==0 and not any(compiled.code)
for _ in range(2000):
    source=''.join(rng.choice('fnxyz0129{}();,+-*<>_\n') for i in range(rng.randrange(200)))
    rc,compiled=native(source)
    if rc:assert not compiled.count and not any(compiled.code)
    else:
        remote_program=Remote();remote_program.argc=compiled.arguments;remote_program.count=compiled.count
        remote_program.code[:]=compiled.code[:];assert vm.script_validate(C.byref(remote_program))==0
sanitizers=False
if os.name!='nt':
    harness=out/'fuzz.c'
    harness.write_text('''#include "jbox_script.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
int main(void){struct jbox_script_program *p=malloc(sizeof(*p));char *s=malloc(32770);assert(p&&s);
unsigned state=1;for(unsigned k=0;k<10000;k++){unsigned n=k%32769;for(unsigned i=0;i<n;i++){
state=state*1664525u+1013904223u;s[i]="fnax09{}();+-# \\n"[state%16];}s[n]=0;jbox_script_compile(s,p);}
memset(s,'(',32769);s[32769]=0;assert(jbox_script_compile(s,p)<0);
assert(jbox_script_compile(jbox_script_example("dht11"),p)==0);free(s);free(p);return 0;}
''')
    subprocess.run([cc,'-std=gnu11','-O1','-g','-fsanitize=address,undefined','-fno-pie','-no-pie',
                    '-I'+str(desktop),str(harness),str(desktop/'jbox_script.c'),'-o',str(out/'fuzz')],check=True)
    subprocess.run([str(out/'fuzz')],check=True,timeout=60,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1',UBSAN_OPTIONS='halt_on_error=1'))
    sanitizers=True
result=dict(passed=True,oracle_cases=len(valid),rejections=len(invalid),fuzz_cases=2000,
            s3_validator=True,sanitizers=sanitizers,real_sensor_tested=False,dht11_instructions=len(compile_function(dht).code)//4)
(out/'RESULT.json').write_text(json.dumps(result,indent=2));print(json.dumps(result))
