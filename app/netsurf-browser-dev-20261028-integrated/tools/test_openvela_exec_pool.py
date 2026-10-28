#!/usr/bin/env python3
"""Test the production allocator and prepared SDK's actual PMP pool block."""
import argparse
from pathlib import Path
import subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--target',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();root=Path(__file__).resolve().parents[1]
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
subprocess.run(['gcc','-std=gnu11','-D_GNU_SOURCE','-fno-pie','-no-pie','-Wall','-Wextra','-Werror','-pthread',
    str(root/'platform/openvela_module/browser_text_heap.c'),str(root/'tests/openvela/text_heap.c'),
    '-o',str(out/'text_heap')],check=True)
subprocess.run([str(out/'text_heap')],check=True)
sdk=a.target/'nuttx/arch/risc-v/src/esp32p4/esp-hal-3rdparty'
source=(sdk/'components/esp_hw_support/port/esp32p4/cpu_region_protect.c').read_text()
start=source.index('#ifdef CONFIG_ESP32P4_PSRAM_XIP\n    /* ESPCLAW XIP PMP HEAP END')
end=source.index('#else\n    PMP_ENTRY_SET_CACHED_AND_UNCACHED(13, 18, (uint32_t)(&_rodata_reserved_end)',start)
block=source[start:end]+'#endif\n'
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define SOC_CPU_PMP_REGION_GRANULARITY 128
#define PMP_TOR 8u
#define R 129u
#define RW 131u
#define RWX 135u
#define CACHE_LL_L2MEM_NON_CACHE_ADDR(a) ((a)+0x10000000u)
#define PMP_RESET_AND_ENTRY_SET(i,a,p) do{addr[i]=(a);perm[i]=(p);}while(0)
#define PMP_ENTRY_SET_CACHED_AND_UNCACHED(i,j,a,p) do{PMP_RESET_AND_ENTRY_SET(i,a,p);PMP_RESET_AND_ENTRY_SET(j,CACHE_LL_L2MEM_NON_CACHE_ADDR(a),p);}while(0)
__asm__(".global _rodata_reserved_end\n.set _rodata_reserved_end,0x48a00020\n"
        ".global _browser_exec_end\n.set _browser_exec_end,0x48d10000\n");
extern int _rodata_reserved_end;
static uintptr_t addr[32];static unsigned perm[32];
static uintptr_t esp_psram_extram_vaddr_end(void){return 0x4a000000;}
static unsigned access_at(uintptr_t p){
 for(unsigned i=0;i<32;i++)if((perm[i]&PMP_TOR)&&p>=addr[i-1]&&p<addr[i])return perm[i]&7;
 return 0;
}
int main(void){
 size_t available_psram_heap=0;
 PMP_ENTRY_SET_CACHED_AND_UNCACHED(10,15,0x48000000,128);
 PMP_ENTRY_SET_CACHED_AND_UNCACHED(11,16,0x48400000,141);
 PMP_ENTRY_SET_CACHED_AND_UNCACHED(12,17,0x48400000,139);
 addr[20]=0x40000000;addr[21]=0x48400000;perm[21]=141;
 addr[22]=0x48a10000;perm[22]=137;
'''+block+r'''
 fprintf(stderr,"PMP model code=%u ro=%u pool=%u heap=%u uncached=%u end14=%lx end23=%lx\n",
 access_at(0x48000100),access_at(0x48700000),access_at(0x48a10000),access_at(0x49ffffff),access_at(0x58a10000),
 (unsigned long)addr[14],(unsigned long)addr[23]);
 fprintf(stderr,"PMP model end13=%lx end18=%lx ro_symbol=%lx\n",(unsigned long)addr[13],(unsigned long)addr[18],(unsigned long)&_rodata_reserved_end);
 assert(access_at(0x48000100)==5); /* Existing code RX. */
 assert(access_at(0x48700000)==1); /* Existing constants R. */
#ifdef CONFIG_OV_BROWSER_TEXT_POOL
 assert(access_at(0x48a10000)==7&&access_at(0x48d0ffff)==7);
 assert(access_at(0x48d10000)==3&&access_at(0x49ffffff)==3);
#else
 assert(access_at(0x48a10000)==3&&access_at(0x49ffffff)==3);
#endif
 assert(access_at(0x58a10000)==3); /* Uncached pool is never executable. */
 assert(access_at(0x4a000000)==0);
 puts("PASS: actual SDK PMP block preserves code/rodata, bounds native execution and keeps ordinary/uncached heap NX");
}
'''
(out/'pmp.c').write_text(code)
for enabled in [False,True]:
    binary=out/('pmp-pool' if enabled else 'pmp-original')
    subprocess.run(['gcc','-std=gnu11','-fno-pie','-no-pie','-Wall','-Wextra','-Werror','-Wno-pointer-to-int-cast',
        '-DCONFIG_ESP32P4_PSRAM_XIP=1',*(['-DCONFIG_OV_BROWSER_TEXT_POOL=1'] if enabled else []),
        str(out/'pmp.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
