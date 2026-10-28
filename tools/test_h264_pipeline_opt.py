#!/usr/bin/env python3
"""Differential I420 packing and bounded late-frame scheduling under sanitizers."""
import argparse,json,subprocess,os
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True)
a=p.parse_args();repo=Path(__file__).resolve().parents[1];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
fixture=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "p4_yuv_pack.h"
#include "bili_late_pacer.h"
static unsigned rng=1;
static unsigned random_byte(void){rng=rng*1664525+1013904223;return rng>>24;}
static unsigned fast,declined;
static void trial(unsigned w,unsigned h,unsigned pad,unsigned offset){
 int strides[2]={(int)(w+pad),(int)((w+1)/2+pad)};
 size_t sizes[3]={(size_t)strides[0]*h,(size_t)strides[1]*((h+1)/2),(size_t)strides[1]*((h+1)/2)};
 uint8_t *base[3],*saved[3];const uint8_t *planes[3];
 for(unsigned k=0;k<3;k++){
   assert(!posix_memalign((void **)&base[k],64,sizes[k]+128));
   for(size_t n=0;n<sizes[k]+128;n++)base[k][n]=random_byte();
   saved[k]=malloc(sizes[k]+128);assert(saved[k]);memcpy(saved[k],base[k],sizes[k]+128);
   planes[k]=base[k]+64+offset;
 }
 size_t bytes=(size_t)w*h*2;uint8_t *dest,*reference=malloc(bytes);assert(reference);
 assert(!posix_memalign((void **)&dest,64,bytes+128));memset(dest,0xa5,bytes+128);
 for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x+=2){
   size_t n=((size_t)y*w+x)*2;
   reference[n]=planes[1][(y/2)*strides[1]+x/2];
   reference[n+1]=planes[0][y*strides[0]+x];
   reference[n+2]=planes[2][(y/2)*strides[1]+x/2];
   reference[n+3]=planes[0][y*strides[0]+x+1];
 }
 bool done=bili_p4_pack_i420(dest+64,planes,strides,w,h);
 if(done){fast++;assert(!memcmp(dest+64,reference,bytes));}
 else{declined++;for(size_t n=0;n<bytes;n++)assert(dest[64+n]==0xa5);}
 for(unsigned n=0;n<64;n++)assert(dest[n]==0xa5&&dest[64+bytes+n]==0xa5);
 for(unsigned k=0;k<3;k++){assert(!memcmp(base[k],saved[k],sizes[k]+128));free(base[k]);free(saved[k]);}
 free(dest);free(reference);
}
int main(void){
 unsigned widths[]={2,4,8,16,20,320,640,1280},heights[]={2,3,4,6,18,180,360,720};
 for(unsigned w=0;w<8;w++)for(unsigned h=0;h<8;h++)for(unsigned p=0;p<3;p++)for(unsigned off=0;off<4;off++)
   trial(widths[w],heights[h],p==0?0:p==1?4:1,off);
 for(unsigned n=0;n<1000;n++)trial(2+2*(random_byte()%32),2+random_byte()%64,random_byte()%8,random_byte()%4);
 struct bili_late_pacer p={0};
 assert(!bili_late_drop(&p,1000,true,100));assert(bili_late_drop(&p,1080,true,100));
 assert(!bili_late_drop(&p,1160,true,100));assert(!bili_late_drop(&p,1240,true,100));
 assert(!bili_late_drop(&p,9000,true,100));assert(bili_late_drop(&p,9001,true,100));
 assert(!bili_late_drop(&p,9001,true,50));
 for(unsigned n=0;n<100;n++)assert(!bili_late_drop(&p,10000+n*41,false,100));
 p=(struct bili_late_pacer){0};uint64_t now=1000;unsigned accepted=0;
 while(now<21000){now+=58;if(!bili_late_drop(&p,now,true,100)){accepted++;now+=24;}}
 assert(accepted>=198&&accepted<=202);
 printf("H264_PIPELINE_OPT_OK fast=%u declined=%u paced_frames=%u/20s, byte equality and immutable sources/guards, odd/alignment fallback, no backlog burst\n",fast,declined,accepted);
}
'''
(out/'test.c').write_text(fixture)
cmd=['gcc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(repo/'app/bilibili_live'),str(out/'test.c'),'-o',str(out/'test')]
subprocess.run(cmd,check=True)
r=subprocess.run([str(out/'test')],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'},check=True)
print(r.stdout,end='');(out/'run.log').write_text(r.stdout+r.stderr)
(out/'RESULT.json').write_text(json.dumps(dict(passed=True,hardware_tested=False,scope='actual aligned word pack versus independent scalar UYVY; ASan/UBSan; guarded scheduling deadlines'),indent=2)+'\n')
