/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_seagull.h"
#include <errno.h>
#include <string.h>

static int unpack(const uint8_t *input,size_t bytes,uint8_t *output,
                  unsigned side,unsigned unit)
{
  size_t offset=0;unsigned position=0;
  const unsigned total=SEAGULL_SIDE*SEAGULL_SIDE;
  const unsigned step=SEAGULL_SIDE/side;
  while(offset<bytes&&position<total){
    unsigned tag=input[offset++],count=(tag&127u)+1u;
    size_t needed=(tag&128u)?unit:count*unit;
    if(count>total-position||needed>bytes-offset)return -EINVAL;
    if(side==SEAGULL_SIDE&&!(tag&128u)){
      memcpy(output+position*unit,input+offset,needed);
    }else{
      for(unsigned i=0;i<count;i++){
        unsigned source=position+i,row=source/SEAGULL_SIDE,col=source%SEAGULL_SIDE;
        if(row%step==0&&col%step==0){
          unsigned target=(row/step)*side+col/step;
          memcpy(output+target*unit,input+offset+((tag&128u)?0:i*unit),unit);
        }
      }
    }
    position+=count;offset+=needed;
  }
  return position==total&&offset==bytes?0:-EINVAL;
}

int glass_seagull_decode(unsigned index,uint8_t *pixels,unsigned side)
{
  if(!pixels||index>=SEAGULL_PACKED_COUNT||(side!=64&&side!=128))return -EINVAL;
  const struct seagull_packed_frame *f=&seagull_packed_frames[index];
  int ret=unpack(f->rgb,f->rgb_bytes,pixels,side,2);
  return ret?ret:unpack(f->alpha,f->alpha_bytes,pixels+side*side*2,side,1);
}

unsigned glass_seagull_yaw(int x)
{
  if(x<-1000)x=-1000;
  if(x>1000)x=1000;
  return (unsigned)((x*60/1000+60+7)/15);
}

int glass_seagull_render(uint8_t *pixels,unsigned side,unsigned emotion,
                        unsigned frame,bool gaze,int gaze_x,int gaze_y)
{
  static const unsigned ids[]={0,1,2,3,4,5,6,7,10,11,12,13,14,15,16,17,18,19,20,21,30,31,32,33,34,35,36,37,38,39,40,41};
  static const uint8_t turn[]={4,4,4,3,2,1,0,0,0,0,1,2,3,4,4,4,5,6,7,8,8,8,8,7,6,5,4,4,4,4};
  unsigned index=2;
  (void)gaze_y;
  if(emotion==42)index=SEAGULL_YAW_BASE+turn[frame%(sizeof(turn)/sizeof(*turn))];
  else if(emotion>=43&&emotion<=51)index=SEAGULL_YAW_BASE+(emotion-43)%3*4;
  else if(emotion==2&&gaze)index=SEAGULL_YAW_BASE+glass_seagull_yaw(gaze_x);
  else {
    for(unsigned i=0;i<sizeof(ids)/sizeof(*ids);i++)if(ids[i]==emotion){index=i;break;}
    /* Brief closed-eye blink; service expressions retain their own artwork. */
    if(emotion==2&&frame%80>=72&&frame%80<=73)index=0;
  }
  return glass_seagull_decode(index,pixels,side);
}
