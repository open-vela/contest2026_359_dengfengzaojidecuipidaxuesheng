#!/usr/bin/env python3
"""Check production RGB565 camera controls under ASan/UBSan, without hardware."""
from pathlib import Path
import argparse,subprocess,tempfile

p=argparse.ArgumentParser();p.add_argument('--output',type=Path);a=p.parse_args()
out=a.output or Path(tempfile.mkdtemp(prefix='camera-controls-'));out.mkdir(parents=True,exist_ok=True)
source=(Path(__file__).resolve().parents[1]/'overlay/apps/system/desktop/qpk_runtime.c').read_text()
start=source.index('static unsigned g_camera_zoom_percent=100;')
end=source.index('static uint64_t qpk_now_ms(void)\n{',start)
prelude='''#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdbool.h>
#include "camera_reference_pixels.h"
#include "camera_reference_layout.h"
#include "camera_reference_controls.h"
#include "camera_color.h"
#define QPK_CAMERA_RAW_HEIGHT 600
#define QPK_CAMERA_BAR_LEFT 896
#define QPK_CAMERA_PHOTO_NOTICE_MS 2000
static pthread_mutex_t g_camera_lock=PTHREAD_MUTEX_INITIALIZER;
static int g_photo_status;
static uint64_t g_photo_status_ms,now=100;
static uint64_t qpk_now_ms(void){return now;}
/* Saving is asynchronous in production; this compositor fixture owns status. */
static void qpk_camera_photo_poll_locked(void){}
static bool video;
static int recording;
static bool qpk_video_selected(void){return video;}
static int qpk_video_status(unsigned *elapsed){if(elapsed)*elapsed=65000;return recording;}
'''
test='''
int main(int argc,char **argv){
  assert(argc==3);
  uint16_t *allocation=malloc((1024*600+2)*sizeof(uint16_t));assert(allocation);
  uint16_t *pixels=allocation+1;allocation[0]=allocation[1024*600+1]=0x55aa;
  for(int i=0;i<1024*600;i++)pixels[i]=0x1234;
  assert(qpk_camera_bar_blit(pixels,1024)==0);
  assert(pixels[200*1024+512]==0x1234); /* Visible image interior survives. */
  assert(pixels[599*1024+512]==0&&pixels[300*1024+20]==0);
  assert(pixels[74*1024+300]==0); /* No heavy rails across the viewfinder. */
  assert(pixels[103*1024+24]==qpk_camera_rgb565(0x676268));
  assert(pixels[22*1024+24]==0); /* The frame fits the image, not a square mockup. */
  assert(pixels[26*1024+28]==qpk_camera_rgb565(0x35ca70));
  const struct camera_reference_tile *shutter=&camera_reference_tiles[CAMERA_REFERENCE_SHUTTER_TILE];
  for(int y=0;y<shutter->h;y++)assert(!memcmp(pixels+(shutter->y+y)*1024+shutter->x,
                                     shutter->pixels+y*shutter->w,shutter->w*2));
  assert(pixels[300*1024+892]==qpk_camera_rgb565(0xef3829));
  for(int i=0;i<shutter->w*shutter->h;i++)assert(shutter->pixels[i]!=0xffff);
  assert(camera_reference_hit(892,300)==1&&camera_reference_hit(1008,300)==2);
  assert(camera_reference_hit(90,300)==0&&camera_reference_hit(892,60)==3);
  assert(camera_reference_hit(90,48)==0&&camera_reference_hit(90,550)==0&&camera_reference_hit(732,540)==0);
  assert(camera_reference_hit(732,64)==6&&camera_reference_hit(892,540)==7);
  assert(camera_reference_hit(732,108)==6&&camera_reference_hit(892,196)==4);
  assert(!qpk_camera_style_set(CAMERA_COLOR_STYLE_COUNT));
  assert(camera_color_pixel(0xf800,0)==0xf800);
  assert(camera_color_pixel(0xf800,2)!=0xf800);
  assert(qpk_camera_style_set(2)&&qpk_camera_style()==2);
  qpk_camera_bar_blit(pixels,1024);
  const struct camera_reference_tile *caption=&camera_reference_tiles[CAMERA_REFERENCE_STYLE_TILE];
  for(int y=0;y<caption->h;y++)assert(!memcmp(pixels+(caption->y+y)*1024+caption->x,
                                          camera_style_2+y*caption->w,caption->w*2));
  assert(camera_color_pixel(0xffff,2)==0xffff&&camera_color_pixel(0,2)==0);
  assert(qpk_camera_style_set(0));
  assert(camera_reference_hit(892,160)==4&&camera_reference_hit(810,300)==0&&camera_reference_hit(500,300)==0);
  FILE *screenshot=fopen(argv[1],"wb");assert(screenshot);
  assert(fwrite(pixels,2,1024*600,screenshot)==1024*600);fclose(screenshot);
  assert(qpk_camera_filter_action(6)&&qpk_camera_filters_open()&&qpk_camera_style()==0);
  for(unsigned i=0;i<CAMERA_COLOR_STYLE_COUNT;i++){
    int x=camera_reference_filter_x(i),y=camera_reference_filter_y(i);
    assert(camera_reference_control_hit(x+20,y+20,true)==8+i);
    assert(qpk_camera_filter_action(8+i)&&!qpk_camera_filters_open()&&qpk_camera_style()==i);
    qpk_camera_filter_action(6);
  }
  assert(camera_reference_control_hit(800,166,true)==CAMERA_FILTER_GAP);
  assert(camera_reference_control_hit(500,300,true)==CAMERA_FILTER_DISMISS);
  assert(camera_reference_control_hit(892,300,true)==1);
  assert(qpk_camera_filter_action(CAMERA_FILTER_GAP)&&qpk_camera_filters_open());
  assert(qpk_camera_filter_action(9)&&!qpk_camera_filters_open()&&qpk_camera_style()==1);
  qpk_camera_filter_action(6);qpk_camera_bar_blit(pixels,1024);
  for(unsigned i=0;i<CAMERA_COLOR_STYLE_COUNT;i++)for(unsigned y=4;y<36;y++)for(unsigned x=4;x<36;x++)
    assert(pixels[(camera_reference_filter_y(i)+y)*1024+camera_reference_filter_x(i)+x]==
      qpk_camera_rgb565(camera_reference_filter_colors[i]));
  assert(pixels[300*1024+892]==qpk_camera_rgb565(0xef3829));
  FILE *popup=fopen(argv[2],"wb");assert(popup);
  assert(fwrite(pixels,2,1024*600,popup)==1024*600);fclose(popup);
  assert(qpk_camera_filter_action(CAMERA_FILTER_DISMISS)&&!qpk_camera_filters_open()&&qpk_camera_style()==1);
  qpk_camera_filter_action(6);assert(!qpk_camera_filter_action(1)&&!qpk_camera_filters_open());
  qpk_camera_style_set(0);
  g_photo_status=-5;g_photo_status_ms=now;
  assert(qpk_camera_bar_blit(pixels,1024)==-5);
  assert(pixels[364*1024+892]==qpk_camera_rgb565(0xe34f4f));
  g_photo_status=2;assert(qpk_camera_bar_blit(pixels,1024)==2);
  assert(pixels[364*1024+892]==qpk_camera_rgb565(0x2fbf8f));
  now+=2001;assert(qpk_camera_bar_blit(pixels,1024)==0);
  video=true;recording=2;qpk_camera_bar_blit(pixels,1024);
  assert(pixels[300*1024+892]==0xffff);
  const struct camera_reference_tile *mode=&camera_reference_tiles[CAMERA_REFERENCE_MODE_TILE];
  for(int y=0;y<mode->h;y++)assert(!memcmp(pixels+(mode->y+y)*1024+mode->x,
                                      camera_mode_video+y*mode->w,mode->w*2));
  assert(allocation[0]==0x55aa&&allocation[1024*600+1]==0x55aa);
  assert(!qpk_camera_zoom_set(0)&&!qpk_camera_zoom_set(999));
  assert(camera_reference_hit(732,300)==5);
  assert(camera_reference_zoom_drag(0,390,330)==1);
  assert(camera_reference_zoom_drag(0,390,100)==3);
  assert(camera_reference_zoom_drag(3,100,500)==0);
  assert(camera_reference_zoom_tap(268)==2);
  for(unsigned style=0;style<CAMERA_COLOR_STYLE_COUNT;style++)for(unsigned width=896;width<=1024;width+=128)for(unsigned index=0;index<4;index++){
    assert(qpk_camera_style_set(style));
    unsigned zoom=camera_reference_zoom_values[index];assert(qpk_camera_zoom_set(zoom));
    assert(qpk_camera_zoom_percent()==zoom);
    for(unsigned y=0;y<600;y++)for(unsigned x=0;x<width;x++)pixels[y*1024+x]=(uint16_t)((y<<5)^x);
    unsigned crop_w=width*100/zoom,crop_h=600*100/zoom;
    unsigned height=crop_h*672/crop_w,top=22+(556-height)/2;
    unsigned crop_x=(width-crop_w)/2,crop_y=(600-crop_h)/2;
    qpk_camera_reference_prepare_view(pixels,width,zoom);
    for(unsigned y=0;y<height;y+=height/11)for(unsigned x=0;x<672;x+=61){
      unsigned sy=crop_y+y*crop_h/height,sx=crop_x+x*crop_w/672;
      assert(pixels[(top+y)*1024+24+x]==camera_color_pixel((uint16_t)((sy<<5)^sx),style));
    }
    assert(allocation[0]==0x55aa&&allocation[1024*600+1]==0x55aa);
  }
  /* Independent solid image fixtures expose letterbox/frame mismatches. */
  const unsigned cases[][11]={
    {1024,1024,600,0,0,1024,600,24,103,672,393},
    {896,640,480,48,0,800,600,24,48,672,504},
    {896,1280,720,0,48,896,504,24,111,672,378},
    {896,160,120,48,0,800,600,24,48,672,504},
    {896,480,640,223,0,450,600,151,22,417,556},
    {896,320,320,148,0,600,600,82,22,556,556}
  };
  const unsigned case_count=sizeof(cases)/sizeof(cases[0]);
  /* Coordinate-coded input catches in-place sampling corruption, including
   * portrait/square frames and enlarged zoom crops. */
  qpk_camera_style_set(0);
  for(unsigned c=0;c<case_count;c++)for(unsigned aspect=0;aspect<=4;aspect++)for(unsigned z=0;z<4;z++){
    const unsigned *a=cases[c];unsigned zoom=camera_reference_zoom_values[z];
    memset(pixels,0,1024*600*2);
    for(unsigned y=a[4];y<a[4]+a[6];y++)for(unsigned x=a[3];x<a[3]+a[5];x++)
      pixels[y*1024+x]=(uint16_t)(0x8000|((y<<5)^x));
    struct camera_reference_rect crop=camera_reference_crop(a[0],a[1],a[2],zoom,aspect);
    struct camera_reference_rect target=camera_reference_output(a[0],a[1],a[2],zoom,aspect);
    qpk_camera_reference_prepare_frame(pixels,a[0],zoom,a[1],a[2],aspect);
    for(unsigned y=0;y<target.h;y+=17)for(unsigned x=0;x<target.w;x+=19){
      unsigned sx=crop.x+x*crop.w/target.w,sy=crop.y+y*crop.h/target.h;
      assert(pixels[(target.y+y)*1024+target.x+x]==(uint16_t)(0x8000|((sy<<5)^sx)));
    }
    assert(allocation[0]==0x55aa&&allocation[1024*600+1]==0x55aa);
  }
  qpk_camera_style_set(0);video=false;g_photo_status=0;
  for(unsigned c=0;c<case_count;c++)for(unsigned z=0;z<4;z++){
    const unsigned *a=cases[c];unsigned zoom=camera_reference_zoom_values[z];qpk_camera_zoom_set(zoom);
    memset(pixels,0,1024*600*2);
    for(unsigned y=a[4];y<a[4]+a[6];y++)for(unsigned x=a[3];x<a[3]+a[5];x++)pixels[y*1024+x]=0x1234;
    qpk_camera_bar_draw(pixels,a[0],NULL,a[1],a[2]);
    struct camera_reference_rect frame=camera_reference_frame(a[0],a[1],a[2],zoom,0);
    if(!z){assert(frame.x==a[7]&&frame.y==a[8]&&frame.w==a[9]);
      assert(frame.h==a[10]);}
    for(unsigned y=22;y<578;y++)for(unsigned x=24;x<696;x++){
      if(x<=32&&y<=30)continue; /* Live status dot is outside the image. */
      bool inside=x>=frame.x&&x<frame.x+frame.w&&y>=frame.y&&y<frame.y+frame.h;
      if((pixels[y*1024+x]!=0)!=inside)fprintf(stderr,"VIEW_MISMATCH case=%u zoom=%u x=%u y=%u pixel=%x inside=%d frame=%u,%u,%u,%u",c,zoom,x,y,pixels[y*1024+x],inside,frame.x,frame.y,frame.w,frame.h);
      assert((pixels[y*1024+x]!=0)==inside);
    }
    /* Prepared MJPEG output must keep exactly the same guides and letterbox. */
    qpk_camera_bar_draw(pixels,a[0],&(struct qpk_camera_view_state){zoom,0,0},a[1],a[2]);
    for(unsigned i=0;i<CAMERA_REFERENCE_GUIDE_COUNT;i++){
      int line[4];camera_reference_guide(frame,i,line);
      assert(pixels[line[1]*1024+line[0]]==qpk_camera_rgb565(0x676268));
    }
  }
  recording=0;assert(!qpk_camera_aspect_set(5));
  for(unsigned aspect=1;aspect<=4;aspect++)for(unsigned c=0;c<case_count;c++)for(unsigned z=0;z<4;z++){
    const unsigned *a=cases[c];unsigned zoom=camera_reference_zoom_values[z];qpk_camera_zoom_set(zoom);
    assert(qpk_camera_aspect_set(aspect));memset(pixels,0,1024*600*2);
    for(unsigned y=a[4];y<a[4]+a[6];y++)for(unsigned x=a[3];x<a[3]+a[5];x++)pixels[y*1024+x]=0x1234;
    qpk_camera_bar_draw(pixels,a[0],NULL,a[1],a[2]);
    struct camera_reference_rect frame=camera_reference_frame(a[0],a[1],a[2],zoom,aspect);
    assert(frame.w==(aspect==4?556:672));assert(frame.h==(aspect==1?420:aspect==2?378:aspect==3?504:556));
    for(unsigned y=22;y<578;y++)for(unsigned x=24;x<696;x++){
      if(x<=32&&y<=30)continue; /* Live status dot is outside the image. */
      bool inside=x>=frame.x&&x<frame.x+frame.w&&y>=frame.y&&y<frame.y+frame.h;
      if((pixels[y*1024+x]!=0)!=inside)fprintf(stderr,"VIEW_MISMATCH case=%u zoom=%u x=%u y=%u pixel=%x inside=%d frame=%u,%u,%u,%u",c,zoom,x,y,pixels[y*1024+x],inside,frame.x,frame.y,frame.w,frame.h);
      assert((pixels[y*1024+x]!=0)==inside);
    }
  }
  recording=2;assert(!qpk_camera_aspect_set(1)&&qpk_camera_aspect()==4);
  recording=0;qpk_camera_filter_action(16);assert(qpk_camera_aspect()==1);
  /* A selection arriving during fast MJPEG decoding applies next frame. */
  memset(pixels,0,1024*600*2);qpk_camera_aspect_set(4);
  qpk_camera_bar_draw(pixels,896,&(struct qpk_camera_view_state){100,0,0},640,480);
  assert(pixels[48*1024+24]==qpk_camera_rgb565(0x676268));
  assert(pixels[22*1024+82]==0);
  qpk_camera_style_set(4);memset(pixels,0,1024*600*2);
  qpk_camera_bar_draw(pixels,896,&(struct qpk_camera_view_state){100,1,2},640,480);
  assert(pixels[111*1024+24]==qpk_camera_rgb565(0x676268));
  for(int y=0;y<caption->h;y++)assert(!memcmp(pixels+(caption->y+y)*1024+caption->x,
    camera_style_1+y*caption->w,caption->w*2));
  assert(camera_reference_control_hit(828,540,false)==16);
  assert(camera_reference_control_hit(828,540,true)==CAMERA_FILTER_DISMISS);
  free(allocation);puts("CAMERA_CONTROLS_OK: maximized 672px view, six input sizes including low-res/portrait, no double letterbox, four aspects, all zooms/styles, shutter, frame boundaries and recording guard");
}
'''
unit=out/'camera-controls.c';unit.write_text(prelude+source[start:end]+test)
binary=out/'camera-controls'
subprocess.run(['gcc','-std=gnu11','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread','-I'+str(Path(__file__).resolve().parents[1]/'overlay/apps/system/desktop'),str(unit),'-o',str(binary)],check=True)
subprocess.run([str(binary),str(out/"live-camera.rgb565"),str(out/"live-filter.rgb565")],check=True)

usb=(Path(__file__).resolve().parents[1]/'overlay/apps/system/desktop/qpk_camera_usb.inc').read_text()
flush=usb[usb.index('static int qpk_camera_clean_frame'):usb.index('static uint64_t qpk_usb_preview_now')]
cache_test='''#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
static uintptr_t next,end;
static unsigned calls,fail;
static int esp_mipi_dsi_flush_framebuffer(void *address,size_t bytes){
  assert((uintptr_t)address==next&&bytes>0&&bytes<=16384&&next+bytes<=end);
  calls++;if(fail&&calls==fail)return -5;next+=bytes;return 0;
}
'''+flush+'''
int main(void){
  size_t bytes=1024*600*2;uint16_t *frame=malloc(bytes);assert(frame);
  next=(uintptr_t)frame;end=next+bytes;
  assert(qpk_camera_clean_frame(frame,bytes)==0&&next==end&&calls==75);
  next=(uintptr_t)frame;calls=0;fail=3;
  assert(qpk_camera_clean_frame(frame,bytes)==-5&&calls==3);
  free(frame);puts("CAMERA_CACHE_OK: actual display writeback invoked, bounded 16KiB chunks, full frame covered, failure propagated");
}
'''
unit=out/'camera-cache.c';unit.write_text(cache_test);binary=out/'camera-cache'
subprocess.run(['gcc','-std=gnu11','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(unit),'-o',str(binary)],check=True)
subprocess.run([str(binary)],check=True)

# Consume navigation only after display and recording ownership have ended.
gate=source[source.index('bool qpk_runtime_camera_take_album(void)'):source.index('static JSValue js_camera_photo_status(')]
gate_test='''#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
static pthread_mutex_t g_camera_lock=PTHREAD_MUTEX_INITIALIZER;
static struct {char package[64];} g_qpk;
enum {QPK_CAMERA_IDLE,QPK_CAMERA_RUNNING};
static int g_camera_state,recording;
static unsigned g_camera_ui_action;
static int qpk_video_status(void *unused){(void)unused;return recording;}
'''+gate+'''
int main(void){
  strcpy(g_qpk.package,"com.openvela.camera.preview");g_camera_ui_action=7;
  g_camera_state=QPK_CAMERA_RUNNING;assert(!qpk_runtime_camera_take_album()&&g_camera_ui_action==7);
  g_camera_state=QPK_CAMERA_IDLE;recording=3;assert(!qpk_runtime_camera_take_album()&&g_camera_ui_action==7);
  recording=4;strcpy(g_qpk.package,"other.app");assert(!qpk_runtime_camera_take_album()&&g_camera_ui_action==7);
  strcpy(g_qpk.package,"com.openvela.camera.preview");assert(qpk_runtime_camera_take_album()&&g_camera_ui_action==0);
  assert(!qpk_runtime_camera_take_album());g_camera_ui_action=3;assert(!qpk_runtime_camera_take_album()&&g_camera_ui_action==3);
  puts("CAMERA_ALBUM_GATE_OK: waits for preview release and video finalization, package ownership, consume once");
}
'''
unit=out/'camera-album-gate.c';unit.write_text(gate_test);binary=out/'camera-album-gate'
subprocess.run(['gcc','-std=gnu11','-O1','-g','-pthread','-fsanitize=address,undefined',str(unit),'-o',str(binary)],check=True)
subprocess.run([str(binary)],check=True)
