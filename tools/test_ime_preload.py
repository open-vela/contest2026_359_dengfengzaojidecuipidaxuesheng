#!/usr/bin/env python3
"""Exercise real Pinyin preparation with delayed storage and concurrent UI calls."""
from pathlib import Path
import argparse,subprocess
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
repo=Path(__file__).resolve().parents[1]
d=repo/'app/espdl-quickapp/overlay/apps/system/desktop'
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
(out/'main.cpp').write_text(r'''
#include "glass_pinyin.h"
#include "glass_material.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>
#include <malloc.h>
using namespace std::chrono;
static std::atomic<unsigned> admissions{0};
static unsigned delay_ms=400;
extern "C" int glass_local_voice_yield_idle_model(unsigned) {
 admissions++;return 0;
}
extern "C" int __real_gmat_verify(unsigned);
extern "C" int __wrap_gmat_verify(unsigned id) {
 /* Slow storage while the production loader owns decoder_lock. */
 std::this_thread::sleep_for(milliseconds(delay_ms));return __real_gmat_verify(id);
}
static long now(){return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();}
int main(int argc,char **){
 bool baseline=argc>1;gmat_boot_enable(true);auto initial=mallinfo2();auto before=initial.uordblks+initial.hblkhd;
 long start=now();bool ready=glass_pinyin_init();long first=now()-start;
 if(baseline){assert(ready&&first>=350);}else{
  assert(!ready&&first<50);
  for(unsigned i=0;i<20;i++){
   long tick=now();char text[100];unsigned used;
   assert(glass_pinyin_search("nihao")==0);
   assert(!glass_pinyin_candidate(0,text,sizeof(text)));
   assert(!glass_pinyin_choose(0,text,sizeof(text),&used));
   glass_pinyin_reset();glass_pinyin_flush();
   assert(now()-tick<50);std::this_thread::sleep_for(milliseconds(5));
  }
  start=now();while(!glass_pinyin_init()){assert(now()-start<5000);std::this_thread::sleep_for(milliseconds(5));}
 }
 assert(admissions==1);
 assert(glass_pinyin_search("nihao")>0);char text[100];assert(glass_pinyin_candidate(0,text,sizeof(text)));assert(!strcmp(text,"你好"));
 auto current=mallinfo2();auto resident=current.uordblks+current.hblkhd-before;assert(resident<2u*1024u*1024u);
 assert(!glass_pinyin_reclaim_idle());glass_pinyin_reset();assert(glass_pinyin_reclaim_idle());
 struct gmat_stats stats;gmat_status(&stats);assert(stats.cache_bytes<=65536&&stats.blob_bytes==0);
 delay_ms=0;start=now();while(!glass_pinyin_init()){assert(now()-start<5000);std::this_thread::sleep_for(milliseconds(5));}
 assert(glass_pinyin_search("zhongguo")>0);glass_pinyin_close();gmat_trim();
 printf("PINYIN_OK first_call_ms=%ld resident_bytes=%zu material_peak=%zu admissions=%u\n",first,resident,stats.peak_bytes,admissions.load());
}
''')
subprocess.run(['gcc','-std=gnu11','-O1','-g','-pthread','-I'+str(d),'-c',str(d/'glass_material.c'),'-o',str(out/'material.o')],check=True)
vendor=d/'music_vendor/googlepinyin/src'
sources=[str(f) for f in vendor.glob('*.cpp') if f.name not in ('dictbuilder.cpp',)]
cmd=['g++','-std=c++17','-O1','-g','-pthread','-Wl,--wrap=gmat_verify','-I'+str(d),str(out/'main.cpp'),str(d/'glass_pinyin.cpp'),*sources,str(out/'material.o'),'-o',str(out/'test')]
subprocess.run(cmd,check=True)
subprocess.run([str(out/'test')],check=True,timeout=15)
