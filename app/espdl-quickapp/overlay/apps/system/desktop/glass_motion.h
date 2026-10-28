/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <lvgl/lvgl.h>
#include <string.h>
#include "glass_icons.h"

/* Native motion tokens: short tactile response, slower spatial arrival.
 * Hermite interpolation retains fractional position and velocity when a target
 * changes. End velocity is zero. Fixed-point interpolation needs no per-frame
 * allocation, transforms, group opacity, image snapshots or float solver.
 * https://github.com/material-components/material-components-android/blob/master/docs/theming/Motion.md
 * UI thread only. A paused shared timer and 12 slots bound all retained work. */
#define GLASS_MOTION_SLOTS 12
/* Match the configured display cadence (33 ms on this board). Updating a
 * hidden intermediate position twice per refresh only wastes UI work. */
#define GLASS_MOTION_TICK (LV_DEF_REFR_PERIOD<16?16:(LV_DEF_REFR_PERIOD>33?33:LV_DEF_REFR_PERIOD))
#define GLASS_MOTION_FIXED 4096
#define GLASS_MOTION_PRESS 80
#define GLASS_MOTION_RELEASE 180
#define GLASS_MOTION_ENTER 240
#define GLASS_MOTION_NOTICE 200
#define GLASS_MOTION_EXIT 140
#define GLASS_MOTION_PAGE 160
#define GLASS_MOTION_DRAWER 260
enum glass_motion_kind { MOTION_PRESS, MOTION_X, MOTION_Y, MOTION_NOTICE, MOTION_RECT, MOTION_COLOR, MOTION_RANGE, MOTION_BATCH, MOTION_SCRIM };
struct glass_motion_job {
  lv_obj_t *object,*range_thumb;
  void (*done)(lv_obj_t *);
  uint32_t started;
  unsigned duration,delay;
  int from,to,value,base_offset;
  lv_color_t base_color,last_color;
  lv_style_value_t saved_color,saved_pressed,saved_offset;
  lv_opa_t bg_opa,text_opa;
  enum glass_motion_kind kind;
  bool had_color,had_pressed,had_offset,painted;
  int64_t start[4],position[4],velocity[4],initial_velocity[4];
  int target[4];unsigned geometry_mask;
};
static struct glass_motion_job g_motion_jobs[GLASS_MOTION_SLOTS];
#define GLASS_MOTION_BATCH_LIMIT 128
static struct {
  lv_obj_t *object;lv_style_value_t saved;bool local;
} g_motion_members[GLASS_MOTION_BATCH_LIMIT];
static unsigned g_motion_member_count;
static lv_style_prop_t g_motion_batch_prop=LV_STYLE_TRANSLATE_Y;
static lv_timer_t *g_motion_timer;
static bool g_motion_reduced;
static void glass_motion_page_ready(void *object);
static int glass_motion_round(int64_t value)
{return (int)(value>=0?(value+GLASS_MOTION_FIXED/2)/GLASS_MOTION_FIXED:
                     -((-value+GLASS_MOTION_FIXED/2)/GLASS_MOTION_FIXED));}

static void glass_motion_idle(void)
{
  for(unsigned i=0;i<GLASS_MOTION_SLOTS;i++)if(g_motion_jobs[i].object)return;
  if(g_motion_timer)lv_timer_pause(g_motion_timer);
}
static void glass_motion_rebase(struct glass_motion_job *job)
{
  if(job->kind!=MOTION_PRESS||!job->painted)return;
  lv_style_value_t color;
  if(lv_obj_get_local_style_prop(job->object,LV_STYLE_BG_COLOR,&color,0)==LV_STYLE_RES_FOUND&&
     lv_color_to_u32(color.color)!=lv_color_to_u32(job->last_color)){
    /* A device response or app callback can change the semantic fill during
     * feedback. Adopt it; release must never restore the previous device state. */
    job->saved_color=color;job->had_color=true;job->base_color=color.color;
  }
}
static void glass_motion_batch_clear(lv_obj_t *deleted)
{
  for(unsigned i=0;i<g_motion_member_count;i++){
    lv_obj_t *obj=g_motion_members[i].object;if(!obj||obj==deleted)continue;
    if(g_motion_members[i].local)lv_obj_set_local_style_prop(obj,g_motion_batch_prop,g_motion_members[i].saved,0);
    else lv_obj_remove_local_style_prop(obj,g_motion_batch_prop,0);
  }
  memset(g_motion_members,0,sizeof(g_motion_members));g_motion_member_count=0;
}
static void glass_motion_range_paint(struct glass_motion_job *job,int stop)
{
  if(stop<0)stop=0;
  if(stop>255)stop=255;
  lv_obj_set_style_bg_color(job->object,stop?job->base_color:job->last_color,0);
  lv_obj_set_style_bg_grad_color(job->object,stop==255?job->base_color:job->last_color,0);
  lv_obj_set_style_bg_main_stop(job->object,stop,0);
  lv_obj_set_style_bg_grad_stop(job->object,stop,0);
  if(job->range_thumb&&lv_obj_get_parent(job->range_thumb)==lv_obj_get_parent(job->object)){
    int w=lv_obj_get_style_width(job->object,0),h=lv_obj_get_style_height(job->object,0);
    int th=lv_obj_get_style_height(job->range_thumb,0);
    int64_t x=(int64_t)lv_obj_get_style_x(job->object,0)+12+(((int64_t)w-30)*stop+127)/255;
    int64_t y=(int64_t)lv_obj_get_style_y(job->object,0)+((int64_t)h-th)/2;
    if(w>=30&&w<=4096&&h>=1&&h<=4096&&th>=1&&th<=4096&&x>=INT32_MIN&&x<=INT32_MAX&&y>=INT32_MIN&&y<=INT32_MAX)
      lv_obj_set_pos(job->range_thumb,(int)x,(int)y);
  }
}
static void glass_motion_restore(struct glass_motion_job *job)
{
  glass_motion_rebase(job);
  lv_obj_t *obj=job->object;
  if(job->kind==MOTION_BATCH){glass_motion_batch_clear(NULL);return;}
  if(job->kind==MOTION_RANGE){glass_motion_range_paint(job,job->to);return;}
  if(job->kind==MOTION_SCRIM){lv_obj_set_style_bg_opa(obj,(unsigned)job->bg_opa*job->to/1024,0);return;}
  if(job->kind==MOTION_COLOR){
    lv_obj_set_style_bg_color(obj,job->last_color,0);return;
  }
  if(job->kind==MOTION_RECT){
    if(job->geometry_mask&1)lv_obj_set_pos(obj,job->target[0],job->target[1]);
    if(job->geometry_mask&2)lv_obj_set_size(obj,job->target[2],job->target[3]);
    return;
  }
  lv_style_prop_t offset=job->kind==MOTION_X?LV_STYLE_TRANSLATE_X:LV_STYLE_TRANSLATE_Y;
  if(job->had_offset)lv_obj_set_local_style_prop(obj,offset,job->saved_offset,0);
  else lv_obj_remove_local_style_prop(obj,offset,0);
  if(job->kind==MOTION_PRESS){
    if(job->had_color)lv_obj_set_local_style_prop(obj,LV_STYLE_BG_COLOR,job->saved_color,0);
    else lv_obj_remove_local_style_prop(obj,LV_STYLE_BG_COLOR,0);
    if(job->had_pressed)lv_obj_set_local_style_prop(obj,LV_STYLE_BG_COLOR,job->saved_pressed,LV_STATE_PRESSED);
    else lv_obj_remove_local_style_prop(obj,LV_STYLE_BG_COLOR,LV_STATE_PRESSED);
  }else if(job->kind==MOTION_NOTICE){
    lv_obj_set_style_bg_opa(obj,job->bg_opa,0);
    lv_obj_set_style_text_opa(obj,job->text_opa,0);
  }
}
static void glass_motion_finish(struct glass_motion_job *job,bool complete)
{
  lv_obj_t *obj=job->object;void (*done)(lv_obj_t *)=job->done;
  glass_motion_restore(job);memset(job,0,sizeof(*job));
  if(complete&&done)done(obj); /* Callback may delete its page and other jobs. */
}
static void glass_motion_deleted(lv_event_t *event)
{
  lv_obj_t *obj=lv_event_get_target(event);
  lv_async_call_cancel(glass_motion_page_ready,obj);
  for(unsigned i=0;i<g_motion_member_count;i++)if(g_motion_members[i].object==obj)g_motion_members[i].object=NULL;
  for(unsigned i=0;i<GLASS_MOTION_SLOTS;i++)if(g_motion_jobs[i].range_thumb==obj)g_motion_jobs[i].range_thumb=NULL;
  for(unsigned i=0;i<GLASS_MOTION_SLOTS;i++)if(g_motion_jobs[i].object==obj){
    if(g_motion_jobs[i].kind==MOTION_BATCH)glass_motion_batch_clear(obj);
    memset(&g_motion_jobs[i],0,sizeof(g_motion_jobs[i]));
  }
  glass_motion_idle();
}
static void glass_motion_watch(lv_obj_t *obj)
{
  for(unsigned i=0;i<lv_obj_get_event_count(obj);i++)
    if(lv_event_dsc_get_cb(lv_obj_get_event_dsc(obj,i))==glass_motion_deleted)return;
  lv_obj_add_event_cb(obj,glass_motion_deleted,LV_EVENT_DELETE,NULL);
}
static void glass_motion_apply(struct glass_motion_job *job,int value)
{
  lv_obj_t *obj=job->object;job->value=value;
  if(job->kind==MOTION_BATCH){
    for(unsigned i=0;i<g_motion_member_count;i++)if(g_motion_members[i].object){
      lv_style_value_t offset={.num=(g_motion_members[i].local?g_motion_members[i].saved.num:0)+value};
      lv_obj_set_local_style_prop(g_motion_members[i].object,g_motion_batch_prop,offset,0);
    }
  }else if(job->kind==MOTION_SCRIM){
    if(value<0)value=0;if(value>1024)value=1024;
    lv_obj_set_style_bg_opa(obj,(unsigned)job->bg_opa*value/1024,0);
  }else if(job->kind==MOTION_RANGE)glass_motion_range_paint(job,value);
  else if(job->kind==MOTION_RECT){
    if(job->geometry_mask&1)lv_obj_set_pos(obj,glass_motion_round(job->position[0]),glass_motion_round(job->position[1]));
    if(job->geometry_mask&2){
      int w=glass_motion_round(job->position[2]),h=glass_motion_round(job->position[3]);
      lv_obj_set_size(obj,w<1?1:w,h<1?1:h);
    }
  }else if(job->kind==MOTION_COLOR){
    if(value<0)value=0;if(value>1024)value=1024;
    lv_obj_set_style_bg_color(obj,lv_color_mix(job->last_color,job->base_color,value*255/1024),0);
  }else if(job->kind==MOTION_PRESS){
    glass_motion_rebase(job);
    lv_color_t ink=lv_color_luminance(job->base_color)>128?lv_color_black():lv_color_white();
    lv_color_t fill=lv_color_mix(ink,job->base_color,value*22/1024);
    lv_obj_set_style_bg_color(obj,fill,0);
    lv_obj_set_style_bg_color(obj,fill,LV_STATE_PRESSED);
    job->last_color=fill;job->painted=true;
    /* Press feedback changes color only; no decorative floating geometry. */
  }else if(job->kind==MOTION_NOTICE){
    /* This object is a label: animate its primitive fills, never group alpha. */
    lv_obj_set_style_bg_opa(obj,(unsigned)job->bg_opa*value/1024,0);
    lv_obj_set_style_text_opa(obj,(unsigned)job->text_opa*value/1024,0);
  }else if(job->kind==MOTION_X)lv_obj_set_style_translate_x(obj,job->base_offset+value,0);
  else lv_obj_set_style_translate_y(obj,job->base_offset+value,0);
}
static void glass_motion_evaluate(struct glass_motion_job *job,unsigned elapsed)
{
  /* Q16 Hermite basis: p(0), p'(0), p(1), p'(1)=0. Preserve C1 continuity
   * across a retarget instead of restarting an easing curve from rest. */
  int64_t s=(int64_t)elapsed*65536/job->duration;
  int64_t s2=s*s/65536,s3=s2*s/65536;
  int64_t h01=3*s2-2*s3,h10=s-2*s2+s3;
  unsigned count=job->kind==MOTION_RECT?4:1;
  for(unsigned i=0;i<count;i++){
    int64_t delta=(int64_t)job->target[i]*GLASS_MOTION_FIXED-job->start[i];
    job->position[i]=job->start[i]+delta*h01/65536+
      job->initial_velocity[i]*job->duration*h10/65536;
    job->velocity[i]=delta*(6*s-6*s2)/(65536*job->duration)+
      job->initial_velocity[i]*(65536-4*s+3*s2)/65536;
  }
  int value=glass_motion_round(job->position[0]);
  if(job->kind==MOTION_PRESS||job->kind==MOTION_NOTICE){if(value<0)value=0;if(value>1024)value=1024;}
  glass_motion_apply(job,value);
}
static void glass_motion_tick(lv_timer_t *timer)
{
  LV_UNUSED(timer);uint32_t now=lv_tick_get();
  for(unsigned i=0;i<GLASS_MOTION_SLOTS;i++){
    struct glass_motion_job *job=&g_motion_jobs[i];if(!job->object)continue;
    uint32_t elapsed=now-job->started;if(elapsed<job->delay)continue;elapsed-=job->delay;
    if(elapsed>=job->duration){
      for(unsigned channel=0;channel<4;channel++){
        job->position[channel]=(int64_t)job->target[channel]*GLASS_MOTION_FIXED;
        job->velocity[channel]=0;
      }
      glass_motion_apply(job,job->to);
      /* Keep a held button's final state until release, without a live timer. */
      if(job->kind==MOTION_PRESS&&job->to==1024)continue;
      glass_motion_finish(job,true);continue;
    }
    glass_motion_evaluate(job,elapsed);
  }
  bool running=false;
  for(unsigned i=0;i<GLASS_MOTION_SLOTS;i++)if(g_motion_jobs[i].object&&
    !(g_motion_jobs[i].kind==MOTION_PRESS&&g_motion_jobs[i].to==1024&&
      now-g_motion_jobs[i].started>=g_motion_jobs[i].duration))running=true;
  if(!running&&g_motion_timer)lv_timer_pause(g_motion_timer);
}
static struct glass_motion_job *glass_motion_begin(lv_obj_t *obj,enum glass_motion_kind kind,
                                                   int from,int to,unsigned duration,unsigned delay)
{
  if(!obj||g_motion_reduced)return NULL;
  struct glass_motion_job *job=NULL;bool fresh=false;
  for(unsigned i=0;i<GLASS_MOTION_SLOTS;i++)if(g_motion_jobs[i].object==obj){
    /* Decorative geometry, fill and batch translation are independent. */
    if(kind==MOTION_BATCH||g_motion_jobs[i].kind==MOTION_BATCH||
       (kind==MOTION_RECT&&(g_motion_jobs[i].kind==MOTION_COLOR||g_motion_jobs[i].kind==MOTION_RANGE))||
       ((kind==MOTION_COLOR||kind==MOTION_RANGE)&&g_motion_jobs[i].kind==MOTION_RECT))continue;
    job=&g_motion_jobs[i];
    if(job->kind==kind){
      if(kind!=MOTION_RECT&&job->to==to)return job;
      from=job->value;
    }
    else {glass_motion_finish(job,false);job=NULL;}
    break;
  }
  if(!job){
    for(unsigned i=0;i<GLASS_MOTION_SLOTS;i++)if(!g_motion_jobs[i].object){job=&g_motion_jobs[i];break;}
    if(!job)return NULL; /* Saturation leaves the existing functional UI intact. */
    memset(job,0,sizeof(*job));job->object=obj;job->kind=kind;
    fresh=true;
    lv_style_prop_t offset=kind==MOTION_X?LV_STYLE_TRANSLATE_X:LV_STYLE_TRANSLATE_Y;
    job->had_offset=lv_obj_get_local_style_prop(obj,offset,&job->saved_offset,0)==LV_STYLE_RES_FOUND;
    job->base_offset=kind==MOTION_X?lv_obj_get_style_translate_x(obj,0):lv_obj_get_style_translate_y(obj,0);
    if(kind==MOTION_PRESS){
      job->base_color=lv_obj_get_style_bg_color(obj,0);
      job->had_color=lv_obj_get_local_style_prop(obj,LV_STYLE_BG_COLOR,&job->saved_color,0)==LV_STYLE_RES_FOUND;
      job->had_pressed=lv_obj_get_local_style_prop(obj,LV_STYLE_BG_COLOR,&job->saved_pressed,LV_STATE_PRESSED)==LV_STYLE_RES_FOUND;
      if(job->had_color)job->base_color=job->saved_color.color;
    }
    job->bg_opa=lv_obj_get_style_bg_opa(obj,0);job->text_opa=lv_obj_get_style_text_opa(obj,0);
    job->position[0]=(int64_t)from*GLASS_MOTION_FIXED;
    glass_motion_watch(obj);
  }
  if(kind!=MOTION_RECT){
    job->start[0]=job->position[0];job->initial_velocity[0]=job->velocity[0];job->target[0]=to;
    /* Spatial arrival starts moving immediately, then decelerates. A retarget
     * still inherits its measured velocity instead of restarting the curve. */
    if(fresh&&duration&&(kind==MOTION_X||kind==MOTION_Y||kind==MOTION_BATCH||kind==MOTION_SCRIM))
      job->initial_velocity[0]=((int64_t)to*GLASS_MOTION_FIXED-job->start[0])*3/duration;
  }
  job->from=from;job->to=to;job->duration=duration;job->delay=delay;job->started=lv_tick_get();job->done=NULL;
  if(!g_motion_timer)g_motion_timer=lv_timer_create(glass_motion_tick,GLASS_MOTION_TICK,NULL);
  if(!g_motion_timer){glass_motion_finish(job,false);return NULL;}
  if(kind!=MOTION_RECT&&kind!=MOTION_COLOR&&kind!=MOTION_RANGE&&kind!=MOTION_BATCH)glass_motion_apply(job,from);
  lv_timer_resume(g_motion_timer);return job;
}
/* Decorative fills only: all pager nodes share one 160 ms transition. */
static bool glass_motion_color(lv_obj_t *obj,uint32_t color,bool animate)
{
  lv_color_t target=lv_color_hex(color),current=lv_obj_get_style_bg_color(obj,0);
  struct glass_motion_job *old=NULL;
  for(unsigned i=0;i<GLASS_MOTION_SLOTS;i++)
    if(g_motion_jobs[i].object==obj&&g_motion_jobs[i].kind==MOTION_COLOR)old=&g_motion_jobs[i];
  bool allowed=animate&&!g_motion_reduced&&!lv_obj_has_flag(obj,LV_OBJ_FLAG_CLICKABLE);
  if(old){
    if(allowed&&lv_color_to_u32(old->last_color)==lv_color_to_u32(target))return true;
    /* Retarget from the currently painted color without committing the old goal. */
    memset(old,0,sizeof(*old));
  }
  if(!allowed||lv_color_to_u32(current)==lv_color_to_u32(target)){
    glass_motion_idle();return false;
  }
  struct glass_motion_job *job=glass_motion_begin(obj,MOTION_COLOR,0,1024,160,0);
  if(!job)return false;
  job->base_color=current;job->last_color=target;glass_motion_apply(job,0);return true;
}
static bool glass_motion_geometry(lv_obj_t *obj,int x,int y,int w,int h,unsigned mask,bool animate)
{
  struct glass_motion_job *old=NULL;
  for(unsigned i=0;i<GLASS_MOTION_SLOTS;i++)if(g_motion_jobs[i].object==obj&&g_motion_jobs[i].kind==MOTION_RECT)old=&g_motion_jobs[i];
  if(!animate||g_motion_reduced||lv_obj_has_flag(obj,LV_OBJ_FLAG_CLICKABLE)||
     x < -4096||x>4096||y < -4096||y>4096||w<1||w>4096||h<1||h>4096){
    if(old)glass_motion_finish(old,false);
    glass_motion_idle();return false;
  }
  lv_obj_update_layout(obj);
  int goals[4]={x,y,w,h};
  int current[4]={lv_obj_get_x(obj),lv_obj_get_y(obj),lv_obj_get_width(obj),lv_obj_get_height(obj)};
  for(unsigned i=0;i<4;i++)if(!(mask&(i<2?1:2)))goals[i]=old?old->target[i]:current[i];
  if(old&&memcmp(old->target,goals,sizeof(goals))==0)return true;
  if(!old&&memcmp(current,goals,sizeof(goals))==0)return false;
  struct glass_motion_job *job=glass_motion_begin(obj,MOTION_RECT,0,0,GLASS_MOTION_ENTER,0);
  if(!job)return false;
  for(unsigned i=0;i<4;i++){
    if(!old){job->position[i]=(int64_t)current[i]*GLASS_MOTION_FIXED;job->velocity[i]=0;}
    job->start[i]=job->position[i];job->initial_velocity[i]=job->velocity[i];job->target[i]=goals[i];
  }
  job->geometry_mask|=mask;
  glass_motion_apply(job,0);return true;
}
static void glass_motion_present_axis(lv_obj_t **objects,unsigned count,int offset,bool horizontal)
{
  for(unsigned i=0;i<GLASS_MOTION_SLOTS;i++)if(g_motion_jobs[i].object&&g_motion_jobs[i].kind==MOTION_BATCH)
    glass_motion_finish(&g_motion_jobs[i],false);
  if(g_motion_reduced||!count||count>GLASS_MOTION_BATCH_LIMIT||offset < -24||offset>24){glass_motion_idle();return;}
  g_motion_batch_prop=horizontal?LV_STYLE_TRANSLATE_X:LV_STYLE_TRANSLATE_Y;
  for(unsigned i=0;i<count;i++){
    lv_obj_t *obj=objects[i];
    if(!obj||lv_obj_has_flag(obj,LV_OBJ_FLAG_CLICKABLE)||lv_obj_has_flag(obj,LV_OBJ_FLAG_HIDDEN))continue;
    bool duplicate=false;for(unsigned j=0;j<g_motion_member_count;j++)if(g_motion_members[j].object==obj)duplicate=true;
    if(duplicate)continue;
    unsigned slot=g_motion_member_count++;g_motion_members[slot].object=obj;
    g_motion_members[slot].local=lv_obj_get_local_style_prop(obj,g_motion_batch_prop,&g_motion_members[slot].saved,0)==LV_STYLE_RES_FOUND;
    glass_motion_watch(obj);
  }
  if(!g_motion_member_count){glass_motion_idle();return;}
  struct glass_motion_job *job=glass_motion_begin(g_motion_members[0].object,MOTION_BATCH,offset,0,GLASS_MOTION_ENTER,0);
  if(!job){glass_motion_batch_clear(NULL);return;}
  glass_motion_apply(job,offset);
}
static void glass_motion_present(lv_obj_t **objects,unsigned count,int offset)
{glass_motion_present_axis(objects,count,offset,false);}
static bool glass_motion_range(lv_obj_t *obj,int stop,uint32_t accent,uint32_t background,bool animate)
{
  if(stop<0||stop>255||lv_obj_has_flag(obj,LV_OBJ_FLAG_CLICKABLE))return false;
  struct glass_motion_job *old=NULL;
  for(unsigned i=0;i<GLASS_MOTION_SLOTS;i++)if(g_motion_jobs[i].object==obj&&g_motion_jobs[i].kind==MOTION_RANGE)old=&g_motion_jobs[i];
  if(!animate||g_motion_reduced){if(old)glass_motion_finish(old,false);glass_motion_idle();return false;}
  if(old&&old->to==stop&&lv_color_to_u32(old->base_color)==lv_color_to_u32(lv_color_hex(accent))&&lv_color_to_u32(old->last_color)==lv_color_to_u32(lv_color_hex(background)))return true;
  int from=lv_obj_get_style_bg_main_stop(obj,0);
  if(!old&&from==stop)return false;
  /* A changed palette must not be mistaken for an unchanged scalar target. */
  if(old&&old->to==stop){old->base_color=lv_color_hex(accent);old->last_color=lv_color_hex(background);return true;}
  struct glass_motion_job *job=glass_motion_begin(obj,MOTION_RANGE,from,stop,GLASS_MOTION_ENTER,0);
  if(!job)return false;
  job->base_color=lv_color_hex(accent);job->last_color=lv_color_hex(background);glass_motion_apply(job,from);return true;
}
static bool glass_motion_range_linked(lv_obj_t *obj,lv_obj_t *thumb,int stop,uint32_t accent,uint32_t background,bool animate)
{
  bool active=glass_motion_range(obj,stop,accent,background,animate);
  for(unsigned i=0;i<GLASS_MOTION_SLOTS;i++){
    struct glass_motion_job *job=&g_motion_jobs[i];
    if(job->object==obj&&job->kind==MOTION_RANGE){
      job->range_thumb=thumb;glass_motion_watch(thumb);glass_motion_range_paint(job,job->value);return true;
    }
  }
  struct glass_motion_job immediate={.object=obj,.range_thumb=thumb,.base_color=lv_color_hex(accent),.last_color=lv_color_hex(background)};
  glass_motion_range_paint(&immediate,stop);return active;
}
static void glass_motion_cancel(lv_obj_t *obj)
{
  if(!obj)return;
  for(unsigned i=0;i<GLASS_MOTION_SLOTS;i++)if(g_motion_jobs[i].object==obj)glass_motion_finish(&g_motion_jobs[i],false);
  glass_motion_idle();
}
static void glass_motion_press_event(lv_event_t *event)
{
  lv_obj_t *obj=lv_event_get_target(event);lv_event_code_t code=lv_event_get_code(event);
  if(obj!=lv_event_get_current_target(event))return;
  if(g_motion_reduced)return;
  if(code==LV_EVENT_PRESSED)(void)0;
  if(code==LV_EVENT_PRESSED&&!lv_obj_has_state(obj,LV_STATE_DISABLED))glass_motion_begin(obj,MOTION_PRESS,0,1024,GLASS_MOTION_PRESS,0);
  else if(code==LV_EVENT_RELEASED||code==LV_EVENT_PRESS_LOST){
    for(unsigned i=0;i<GLASS_MOTION_SLOTS;i++)if(g_motion_jobs[i].object==obj){
      glass_motion_begin(obj,MOTION_PRESS,0,0,GLASS_MOTION_RELEASE,0);break;
    }
  }
}
static void glass_motion_button(lv_obj_t *obj)
{
  if(!obj)return;
  for(unsigned i=0;i<lv_obj_get_event_count(obj);i++)
    if(lv_event_dsc_get_cb(lv_obj_get_event_dsc(obj,i))==glass_motion_press_event)return;
  lv_obj_add_event_cb(obj,glass_motion_press_event,LV_EVENT_ALL,NULL);
}
static void glass_motion_collect(lv_obj_t *obj,lv_obj_t **items,unsigned *count,unsigned depth)
{
  if(!obj||depth>10||*count>=GLASS_MOTION_BATCH_LIMIT||!lv_obj_is_visible(obj))return;
  if(lv_obj_has_class(obj,&lv_label_class)){
    lv_obj_t *parent=lv_obj_get_parent(obj);
    /* Control geometry and captions stay fixed under an active pointer. */
    if(!parent||!lv_obj_check_type(parent,&lv_button_class))items[(*count)++]=obj;
    return;
  }
  for(unsigned i=0;i<lv_obj_get_child_count(obj);i++)
    glass_motion_collect(lv_obj_get_child(obj,i),items,count,depth+1);
}
static void glass_motion_enter(lv_obj_t *obj,bool horizontal,unsigned delay)
{
  LV_UNUSED(delay);
  if(!obj||g_motion_reduced)return;
  lv_obj_update_layout(obj);
  lv_obj_t *items[GLASS_MOTION_BATCH_LIMIT];unsigned count=0;
  glass_motion_collect(obj,items,&count,0);
  glass_motion_present_axis(items,count,6,horizontal);
  (void)0;
}
static void glass_motion_page_ready(void *object)
{glass_motion_enter(object,false,0);}
static void glass_motion_panel(lv_obj_t *obj)
{
  if(!obj||g_motion_reduced)return;
  glass_motion_watch(obj);
  lv_async_call_cancel(glass_motion_page_ready,obj);
  lv_async_call(glass_motion_page_ready,obj);
}
static void glass_motion_notice(lv_obj_t *obj)
{glass_motion_begin(obj,MOTION_NOTICE,0,1024,GLASS_MOTION_NOTICE,0);}
static bool glass_motion_dismiss(lv_obj_t *obj,void (*done)(lv_obj_t *))
{
  LV_UNUSED(done);
  glass_motion_cancel(obj);
  return false; /* The caller performs immediate cleanup. */
}
static void glass_motion_set_reduced(bool reduced)
{
  g_motion_reduced=reduced;
  (void)0;
  if(reduced){
    for(unsigned i=0;i<GLASS_MOTION_SLOTS;i++)if(g_motion_jobs[i].object)glass_motion_finish(&g_motion_jobs[i],true);
    glass_motion_idle();
  }
}
