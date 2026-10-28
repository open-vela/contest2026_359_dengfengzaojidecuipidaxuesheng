#!/usr/bin/env python3
"""Native diagnostic pointer taps through real LVGL hit testing and events."""
import argparse,json,os,re,subprocess
from pathlib import Path
from render_all_ui import function
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--apps',type=Path,required=True);p.add_argument('--cache',type=Path,required=True)
p.add_argument('--output',type=Path,required=True);a=p.parse_args()
repo=Path(__file__).resolve().parents[1];desktop=repo/'app/espdl-quickapp/overlay/apps/system/desktop'
out=a.output;out.mkdir(parents=True,exist_ok=True)
source=(desktop/'glass_ui_probe.inc').read_text()
prefix=r'''
#include <assert.h>
#include <stdio.h>
#include <lvgl/lvgl.h>
#include "glass_motion.h"
#include "glass_input_guard.h"
static lv_point_t g_ui_probe_pointer_point;
static lv_indev_state_t g_ui_probe_pointer_state;
static unsigned pressed,shorts,clicks,releases;
static bool delete_on_short;
static void count(lv_event_t *e)
{
 lv_event_code_t code=lv_event_get_code(e);
 if(code==LV_EVENT_PRESSED){assert(lv_indev_get_type(lv_event_get_indev(e))==LV_INDEV_TYPE_POINTER);pressed++;}
 if(code==LV_EVENT_RELEASED)releases++;
 if(code==LV_EVENT_CLICKED)clicks++;
 if(code==LV_EVENT_SHORT_CLICKED){shorts++;if(delete_on_short)lv_obj_delete(lv_event_get_target(e));}
}
static void dummy(lv_event_t *e){(void)e;}
'''
main=r'''
int main(void)
{
 lv_init();lv_display_t *d=lv_display_create(1024,600);
 lv_obj_t *button=lv_button_create(lv_screen_active());lv_obj_set_pos(button,40,40);lv_obj_set_size(button,200,100);
 glass_motion_button(button);assert(glass_input_guard_install(button));
 lv_obj_add_event_cb(button,count,LV_EVENT_ALL,NULL);
 lv_obj_add_event_cb(button,dummy,LV_EVENT_DELETE,NULL);lv_obj_add_event_cb(button,dummy,LV_EVENT_DELETE,NULL);
 assert(lv_obj_get_event_count(button)==5);lv_obj_update_layout(lv_screen_active());
 for(unsigned i=0;i<100;i++)assert(desktop_ui_probe_pointer_tap(100,80));
 assert(pressed==100&&shorts==100&&clicks==100&&releases==100);
 assert(lv_obj_get_event_count(button)==6);
 assert(!desktop_ui_probe_pointer_tap(-1,80)&&!desktop_ui_probe_pointer_tap(1024,80));
 assert(pressed==100);
 delete_on_short=true;assert(desktop_ui_probe_pointer_tap(100,80));
 assert(shorts==101&&pressed==101);
 assert(desktop_ui_probe_pointer_tap(100,80));assert(pressed==101);
 lv_display_delete(d);lv_deinit();puts("UI_POINTER_TAP_OK: actual native hit testing, guards, lazy-motion growth, press/release/short/click, target deletion, input disposal and coordinate bounds");
}
'''
(out/'test.c').write_text(prefix+'\n'.join(function(source,n) for n in ['desktop_ui_probe_pointer_read','desktop_ui_probe_pointer_tap'])+main)
objects=sorted(a.cache.glob('lvgl-*.o'))
symbols=subprocess.check_output(['nm','-A','-g','--defined-only',*map(str,objects)],text=True)
event=re.search(r'^(.+\.o):[0-9a-f]+ T lv_event_send$',symbols,re.M);assert event
objects.remove(Path(event[1]))
flags=['-std=gnu11','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie',
 '-DLV_CONF_INCLUDE_SIMPLE','-I'+str(a.cache),'-I'+str(a.apps/'graphics/lvgl'),'-I'+str(desktop)]
subprocess.run(['gcc',*flags,str(out/'test.c'),str(a.apps/'graphics/lvgl/lvgl/src/misc/lv_event.c'),*map(str,objects),'-lm','-o',str(out/'test')],check=True)
env=os.environ.copy();env['ASAN_OPTIONS']='detect_leaks=0'
subprocess.run([str(out/'test')],env=env,check=True,timeout=15)
(out/'RESULT.json').write_text(json.dumps(dict(passed=True,actual_pointer_helper=True,
 actual_lvgl=True,press_release_cycles=100,native_short_click_verified=True,hardware_tested=False),indent=2)+'\n')
