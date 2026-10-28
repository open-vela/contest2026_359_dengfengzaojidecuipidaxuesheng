#!/usr/bin/env python3
"""Actual bridge, real LVGL structs and guarded fake DMA transport under sanitizers."""
import argparse,json,hashlib,subprocess
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--apps',type=Path,required=True)
p.add_argument('--cache',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
repo=Path(__file__).resolve().parents[1]
desktop=repo/'app/espdl-quickapp/overlay/apps/system/desktop'
for name,text in {'nuttx/config.h':'#pragma once\n',
 'nuttx/compiler.h':'#pragma once\n#define FAR\n',
 'nuttx/fs/ioctl.h':'#pragma once\n#define _IOC(a,b) ((a)|(b))\n',
 'sys/ioctl.h':'#pragma once\nint blit_test_ioctl(int,unsigned long,...);\n'}.items():
    f=out/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_text(text)
fixture=r'''
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nuttx/video/p4_ppa.h>
#include <lvgl/lvgl.h>
#include <lvgl/src/draw/lv_draw_private.h>
#include <lvgl/src/draw/lv_image_decoder_private.h>
#include "glass_video_blit.h"
static unsigned calls,opens,closes;
static int fail_open,fail_io;
static int64_t ticks=100;
static void emit(const char *format,...)
{va_list ap;va_start(ap,format);vprintf(format,ap);va_end(ap);}
int64_t esp_timer_get_time(void){return ticks+=10;}
int blit_test_open(const char *name,int flags,...)
{assert(!strcmp(name,"/dev/ppa0")&&flags==O_RDWR);opens++;if(fail_open){errno=EIO;return -1;}return 17;}
int blit_test_close(int fd){assert(fd==17);closes++;return 0;}
int blit_test_ioctl(int fd,unsigned long cmd,...)
{
  assert(fd==17&&cmd==P4_PPA_SRM_RGB565);calls++;
  va_list ap;va_start(ap,cmd);struct p4_ppa_job_s *j=(void *)va_arg(ap,unsigned long);va_end(ap);
  assert(j->width==1024&&j->out_width==1024&&j->out_height==600&&j->scale256==256);
  assert(!j->out_x&&j->out_y+j->height<=600);
  assert(!((uintptr_t)j->input&127)&&!((uintptr_t)j->output&127));
  assert(j->input_size==(size_t)j->height*2048&&j->output_size==600*2048);
  if(fail_io){errno=EIO;return -1;}
  memcpy((char *)j->output+(size_t)j->out_y*2048,j->input,j->input_size);return 0;
}
static unsigned char *allocate(unsigned char **base)
{
  assert(!posix_memalign((void **)base,128,600*2048+256));
  memset(*base,0xa5,600*2048+256);return *base+128;
}
static void guards(unsigned char *base)
{for(unsigned i=0;i<128;i++)assert(base[i]==0xa5&&base[128+600*2048+i]==0xa5);}
int main(void)
{
  unsigned char *ab,*bb,*a=allocate(&ab),*b=allocate(&bb);
  for(unsigned i=0;i<600*2048;i++)a[i]=(unsigned char)(i*7+i/2048);
  lv_draw_buf_t src={.data=a,.data_size=600*2048},dst={.data=b,.data_size=600*2048};
  src.header.w=dst.header.w=1024;src.header.h=dst.header.h=600;
  src.header.stride=dst.header.stride=2048;src.header.cf=dst.header.cf=LV_COLOR_FORMAT_RGB565;
  lv_area_t full={0,0,1023,599};
  lv_layer_t layer={.draw_buf=&dst,.buf_area=full,.color_format=LV_COLOR_FORMAT_RGB565};
  lv_draw_unit_t unit={.target_layer=&layer};
  lv_image_dsc_t image={0};image.data=a;image.data_size=600*2048;
  lv_draw_image_dsc_t dsc={.src=&image,.scale_x=256,.scale_y=256,.opa=255};
  lv_image_decoder_dsc_t decoder={.decoded=&src};
  glass_video_blit_bind(&image);glass_video_profile_reset();
  unit.clip_area=&full;
  lv_draw_fill_dsc_t fill={0};fill.opa=255;
  memset(b,0x55,600*2048);
  assert(glass_video_blit_fill(&unit,&fill,&full)==1);
  for(unsigned i=0;i<600*2048;i++)assert(b[i]==0);
  for(unsigned i=0;i<600*2048;i++)assert(a[i]==(unsigned char)(i*7+i/2048));
  guards(ab);guards(bb);
  fill.opa=128;assert(!glass_video_blit_fill(&unit,&fill,&full));fill.opa=255;
  fill.radius=1;assert(!glass_video_blit_fill(&unit,&fill,&full));fill.radius=0;
  fill.grad.dir=LV_GRAD_DIR_HOR;assert(!glass_video_blit_fill(&unit,&fill,&full));fill.grad.dir=LV_GRAD_DIR_NONE;
  fill.color.red=1;assert(!glass_video_blit_fill(&unit,&fill,&full));fill.color.red=0;
  lv_area_t small_clip=full;small_clip.x2--;
  unit.clip_area=&small_clip;assert(!glass_video_blit_fill(&unit,&fill,&full));unit.clip_area=&full;
  layer.draw_buf=&src;assert(!glass_video_blit_fill(&unit,&fill,&full));layer.draw_buf=&dst;
  glass_video_blit_set_enabled(false);assert(!glass_video_blit_fill(&unit,&fill,&full));glass_video_blit_set_enabled(true);
  assert(glass_video_blit_image(&unit,&dsc,&decoder,&full,&full)==1);
  assert(calls==1&&!memcmp(a,b,600*2048));guards(ab);guards(bb);
  lv_image_dsc_t wrapped={0};dsc.src=&wrapped;
  assert(glass_video_blit_image(&unit,&dsc,&decoder,&full,&full)==1);
  dsc.src=&image;
  unsigned n=calls;
  lv_area_t partial=full;partial.x1=1;
  assert(!glass_video_blit_image(&unit,&dsc,&decoder,&full,&partial));
  dsc.opa=128;assert(!glass_video_blit_image(&unit,&dsc,&decoder,&full,&full));dsc.opa=255;
  dsc.rotation=1;assert(!glass_video_blit_image(&unit,&dsc,&decoder,&full,&full));dsc.rotation=0;
  dsc.scale_x=257;assert(!glass_video_blit_image(&unit,&dsc,&decoder,&full,&full));dsc.scale_x=256;
  dsc.clip_radius=8;assert(!glass_video_blit_image(&unit,&dsc,&decoder,&full,&full));dsc.clip_radius=0;
  dst.header.cf=LV_COLOR_FORMAT_ARGB8888;assert(!glass_video_blit_image(&unit,&dsc,&decoder,&full,&full));dst.header.cf=LV_COLOR_FORMAT_RGB565;
  assert(calls==n);
  lv_area_t sa={0,10,1023,89},da={0,100,1023,179};
  memset(b,0x55,600*2048);
  assert(glass_video_blit_copy(&dst,&da,&src,&sa)==1);
  assert(!memcmp(b+100*2048,a+10*2048,80*2048));
  for(unsigned i=0;i<100*2048;i++)assert(b[i]==0x55);
  for(unsigned i=180*2048;i<600*2048;i++)assert(b[i]==0x55);
  assert(!glass_video_blit_copy(&src,NULL,&src,NULL));
  da.y1=-1;assert(!glass_video_blit_copy(&dst,&da,&src,&sa));da.y1=100;
  da.x1=1;assert(!glass_video_blit_copy(&dst,&da,&src,&sa));da.x1=0;
  dst.data=b+2;assert(!glass_video_blit_copy(&dst,NULL,&src,NULL));dst.data=b;
  glass_video_blit_set_enabled(false);n=calls;
  assert(!glass_video_blit_image(&unit,&dsc,&decoder,&full,&full)&&calls==n);
  glass_video_blit_set_enabled(true);
  fail_io=1;n=calls;
  assert(!glass_video_blit_image(&unit,&dsc,&decoder,&full,&full)&&calls==n+1);
  assert(!glass_video_blit_image(&unit,&dsc,&decoder,&full,&full)&&calls==n+1);
  /* Caller takes the original software path after any failed DMA. */
  memcpy(b,a,600*2048);assert(!memcmp(a,b,600*2048));guards(ab);guards(bb);
  glass_video_blit_release();assert(closes==1);
  fail_io=0;fail_open=1;glass_video_blit_bind(&image);n=calls;
  assert(!glass_video_blit_image(&unit,&dsc,&decoder,&full,&full)&&calls==n);
  glass_video_blit_release();fail_open=0;glass_video_blit_bind(&image);
  assert(glass_video_blit_image(&unit,&dsc,&decoder,&full,&full));
  glass_video_blit_bind(NULL);n=calls;
  assert(!glass_video_blit_copy(&dst,NULL,&src,NULL)&&calls==n);
  glass_video_blit_release();assert(closes==2&&opens==3);
  glass_video_profile_report(emit);free(ab);free(bb);
  puts("VIDEO_BLIT_OK: actual bridge/real LVGL ABI, opaque full image, guarded strip copy, clip/style/format rejection, alias/alignment bounds, disable, open/DMA failure and release");
}
'''
(out/'test.c').write_text(fixture)
flags=['-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
 '-fno-pie','-no-pie','-DGLASS_VIDEO_BLIT','-DLV_CONF_INCLUDE_SIMPLE',
 '-Dopen=blit_test_open','-Dclose=blit_test_close','-Dioctl=blit_test_ioctl',
 '-I'+str(out),'-I'+str(desktop),'-I'+str(a.apps.resolve()/'graphics/lvgl'),
 '-I'+str(a.cache.resolve()),'-I'+str(repo/'app/espdl-quickapp/overlay/nuttx/include')]
subprocess.run(['gcc',*flags,str(out/'test.c'),str(desktop/'glass_video_blit.c'),'-o',str(out/'test')],check=True)
subprocess.run([str(out/'test')],check=True)
(out/'RESULT.json').write_text(json.dumps(dict(passed=True,hardware_tested=False,
 scope='actual bridge, real LVGL headers, fake PPA transport under ASan/UBSan',
 source_sha256={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in [desktop/'glass_video_blit.c',desktop/'glass_video_blit.h',Path(__file__)]}),indent=2)+'\n')
