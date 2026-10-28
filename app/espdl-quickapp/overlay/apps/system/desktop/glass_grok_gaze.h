/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
/* Reposition the original eye pixels, retaining every emotion contour.
 * One scratch allocation per player; the reader owns it, never the UI. */
struct grok_gaze_scratch {
  uint8_t mask[128*128/8];
  struct {uint16_t index,color;} dots[2048];
};
static inline bool grok_eye_mask(const struct grok_gaze_scratch *s,unsigned p)
{return (s->mask[p/8]>>(p%8))&1;}
static inline bool grok_body_pixel(uint16_t v,bool black)
{
#define GROK_RGB565(r,g,b) (((r)>>3)<<11|((g)>>2)<<5|((b)>>3))
  if(black)return v==GROK_RGB565(68,68,68)||v==GROK_RGB565(53,53,53)||
    v==GROK_RGB565(38,38,38)||v==GROK_RGB565(26,26,26)||v==GROK_RGB565(17,17,17);
  return v==GROK_RGB565(247,246,240)||v==GROK_RGB565(235,233,225)||
    v==GROK_RGB565(221,219,210)||v==GROK_RGB565(200,199,189)||v==GROK_RGB565(178,179,172);
#undef GROK_RGB565
}
static inline void grok_gaze_frame(uint8_t *pixels,bool black,unsigned emotion,int gaze_x,int gaze_y,struct grok_gaze_scratch *s)
{
  uint16_t *rgb=(uint16_t *)pixels;uint8_t *alpha=pixels+128*128*2;
  unsigned count=0;int x1=128,x2=0,y1=128,y2=0;
  memset(s->mask,0,sizeof(s->mask));
  /* Follow the opaque body on each row. Speaking/tilted eyes can extend
   * beyond the old fixed circle; clipping that mask left half an old eye
   * beside the translated pair. Detached particles are outside this span. */
  for(int y=8;y<120;y++){
    if(alpha[y*128+64]<220)continue;
    int left=64,right=64;
    while(left>8&&alpha[y*128+left-1]>=220)left--;
    while(right<119&&alpha[y*128+right+1]>=220)right++;
    for(int x=left;x<=right;x++){
      unsigned p=y*128+x,v=rgb[p],r=v>>11,g=(v>>5)&63,b=v&31;
      bool eye=black?(r>26&&g>53&&b>26):(r<7&&g<14&&b<7);
      if(alpha[p]<220||!eye)continue;
      if(count==2048)return; /* Unrecognized palette: leave frame intact. */
      s->dots[count].index=p;s->dots[count++].color=v;s->mask[p/8]|=1u<<(p%8);
      if(x<x1)x1=x;if(x>x2)x2=x;if(y<y1)y1=y;if(y>y2)y2=y;
    }
  }
  if(!count)return; /* Blink: do not invent open eyes. */
  if(gaze_x<-1000)gaze_x=-1000;if(gaze_x>1000)gaze_x=1000;
  if(gaze_y<-1000)gaze_y=-1000;if(gaze_y>1000)gaze_y=1000;
  int dx=64-(x1+x2)/2+gaze_x*8/1000,dy=61-(y1+y2)/2+gaze_y*6/1000;
  if(dx<8-x1)dx=8-x1;if(dx>119-x2)dx=119-x2;
  if(dy<8-y1)dy=8-y1;if(dy>119-y2)dy=119-y2;
  /* The baked body has a radial light at 38% / 32% of its bounds. Copy a
   * clean pixel at the same radius when possible: nearest-row filling could
   * smear a different shade across the old eye and leave a visible scar. */
  int bx1=128,bx2=0,by1=128,by2=0;
  for(int y=0;y<128;y++)for(int x=0;x<128;x++){
    unsigned p=y*128+x;
    if(alpha[p]<220||!grok_body_pixel(rgb[p],black))continue;
    if(x<bx1)bx1=x;if(x>bx2)bx2=x;if(y<by1)by1=y;if(y>by2)by2=y;
  }
  int light_x=bx1+(bx2-bx1)*38/100,light_y=by1+(by2-by1)*32/100;
  for(unsigned i=0;i<count;i++){
    unsigned p=s->dots[i].index;int x=p%128,y=p/128;
    int rx=x-light_x,ry=y-light_y;
    int donors[7][2]={{-rx,ry},{rx,-ry},{-rx,-ry},{ry,rx},{-ry,rx},{ry,-rx},{-ry,-rx}};
    bool filled=false;
    for(unsigned j=0;bx1<=bx2&&j<7;j++){
      int sx=light_x+donors[j][0],sy=light_y+donors[j][1];
      if(sx<0||sx>=128||sy<0||sy>=128)continue;
      unsigned sample=sy*128+sx;
      if(alpha[sample]>=220&&!grok_eye_mask(s,sample)&&grok_body_pixel(rgb[sample],black)){
        rgb[p]=rgb[sample];filled=true;break;
      }
    }
    if(filled)continue;
    for(int distance=1;distance<96;distance++){
      int a=x-distance,b=x+distance;
      if(a>=8&&alpha[y*128+a]>=220&&!grok_eye_mask(s,y*128+a)){rgb[p]=rgb[y*128+a];break;}
      if(b<120&&alpha[y*128+b]>=220&&!grok_eye_mask(s,y*128+b)){rgb[p]=rgb[y*128+b];break;}
    }
  }
  if(emotion==2){
    /* The upstream idle contour is baked in perspective. A neutral frontal
     * pose needs equal-size, upright eyes, not just a translated side view.
     * Keep the source blink height and palette; gaze moves both equally. */
    int height=y2-y1+1;if(height>16)height=16;if(height<2)height=2;
    int cx=64+gaze_x*8/1000,cy=61+gaze_y*6/1000;
    for(int eye=-1;eye<=1;eye+=2)for(int row=0;row<height;row++){
      int inset=height>6&&(row==0||row==height-1)?2:height>6&&(row==1||row==height-2)?1:0;
      for(int col=inset;col<6-inset;col++){
        int x=cx+eye*10-3+col,y=cy-height/2+row;
        if(alpha[y*128+x]>=220)rgb[y*128+x]=s->dots[0].color;
      }
    }
    return;
  }
  for(unsigned i=0;i<count;i++){
    int p=s->dots[i].index,x=p%128+dx,y=p/128+dy;
    if(x>=8&&x<120&&y>=8&&y<120&&alpha[y*128+x]>=220)rgb[y*128+x]=s->dots[i].color;
  }
}
