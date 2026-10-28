#!/usr/bin/env python3
"""Exercise actual PPA fit and player layout bodies with fake synchronous I/O."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
from render_all_ui import function

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
repo = Path(__file__).resolve().parents[1]
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
source = repo/'app/bilibili_live/local_video_ui.c'
ppa = repo/'app/bilibili_live/bili_ppa.c'
for name, data in {
    'nuttx/config.h': '#pragma once\n',
    'nuttx/compiler.h': '#pragma once\n#define FAR\n',
    'nuttx/fs/ioctl.h': '#pragma once\n#define _IOC(a,b) ((a)|(b))\n',
    'sys/ioctl.h': '#pragma once\nint ioctl(int,unsigned long,...);\n',
}.items():
    target = out/name
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(data)
prelude = r'''
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nuttx/video/p4_ppa.h>
#include "bili_ppa.h"
#define W 640
#define H 360
#define LV_SCALE_NONE 256
#define LV_IMAGE_HEADER_MAGIC 0x19
#define LV_COLOR_FORMAT_RGB565 1
typedef struct {struct {unsigned magic,cf,w,h,stride;} header;
 size_t data_size;const uint8_t *data;} lv_image_dsc_t;
typedef struct lv_obj {unsigned w,h,scale;const void *src;struct lv_obj *parent;} lv_obj_t;
struct play_session {struct bili_ppa display_ppa;uint16_t *full_pixels;
 unsigned full_width,full_height;};
static struct play_session *session;
static lv_image_dsc_t picture,full_picture;
static lv_obj_t *image,*fullscreen;
static bool fullscreen_ppa_enabled=true,fail_dma,fail_alloc;
static unsigned io_calls,opens,closes,last_scale,allocations;
void *upscale_test_memalign(size_t alignment,size_t size)
{void *p=NULL;if(fail_alloc)return NULL;assert(!posix_memalign(&p,alignment,size));allocations++;return p;}
int upscale_test_open(const char *path,int flags,...)
{(void)flags;assert(!strcmp(path,"/dev/ppa0"));opens++;return 17;}
int upscale_test_close(int fd){assert(fd==17);closes++;return 0;}
int upscale_test_ioctl(int fd,unsigned long request,...)
{
 assert(fd==17&&request==P4_PPA_SRM_RGB565);io_calls++;
 va_list ap;va_start(ap,request);struct p4_ppa_job_s *j=(void *)va_arg(ap,unsigned long);va_end(ap);
 assert(j&&j->input!=j->output&&j->scale256>=16&&j->scale256<=1024&&!(j->scale256&15));
 assert(!((uintptr_t)j->input&127)&&!((uintptr_t)j->output&127));
 last_scale=j->scale256;
 if(fail_dma){errno=EIO;return -1;}
 unsigned w=j->width*j->scale256/256,h=j->height*j->scale256/256;
 assert(w+j->out_x<=j->out_width&&h+j->out_y<=j->out_height);
 const uint16_t *src=j->input;uint16_t *dst=j->output;
 /* Transport substitute tests bounds/placement; it does not model physical
  * PPA interpolation, DMA ownership or cache behaviour. */
 for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++)
  dst[(y+j->out_y)*j->out_width+x+j->out_x]=src[(y*256/j->scale256)*j->width+x*256/j->scale256];
 return 0;
}
static void lv_image_cache_drop(const void *p){(void)p;}
static lv_obj_t *lv_obj_get_parent(lv_obj_t *p){return p->parent;}
static unsigned lv_obj_get_width(lv_obj_t *p){return p->w;}
static unsigned lv_obj_get_height(lv_obj_t *p){return p->h;}
static void lv_image_set_src(lv_obj_t *p,const void *s){p->src=s;}
static void lv_obj_set_size(lv_obj_t *p,unsigned w,unsigned h){p->w=w;p->h=h;}
static void lv_image_set_scale(lv_obj_t *p,unsigned s){p->scale=s;}
static void lv_image_set_pivot(lv_obj_t *p,unsigned x,unsigned y){(void)p;(void)x;(void)y;}
static void lv_image_set_antialias(lv_obj_t *p,bool v){(void)p;assert(!v);}
static void lv_obj_center(lv_obj_t *p){(void)p;}
static void lv_obj_invalidate(lv_obj_t *p){(void)p;}
'''
body = '\n'.join(function(source.read_text(), name) for name in
                 ['local_video_scaled_picture', 'picture_layout', 'local_video_set_upscale'])
main = r'''
int main(void)
{
 struct play_session s={0};session=&s;
 lv_obj_t screen={.w=1024,.h=600},obj={.parent=&screen};image=&obj;
 uint16_t *pixels=upscale_test_memalign(128,640*360*2+128);
 for(unsigned i=0;i<640*360;i++)pixels[i]=0xf800;
 memset(pixels+640*360,0xa5,128);
 picture.header.w=320;picture.header.h=180;picture.header.stride=640;
 picture.data=(const uint8_t *)pixels;picture.data_size=320*180*2;
 picture_layout();assert(!io_calls&&obj.src==&picture&&obj.w==320&&obj.h==180&&obj.scale==256);
 fullscreen=&screen;picture_layout();
 assert(io_calls==1&&last_scale==816&&obj.src==&full_picture&&obj.scale==256&&obj.w==1024&&obj.h==600);
 uint16_t *saved=s.full_pixels;unsigned saved_allocations=allocations;
 for(unsigned y=0;y<600;y++)for(unsigned x=0;x<1024;x++)
  assert(saved[y*1024+x]==(x>=2&&x<1022&&y>=13&&y<586?0xf800:0));
 /* The paused/final frame can switch modes without any decoder callback. */
 local_video_set_upscale(false);assert(io_calls==1&&obj.src==&picture&&obj.scale==819);
 local_video_set_upscale(true);assert(io_calls==2&&s.full_pixels==saved&&allocations==saved_allocations&&obj.scale==256);
 fullscreen=NULL;picture_layout();assert(io_calls==2&&obj.src==&picture&&obj.scale==256);
 for(unsigned i=0;i<640*360;i++)pixels[i]=0x07e0;
 picture.header.w=640;picture.header.h=360;picture.header.stride=1280;picture.data_size=640*360*2;
 fullscreen=&screen;picture_layout();assert(last_scale==400&&obj.src==&full_picture&&obj.scale==256);
 for(unsigned y=0;y<600;y++)for(unsigned x=0;x<1024;x++)
  assert(saved[y*1024+x]==(x>=12&&x<1012&&y>=19&&y<581?0x07e0:0));
 fail_dma=true;picture_layout();assert(s.display_ppa.failed&&obj.src==&picture&&obj.scale==409);
 unsigned before=io_calls;picture_layout();assert(io_calls==before&&obj.src==&picture);
 struct bili_ppa clean={0};
 assert(bili_ppa_rgb565_fit(&clean,pixels,pixels,320,180,1024,600)==-EINVAL);
 assert(bili_ppa_rgb565_fit(&clean,saved,pixels+1,320,180,1024,600)==-EINVAL);
 assert(bili_ppa_rgb565_fit(&clean,saved,pixels,0,180,1024,600)==-EINVAL);
 assert(io_calls==before);
 bili_ppa_close(&s.display_ppa);free(saved);memset(&s,0,sizeof(s));
 fail_dma=false;fail_alloc=true;picture_layout();assert(s.display_ppa.failed&&!s.full_pixels&&obj.src==&picture);
 for(unsigned i=0;i<128;i++)assert(((uint8_t *)(pixels+640*360))[i]==0xa5);
 assert(opens==closes);free(pixels);
 puts("VIDEO_UPSCALE_OK: actual RGB565 fit/player layout, 3.1875x/1.5625x bounds/letterbox, native unchanged, 1:1 full-screen, paused/final toggles, buffer reuse, DMA/allocation fallback, no alias writes or input corruption");
 return 0;
}
'''
test = out/'upscale.c'
test.write_text(prelude+'\n'+body+'\n'+main)
cmd = ['gcc', '-std=gnu11', '-D_GNU_SOURCE', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
       '-Wno-misleading-indentation', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
       '-fno-pie', '-no-pie', '-Dopen=upscale_test_open', '-Dclose=upscale_test_close',
       '-Dioctl=upscale_test_ioctl', '-Dmemalign=upscale_test_memalign', '-I'+str(out),
       '-I'+str(repo/'app/bilibili_live'),
       '-I'+str(repo/'app/espdl-quickapp/overlay/nuttx/include'), str(ppa), str(test),
       '-o', str(out/'upscale-host')]
for name, argv in [('compile', cmd), ('run', [str(out/'upscale-host')])]:
    result = subprocess.run(argv, capture_output=True, text=True, timeout=60,
                            env={**os.environ, 'ASAN_OPTIONS':'detect_leaks=1:halt_on_error=1',
                                 'UBSAN_OPTIONS':'halt_on_error=1'})
    (out/(name+'.log')).write_text(result.stdout+result.stderr)
    if result.returncode:
        raise RuntimeError(name+' failed:\n'+result.stdout+result.stderr)
    print(result.stdout, end='')
(out/'RESULT.json').write_text(json.dumps(dict(passed=True, hardware_tested=False,
    scope='actual player layout and RGB565 fit; fake LVGL setters and DMA transport',
    source_sha256={str(f):hashlib.sha256(f.read_bytes()).hexdigest()
                   for f in [source, ppa, Path(__file__).resolve()]}), indent=2)+'\n')
