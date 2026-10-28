/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include "camera_color.h"
#include "camera_frame.h"
#define CAMERA_REFERENCE_VIEW_LEFT 24
#define CAMERA_REFERENCE_VIEW_RIGHT 696
#define CAMERA_REFERENCE_VIEW_TOP 22
#define CAMERA_REFERENCE_VIEW_HEIGHT 556
#define CAMERA_REFERENCE_VIEW_WIDTH (CAMERA_REFERENCE_VIEW_RIGHT-CAMERA_REFERENCE_VIEW_LEFT)
#define CAMERA_REFERENCE_GUIDE_COUNT 8
/* Preserve the photo/video field of view, but discard canvas-only letterbox
 * pixels before fitting the display. Low-resolution USB input then uses the
 * whole viewfinder instead of being contained twice inside a smaller canvas. */
static struct camera_reference_rect camera_reference_crop(unsigned canvas,unsigned width,unsigned height,unsigned zoom,unsigned aspect)
{
  struct camera_reference_rect crop=camera_frame_crop(canvas,600,width,height,zoom,aspect);
  if(aspect)return crop;
  struct camera_reference_rect image=camera_frame_content(canvas,600,width,height);
  unsigned left=crop.x>image.x?crop.x:image.x,top=crop.y>image.y?crop.y:image.y;
  unsigned right=crop.x+crop.w<image.x+image.w?crop.x+crop.w:image.x+image.w;
  unsigned bottom=crop.y+crop.h<image.y+image.h?crop.y+crop.h:image.y+image.h;
  return (struct camera_reference_rect){left,top,right-left,bottom-top};
}
static struct camera_reference_rect camera_reference_output(unsigned canvas,unsigned width,unsigned height,unsigned zoom,unsigned aspect)
{
  struct camera_reference_rect crop=camera_reference_crop(canvas,width,height,zoom,aspect);
  unsigned rw=aspect?camera_aspects[aspect][0]:crop.w,rh=aspect?camera_aspects[aspect][1]:crop.h;
  unsigned w=CAMERA_REFERENCE_VIEW_WIDTH,h=(uint64_t)rh*w/rw;
  if(h>CAMERA_REFERENCE_VIEW_HEIGHT){h=CAMERA_REFERENCE_VIEW_HEIGHT;w=(uint64_t)rw*h/rh;}
  if(!w)w=1;
  if(!h)h=1;
  return (struct camera_reference_rect){CAMERA_REFERENCE_VIEW_LEFT+(CAMERA_REFERENCE_VIEW_WIDTH-w)/2,
    CAMERA_REFERENCE_VIEW_TOP+(CAMERA_REFERENCE_VIEW_HEIGHT-h)/2,w,h};
}
static struct camera_reference_rect camera_reference_frame(unsigned canvas,unsigned width,unsigned height,unsigned zoom,unsigned aspect)
{
  return camera_reference_output(canvas,width,height,zoom,aspect);
}
static void camera_reference_guide(struct camera_reference_rect frame,unsigned index,int line[4])
{
  unsigned length=frame.w<24?frame.w:24;if(frame.h<length)length=frame.h;
  bool right=(index%4)>=2,bottom=index>=4,vertical=index%2;
  line[0]=frame.x+(right?frame.w-(vertical?1:length):0);
  line[1]=frame.y+(bottom?frame.h-(vertical?length:1):0);
  line[2]=vertical?1:length;line[3]=vertical?length:1;
}

/* Coordinates are shared by the visual compositor and hit testing. */
static int camera_reference_hit(int x,int y)
{
  if(x>=700&&x<764&&y>=160&&y<440)return 5;
  if(x>=836&&x<948&&y>=244&&y<356)return 1;
  if(x>=988&&x<1024&&y>=210&&y<390)return 2;
  if(x>=856&&x<934&&y>=24&&y<96)return 3;
  if(x>=700&&x<764&&y>=28&&y<118)return 6;
  if(x>=856&&x<934&&y>=504&&y<582)return 7;
  if(x>=836&&x<948&&y>=128&&y<208)return 4;
  if(x>=800&&x<856&&y>=504&&y<582)return 16;
  return 0;
}

/* Mini colour cards share geometry across LVGL and the live compositor. */
#define CAMERA_FILTER_X 772
#define CAMERA_FILTER_Y 116
#define CAMERA_FILTER_W 56
#define CAMERA_FILTER_H 296
static const uint32_t camera_reference_filter_colors[CAMERA_COLOR_STYLE_COUNT]={
  0xb8b7ae,0xed7655,0x909090,0xe9b66e,0x85b6d6,0xb2a084
};
#define CAMERA_FILTER_DISMISS (8+CAMERA_COLOR_STYLE_COUNT)
#define CAMERA_FILTER_GAP (CAMERA_FILTER_DISMISS+1)
static int camera_reference_filter_x(unsigned index)
{(void)index;return CAMERA_FILTER_X+8;}
static int camera_reference_filter_y(unsigned index)
{return CAMERA_FILTER_Y+8+index*48;}
static int camera_reference_control_hit(int x,int y,bool filters)
{
  int hit=camera_reference_hit(x,y);
  if(!filters||hit==1||hit==2||hit==6)return hit;
  if(x>=CAMERA_FILTER_X&&x<CAMERA_FILTER_X+CAMERA_FILTER_W&&
     y>=CAMERA_FILTER_Y&&y<CAMERA_FILTER_Y+CAMERA_FILTER_H){
    for(unsigned i=0;i<CAMERA_COLOR_STYLE_COUNT;i++){
      int cx=camera_reference_filter_x(i),cy=camera_reference_filter_y(i);
      if(x>=cx&&x<cx+40&&y>=cy&&y<cy+40)return 8+i;
    }
    return CAMERA_FILTER_GAP;
  }
  return CAMERA_FILTER_DISMISS;
}

static const unsigned camera_reference_zoom_values[4]={100,150,200,300};
static int camera_reference_zoom_index(unsigned percent)
{
  for(int i=0;i<4;i++)if(camera_reference_zoom_values[i]==percent)return i;
  return -1;
}
static int camera_reference_zoom_clamp(int index)
{return index<0?0:index>3?3:index;}
static int camera_reference_zoom_drag(int start,int down,int current)
{return camera_reference_zoom_clamp(start+(down-current)/48);}
static int camera_reference_zoom_tap(int y)
{return camera_reference_zoom_clamp(3-(y-172)/64);}
