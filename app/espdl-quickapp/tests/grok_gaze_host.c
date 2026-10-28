/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_grok_gaze.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static uint8_t raw[128*128*3],frame[sizeof(raw)];
static struct grok_gaze_scratch scratch;
static bool eye(uint16_t v,bool black)
{
  unsigned r=v>>11,g=(v>>5)&63,b=v&31;
  return black?(r>26&&g>53&&b>26):(r<7&&g<14&&b<7);
}
static void edge_case(bool black)
{
  uint16_t *rgb=(uint16_t *)frame;uint8_t *alpha=frame+32768;
  memset(frame,0,sizeof(frame));
  for(int y=16;y<112;y++)for(int x=16;x<112;x++)
    if((x-64)*(x-64)+(y-64)*(y-64)<48*48){rgb[y*128+x]=black?0x2124:0xef5c;alpha[y*128+x]=255;}
  /* Both source eyes lie outside the previous 44-pixel mask. */
  rgb[88*128+104]=rgb[89*128+103]=black?0xf7be:0x18c3;
  grok_gaze_frame(frame,black,39,0,0,&scratch);
  assert(!eye(rgb[88*128+104],black)&&!eye(rgb[89*128+103],black));
  assert(eye(rgb[61*128+65],black)&&eye(rgb[62*128+64],black));
}
int main(int argc,char **argv)
{
  assert(argc==2);edge_case(false);edge_case(true);unsigned frames=0,cleared=0;
  for(unsigned black=0;black<2;black++){
    char path[1024];snprintf(path,sizeof(path),"%s/app-%s.gbs",argv[1],black?"black":"white");
    FILE *f=fopen(path,"rb");assert(f);uint32_t header[8],table[42][3];
    assert(fread(header,1,32,f)==32);assert(header[6]==42);
    assert(fread(table,12,42,f)==42);
    for(unsigned state=0;state<32;state++)for(unsigned n=0;n<table[state][1];n++){
      assert(!fseek(f,table[state][2]+n*sizeof(raw),SEEK_SET));assert(fread(raw,1,sizeof(raw),f)==sizeof(raw));
      for(int gaze=-1000;gaze<=1000;gaze+=1000){
        memcpy(frame,raw,sizeof(raw));grok_gaze_frame(frame,black,table[state][0],gaze,-gaze,&scratch);
        assert(!memcmp(frame+32768,raw+32768,16384));
        unsigned count=0;int x1=128,x2=0,y1=128,y2=0;
        for(int y=0;y<128;y++)for(int x=0;x<128;x++)if(grok_eye_mask(&scratch,y*128+x)){
          count++;if(x<x1)x1=x;if(x>x2)x2=x;if(y<y1)y1=y;if(y>y2)y2=y;
        }
        if(count&&count<2048&&table[state][0]!=2){
          int dx=64-(x1+x2)/2+gaze*8/1000,dy=61-(y1+y2)/2-gaze*6/1000;
          if(dx<8-x1)dx=8-x1;if(dx>119-x2)dx=119-x2;
          if(dy<8-y1)dy=8-y1;if(dy>119-y2)dy=119-y2;
          for(int y=0;y<128;y++)for(int x=0;x<128;x++){
            unsigned p=y*128+x;if(!grok_eye_mask(&scratch,p))continue;
            int sx=x-dx,sy=y-dy;
            if(sx>=0&&sx<128&&sy>=0&&sy<128&&grok_eye_mask(&scratch,sy*128+sx))continue;
            assert(!eye(((uint16_t *)frame)[p],black));cleared++;
          }
        }
        frames++;
      }
    }
    fclose(f);
  }
  printf("GROK_GAZE_OK frames=%u cleared_old_eye_pixels=%u; both palettes; extreme gaze; alpha preserved\n",frames,cleared);
  return 0;
}
