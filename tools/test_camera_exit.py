#!/usr/bin/env python3
"""Execute production direct-preview pointer polling and deferred shell exit."""
from pathlib import Path
import argparse,subprocess
from test_display_flash import function
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
repo=Path(__file__).resolve().parents[1]
d=repo/'app/espdl-quickapp/overlay/apps/system/desktop'
source=(d/'qpk_runtime.c').read_text()
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include "camera_reference_layout.h"
enum {QPK_CAMERA_IDLE,QPK_CAMERA_ACTIVE,QPK_CAMERA_FAILED,QPK_CAMERA_STOPPING};
enum {LV_INDEV_TYPE_POINTER=0,LV_INDEV_STATE_RELEASED=0,LV_INDEV_STATE_PRESSED=1};
typedef struct {int x,y;} lv_point_t;
typedef struct {lv_point_t point;int state;bool continue_reading;} lv_indev_data_t;
typedef int lv_indev_t;
typedef void (*lv_indev_read_cb_t)(lv_indev_t *,lv_indev_data_t *);
static lv_indev_data_t input;
static lv_indev_t device;
static struct {bool direct_preview,thread_running,thread_alive,stop_requested;int thread_error;} g_camera;
static pthread_mutex_t g_camera_lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned g_camera_state,g_camera_ui_action,stops,captures,zoom=100;
static bool g_camera_redraw,g_camera_cleanup_blocked,filters;
static uint64_t g_camera_retry_at;
static int g_camera_last_error,video_state;
static uint64_t qpk_now_ms(void){return 0;}
static void qpk_camera_service_start(void){}
static bool qpk_runtime_camera_active(void){return g_camera.direct_preview||g_camera_redraw;}
static void qpk_camera_stop(void){stops++;g_camera_state=QPK_CAMERA_STOPPING;}
static void qpk_camera_filters_set(bool v){filters=v;}
static bool qpk_camera_filters_open(void){return filters;}
static lv_indev_t *lv_indev_get_next(lv_indev_t *p){return p?NULL:&device;}
static int lv_indev_get_type(lv_indev_t *p){(void)p;return LV_INDEV_TYPE_POINTER;}
static void read_input(lv_indev_t *p,lv_indev_data_t *d){(void)p;*d=input;}
static lv_indev_read_cb_t lv_indev_get_read_cb(lv_indev_t *p){(void)p;return read_input;}
static unsigned qpk_camera_zoom_percent(void){return zoom;}
static void qpk_camera_zoom_set(unsigned v){zoom=v;}
static bool qpk_video_selected(void){return false;}
static int qpk_video_status(void *p){(void)p;return video_state;}
static void qpk_video_mode(bool b){(void)b;}
static void qpk_video_start(void){}
static void qpk_video_stop(void){}
static bool qpk_camera_filter_action(int a){(void)a;return false;}
static void qpk_runtime_camera_capture(void){captures++;}
'''
actual=function(source,'qpk_runtime_camera_poll','bool ')+function(source,'qpk_runtime_camera_take_exit','bool ')
suffix=r'''
static void feed(int state,int x,int y){input=(lv_indev_data_t){{x,y},state,false};qpk_runtime_camera_poll();}
static void begin(void){g_camera.direct_preview=false;g_camera_redraw=false;qpk_runtime_camera_poll();g_camera.direct_preview=true;g_camera_state=QPK_CAMERA_ACTIVE;g_camera_ui_action=stops=0;feed(LV_INDEV_STATE_RELEASED,500,300);}
static void finished(void){fprintf(stderr,"stops=%u state=%u action=%u\n",stops,g_camera_state,g_camera_ui_action);assert(stops==1&&!qpk_runtime_camera_take_exit());g_camera_state=QPK_CAMERA_IDLE;g_camera.direct_preview=false;g_camera_redraw=true;assert(!qpk_runtime_camera_take_exit());video_state=3;g_camera_redraw=false;assert(!qpk_runtime_camera_take_exit());video_state=0;assert(qpk_runtime_camera_take_exit());assert(!qpk_runtime_camera_take_exit());}
int main(void){
 begin();feed(1,1008,300);feed(0,1008,300);finished();
 begin();feed(1,1010,120);feed(1,950,125);finished();
 begin();feed(1,12,240);feed(0,75,245);finished();
 begin();feed(1,500,590);feed(0,505,500);finished();
 begin();feed(1,1010,120);feed(1,990,230);feed(0,930,125);assert(stops==0);
 begin();feed(1,700,320);feed(0,620,320);assert(stops==0);
 begin();feed(1,892,300);feed(0,892,300);assert(captures==1&&stops==0);
 puts("CAMERA_EXIT_OK: tap, both side gestures, bottom gesture, drift cancellation, shutter isolation, cleanup/redraw/recording gates and one-shot navigation");
}
'''
(out/'test.c').write_text(prefix+actual+suffix)
subprocess.run(['gcc','-std=gnu11','-O1','-g','-pthread','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(d),str(out/'test.c'),'-o',str(out/'test')],check=True)
subprocess.run([str(out/'test')],check=True)
