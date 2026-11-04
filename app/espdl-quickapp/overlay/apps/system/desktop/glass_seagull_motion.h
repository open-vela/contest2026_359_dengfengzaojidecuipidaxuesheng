/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <math.h>

/* Small, bounded gestures shared by the nest and floating bird. Coordinates
 * are in the original 128 px sprite; no allocation, disk IO or extra timer. */
enum seagull_motion_e {
  SEAGULL_IDLE, SEAGULL_TILT, SEAGULL_SHAKE, SEAGULL_STRETCH,
  SEAGULL_PECK, SEAGULL_HOP, SEAGULL_FLAP, SEAGULL_DOZE,
  SEAGULL_FLY, SEAGULL_ROLL_FORWARD, SEAGULL_ROLL_BACKWARD,
  SEAGULL_MOTION_COUNT
};
#define SEAGULL_MOTION_MS 2400u
#define SEAGULL_MOTION_FRAME_MS 50u
struct seagull_pose {
  int angle, x, y, sx, sy;
  unsigned emotion, index;
};

static inline struct seagull_pose seagull_motion_pose(unsigned motion,
                                                     unsigned elapsed)
{
  /* angle in tenths of a degree; scale deltas in 1/256 units. */
  static const short keys[7][7][6] = {
    {{0,0,0,0,0,2},{-70,0,0,0,0,3},{-70,0,0,0,0,3},
     {0,0,0,0,0,2},{60,0,0,0,0,11},{60,0,0,0,0,11},{0,0,0,0,0,2}},
    {{0,0,0,0,0,2},{-35,-2,0,0,0,7},{35,2,0,0,0,7},
     {-35,-2,0,0,0,7},{35,2,0,0,0,7},{0,0,0,0,0,36},{0,0,0,0,0,2}},
    {{0,0,0,0,0,2},{0,0,2,8,-10,1},{0,0,-3,-8,14,33},
     {0,0,-3,-8,14,33},{0,0,-2,-4,8,10},{0,0,0,0,0,19},{0,0,0,0,0,2}},
    {{0,0,0,0,0,2},{0,0,3,8,-14,16},{0,0,0,0,0,19},
     {0,0,3,8,-14,16},{0,0,0,0,0,19},{0,0,2,6,-8,16},{0,0,0,0,0,2}},
    {{0,0,0,0,0,2},{0,0,2,10,-12,10},{0,0,-9,-6,8,10},
     {0,0,2,10,-12,19},{0,0,-6,-4,6,10},{0,0,1,4,-6,19},{0,0,0,0,0,2}},
    {{0,0,0,0,0,2},{-20,0,-2,0,0,31},{0,0,-4,0,0,33},
     {20,0,-2,0,0,10},{0,0,-4,0,0,33},{0,0,-1,0,0,31},{0,0,0,0,0,2}},
    {{0,0,0,0,0,2},{0,0,1,4,-4,6},{0,0,2,6,-8,0},
     {-20,0,2,6,-8,0},{0,0,1,4,-4,0},{0,0,0,0,0,1},{0,0,0,0,0,2}}
  };
  static const unsigned emotions[32] = {
    0,1,2,3,4,5,6,7,10,11,12,13,14,15,16,17,
    18,19,20,21,30,31,32,33,34,35,36,37,38,39,40,41
  };
  struct seagull_pose p = {0,0,0,256,256,2,2};
  if (motion == SEAGULL_IDLE || motion >= SEAGULL_MOTION_COUNT ||
      elapsed >= SEAGULL_MOTION_MS) return p;
  if (motion >= SEAGULL_FLY) {
    /* Anticipation and recovery bracket the aerial phase. Pitch is shown
     * through front/rear silhouettes and foreshortening, not a flat spin. */
    if(elapsed<300 || elapsed>=1900){
      float t=elapsed<300?elapsed/300.0f:(2400-elapsed)/500.0f;
      float squash=sinf(t*3.14159265f);
      p.sx+=squash*16;p.sy-=squash*20;p.y=squash*3;
      return p;
    }
    float t=(elapsed-300)/1600.0f;
    float arc=sinf(t*3.14159265f);
    float ease=t*t*(3.0f-2.0f*t);
    if(motion==SEAGULL_FLY){
      p.y=-28*arc;p.x=10*sinf(t*6.2831853f)*arc;
      p.angle=45*sinf(t*6.2831853f)*arc;
      unsigned wing=(elapsed-300)%400;
      p.emotion=wing<150?33:wing<250?31:10;
      p.index=wing<150?23:wing<250?21:8;
      p.sy+=6*arc;p.sx-=4*arc;
    }else{
      float pitch=ease*6.2831853f;
      float projection=cosf(pitch);
      int forward=motion==SEAGULL_ROLL_FORWARD?1:-1;
      p.y=-16*arc+forward*9*arc;
      p.sx=256+forward*20*arc;
      p.sy=(int)(fabsf(projection)*p.sx);
      if(p.sy<16)p.sy=16;
      /* Back faces the viewer only during the inverted middle half. */
      if(projection<0){p.index=32;p.angle=1800;}
      else {p.index=2;p.emotion=2;}
      /* The two directions expose the crown/feet in opposite order. */
      p.y+=forward*7*sinf(pitch);
    }
    return p;
  }
  unsigned key = elapsed / 400, t = elapsed % 400;
  const short *a = keys[motion-1][key], *b = keys[motion-1][key+1];
  /* Smoothstep keeps velocity zero at each gesture's resting pose. */
  int u = (int)(t * t * (1200 - 2*t) / 160000);
  p.angle=a[0]+(b[0]-a[0])*u/400;
  p.x=a[1]+(b[1]-a[1])*u/400;
  p.y=a[2]+(b[2]-a[2])*u/400;
  p.sx+=a[3]+(b[3]-a[3])*u/400;
  p.sy+=a[4]+(b[4]-a[4])*u/400;
  p.emotion=(unsigned)(t < 200 ? a[5] : b[5]);
  for(unsigned i=0;i<32;i++)if(emotions[i]==p.emotion){p.index=i;break;}
  return p;
}
