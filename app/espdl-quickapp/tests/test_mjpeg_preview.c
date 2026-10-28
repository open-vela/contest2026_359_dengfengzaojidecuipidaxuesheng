/* SPDX-License-Identifier: Apache-2.0 */
#include "qpk_mjpeg.h"
#include "camera_frame.h"
#include "camera_color.h"
#include "camera_reference_layout.h"
#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc,char **argv)
{
  assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);
  fseek(f,0,SEEK_END);size_t size=ftell(f);rewind(f);
  uint8_t *input=malloc(size+512);assert(input);assert(fread(input,1,size,f)==size);fclose(f);
  unsigned w=0,h=0;assert(!qpk_mjpeg_probe(input,size,&w,&h));
  size_t count=(size_t)w*h;uint16_t *full=malloc(count*2),*view=malloc((count+16)*2);assert(full&&view);
  void *decoder=qpk_mjpeg_create();assert(decoder);
  assert(!qpk_mjpeg_decode(decoder,input,size,size+512,full,w,h,w,h));
  for(unsigned scale=1;scale<=3;scale++) {
    unsigned sw=w>>scale,sh=h>>scale;assert(sw&&sh);
    assert(!qpk_mjpeg_preview_limits(decoder,sw,sh));
    for(unsigned i=0;i<16;i++)view[count+i]=0xa55a;
    assert(!qpk_mjpeg_decode_view(decoder,input,size,size+512,view,w,h,w,w,h,w,h));
    unsigned dw=w,dh=sh*w/sw;if(dh>h){dh=h;dw=sw*h/sh;}
    unsigned ox=(w-dw)/2,oy=(h-dh)/2;
    for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++) {
      uint16_t expected=0;
      if(x>=ox&&x<ox+dw&&y>=oy&&y<oy+dh)
        expected=full[(((y-oy)*sh/dh)<<scale)*w+(((x-ox)*sw/dw)<<scale)];
      assert(view[y*w+x]==expected);
    }
    for(unsigned i=0;i<16;i++)assert(view[count+i]==0xa55a);
    for(unsigned aspect=0;aspect<=4;aspect++)for(unsigned style=0;style<CAMERA_COLOR_STYLE_COUNT;style++){
      struct camera_reference_rect crop=camera_frame_crop(w,h,w,h,100,aspect);
      struct qpk_mjpeg_view options={.canvas_width=w,.canvas_height=h,.stride=w,.width=w,.height=h,
        .crop_x=crop.x,.crop_y=crop.y,.crop_width=crop.w,.crop_height=crop.h,.style=style};
      assert(!qpk_mjpeg_decode_preview(decoder,input,size,size+512,view,w,h,&options));
      for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++){
        unsigned cx=crop.x+x*crop.w/w,cy=crop.y+y*crop.h/h;
        uint16_t expected=0;
        if(cx>=ox&&cx<ox+dw&&cy>=oy&&cy<oy+dh)
          expected=camera_color_pixel(full[(((cy-oy)*sh/dh)<<scale)*w+(((cx-ox)*sw/dw)<<scale)],style);
        assert(view[y*w+x]==expected);
      }
      for(unsigned i=0;i<16;i++)assert(view[count+i]==0xa55a);
      options.crop_width=w+1;
      assert(qpk_mjpeg_decode_preview(decoder,input,size,size+512,view,w,h,&options)==-EINVAL);
    }
    assert(qpk_mjpeg_decode_view(decoder,input,size/2,size+512,view,w,h,w,w,h,w,h)<0);
    assert(!qpk_mjpeg_decode_region(decoder,input,size,size+512,view,w,h,w,w,h));
    assert(!memcmp(view,full,count*2));
  }
  /* Exercise the actual enlarged USB view coordinates and reduced decoder,
   * including source letterbox removal. The rest of the display is a canary. */
  uint16_t *display=malloc(1024*600*2);assert(display);
  assert(!qpk_mjpeg_preview_limits(decoder,w>>1,h>>1));
  unsigned sw=w>>1,sh=h>>1,dw=896,dh=(uint64_t)sh*896/sw;
  if(dh>600){dh=600;dw=(uint64_t)sw*600/sh;}
  unsigned ox=(896-dw)/2,oy=(600-dh)/2;
  for(unsigned aspect=0;aspect<=4;aspect++)for(unsigned z=0;z<4;z++){
    unsigned zoom=camera_reference_zoom_values[z];
    struct camera_reference_rect crop=camera_reference_crop(896,w,h,zoom,aspect);
    struct camera_reference_rect target=camera_reference_output(896,w,h,zoom,aspect);
    for(unsigned i=0;i<1024*600;i++)display[i]=0xa55a;
    struct qpk_mjpeg_view options={.canvas_width=896,.canvas_height=600,.stride=1024,
      .width=target.w,.height=target.h,.crop_x=crop.x,.crop_y=crop.y,.crop_width=crop.w,.crop_height=crop.h};
    assert(!qpk_mjpeg_decode_preview(decoder,input,size,size+512,display+target.y*1024+target.x,w,h,&options));
    for(unsigned y=0;y<600;y++)for(unsigned x=0;x<1024;x++){
      bool inside=x>=target.x&&x<target.x+target.w&&y>=target.y&&y<target.y+target.h;
      if(!inside){assert(display[y*1024+x]==0xa55a);continue;}
      unsigned cx=crop.x+(x-target.x)*crop.w/target.w,cy=crop.y+(y-target.y)*crop.h/target.h;
      uint16_t expected=0;
      if(cx>=ox&&cx<ox+dw&&cy>=oy&&cy<oy+dh)
        expected=full[(((cy-oy)*sh/dh)*2)*w+((cx-ox)*sw/dw)*2];
      assert(display[y*1024+x]==expected);
    }
  }
  free(display);qpk_mjpeg_destroy(decoder);free(view);free(full);free(input);
  puts("MJPEG_PREVIEW_OK: fused six colours/five aspects across three scales, crop bounds, clipped MCU edges, full-quality photo isolation and truncation");
}
