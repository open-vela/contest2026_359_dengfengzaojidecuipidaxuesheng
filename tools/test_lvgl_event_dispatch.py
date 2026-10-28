#!/usr/bin/env python3
"""Reproduce physical first-press event-vector UAF using real LVGL and motion."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
from prepare_lvgl_event_dispatch import apply, MARKER
from render_all_ui import function

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--apps', type=Path, required=True)
p.add_argument('--cache', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
repo = Path(__file__).resolve().parents[1]
out = a.output.resolve(); out.mkdir(parents=True, exist_ok=True)
src = a.apps/'graphics/lvgl/lvgl/src/misc/lv_event.c'
original = src.read_text()
assert MARKER not in original, 'Negative control requires the unmodified dispatcher'
fixture = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lvgl/lvgl.h>
#include "glass_motion.h"
#include "glass_input_guard.h"
static lv_indev_state_t input_state;
static unsigned hits, added_hits, inside;
static lv_obj_t *button;
static void read_pointer(lv_indev_t *input,lv_indev_data_t *data)
{(void)input;data->point.x=100;data->point.y=100;data->state=input_state;}
static void count(lv_event_t *e){(void)e;hits++;}
static void added(lv_event_t *e){(void)e;added_hits++;}
static void grow(lv_event_t *e)
{
  if(inside)return;
  inside=1;
  for(unsigned i=0;i<40;i++)lv_obj_add_event_cb(button,added,LV_EVENT_PRESSED,NULL);
  if(lv_event_get_user_data(e))lv_obj_send_event(button,LV_EVENT_PRESSED,NULL);
}
static void erase(lv_event_t *e)
{lv_obj_t *obj=lv_event_get_target(e);while(lv_obj_get_event_count(obj))lv_obj_remove_event(obj,0);}
static void destroy(lv_event_t *e){lv_obj_delete(lv_event_get_target(e));button=NULL;}
static void stop(lv_event_t *e){lv_event_stop_processing(e);}
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *b)
{(void)a;(void)b;lv_display_flush_ready(d);}
int main(int argc,char **argv)
{
  assert(argc==2);lv_init();
  lv_display_t *display=lv_display_create(1024,600);
  static unsigned char pixels[1024*32*2];
  lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(display,pixels,NULL,sizeof(pixels),LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(display,flush);
  button=lv_button_create(lv_screen_active());
  lv_obj_set_pos(button,46,70);lv_obj_set_size(button,348,144);
  if(!strcmp(argv[1],"press")){
    /* Same five-entry arrangement as the captured physical recorder button.
     * The first callback lazily adds glass_motion_deleted (entry six). */
    glass_motion_button(button);
    lv_obj_add_event_cb(button,count,LV_EVENT_DELETE,NULL);
    assert(glass_input_guard_install(button));
    lv_obj_add_event_cb(button,count,LV_EVENT_CLICKED,NULL);
    lv_obj_add_event_cb(button,count,LV_EVENT_DELETE,NULL);
    assert(lv_obj_get_event_count(button)==5);
    lv_indev_t *input=lv_indev_create();
    lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(input,read_pointer);
    lv_obj_update_layout(lv_screen_active());
    for(unsigned i=0;i<100;i++){
      input_state=LV_INDEV_STATE_PRESSED;lv_indev_read(input);
      input_state=LV_INDEV_STATE_RELEASED;lv_indev_read(input);
      lv_tick_inc(200);lv_timer_handler();
    }
    assert(hits==100);assert(lv_obj_get_event_count(button)==6);
    lv_indev_delete(input);
  }else if(!strcmp(argv[1],"grow")||!strcmp(argv[1],"nested")){
    bool nested=!strcmp(argv[1],"nested");
    lv_obj_add_event_cb(button,grow,LV_EVENT_PRESSED,nested?(void *)1:NULL);
    for(unsigned i=0;i<4;i++)lv_obj_add_event_cb(button,count,LV_EVENT_PRESSED,NULL);
    lv_obj_send_event(button,LV_EVENT_PRESSED,NULL);
    assert(hits==(nested?8:4));assert(added_hits==(nested?40:0));
    lv_obj_send_event(button,LV_EVENT_PRESSED,NULL);
    assert(added_hits==(nested?80:40));
  }else if(!strcmp(argv[1],"erase")){
    lv_obj_add_event_cb(button,erase,LV_EVENT_PRESSED,NULL);
    for(unsigned i=0;i<4;i++)lv_obj_add_event_cb(button,count,LV_EVENT_PRESSED,NULL);
    lv_obj_send_event(button,LV_EVENT_PRESSED,NULL);assert(!hits);
    assert(!lv_obj_get_event_count(button));
  }else if(!strcmp(argv[1],"delete")){
    lv_obj_add_event_cb(button,destroy,LV_EVENT_PRESSED,NULL);
    lv_obj_add_event_cb(button,count,LV_EVENT_PRESSED,NULL);
    assert(lv_obj_send_event(button,LV_EVENT_PRESSED,NULL)==LV_RESULT_INVALID);
    assert(!button&&!hits);
  }else if(!strcmp(argv[1],"stop")){
    lv_obj_add_event_cb(button,stop,LV_EVENT_PRESSED,NULL);
    lv_obj_add_event_cb(button,count,LV_EVENT_PRESSED,NULL);
    lv_obj_send_event(button,LV_EVENT_PRESSED,NULL);assert(!hits);
  }else abort();
  lv_display_delete(display);lv_deinit();puts("LVGL_EVENT_CASE_OK");
}
'''
(out/'test.c').write_text(fixture)
flags = ['-std=gnu11','-O1','-g','-fsanitize=address,undefined',
         '-fno-omit-frame-pointer','-fno-pie','-no-pie','-DLV_CONF_INCLUDE_SIMPLE',
         '-I'+str(a.cache),'-I'+str(a.apps/'graphics/lvgl'),
         '-I'+str(src.parent),
         '-I'+str(repo/'app/espdl-quickapp/overlay/apps/system/desktop')]
objects = sorted(a.cache.glob('lvgl-*.o'))
symbols = subprocess.check_output(['nm','-A','-g','--defined-only',*map(str,objects)], text=True)
event_object = re.search(r'^(.+\.o):[0-9a-f]+ T lv_event_send$',symbols,re.M)
assert event_object; objects.remove(Path(event_object[1]))
original_file = out/'lv_event.original.c'; original_file.write_text(original)
prepared = out/'prepared'; target = prepared/'graphics/lvgl/lvgl/src/misc/lv_event.c'
target.parent.mkdir(parents=True,exist_ok=True);target.write_text(original)
receipt = apply(prepared); first = target.read_bytes(); assert apply(prepared)==receipt
assert target.read_bytes()==first, 'Preparation must be idempotent'
env = os.environ.copy(); env['ASAN_OPTIONS']='detect_leaks=0:halt_on_error=1'
def build(source, name):
    executable=out/name
    subprocess.run(['gcc',*flags,str(out/'test.c'),str(source),*map(str,objects),
                    '-lm','-o',str(executable)],check=True)
    return executable
old = build(original_file,'original')
negative = subprocess.run([str(old),'press'],env=env,text=True,capture_output=True)
(out/'negative.log').write_text(negative.stdout+negative.stderr)
assert negative.returncode!=0 and 'heap-use-after-free' in negative.stderr, negative.stderr[-4000:]
fixed = build(target,'fixed')
cases=['press','grow','nested','erase','delete','stop']
for case in cases:
    result=subprocess.run([str(fixed),case],env=env,text=True,capture_output=True)
    (out/(case+'.log')).write_text(result.stdout+result.stderr)
    assert result.returncode==0,(case,result.stdout,result.stderr)
report=dict(passed=True,negative_control='ASan heap-use-after-free in lv_event_send on physical first press',
            actual_lvgl=True,actual_glass_motion=True,actual_pointer_input=True,
            cases=cases,physical_board_tested=False,prepared_sources=receipt,
            original_sha256=hashlib.sha256(original_file.read_bytes()).hexdigest())
(out/'RESULT.json').write_text(json.dumps(report,indent=2)+'\n')
print('LVGL_EVENT_DISPATCH_OK:',', '.join(cases),'; original physical-press UAF reproduced')
