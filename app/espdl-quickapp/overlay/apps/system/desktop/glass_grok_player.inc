/* SPDX-License-Identifier: Apache-2.0 */
/* Built-in seagull RGB565+A8 frames; existing state/reader ownership retained. */
#include "glass_grok.h"
#include "glass_seagull.h"
#include "pet_engine.h"
#include "pet_lvgl.h"
#include "glass_ui_visibility.h"
#include "glass_grok_gaze.h"
#include "glass_grok_cache.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifndef GROK_ASSET_ROOT
#define GROK_ASSET_ROOT "/sdcard/grok"
#endif
#define GROK_WAKE_SPRITE 7u
#define GROK_STATES 42
#define GROK_POLL_MS 20
struct grok_entry {uint32_t id,count,offset;};
struct glass_grok {
  lv_obj_t *root,*image,*notice,*overlay;lv_timer_t *timer;
  unsigned nest_taps;uint32_t nest_last_tap,nest_departed_at;
  unsigned nest_motion,nest_motion_index,nest_motion_frame;
  uint32_t nest_motion_at,nest_motion_due;
  pthread_t worker;pthread_mutex_t lock;pthread_cond_t wake;
  lv_image_dsc_t images[2];unsigned char *pixels[2];
  char path[96];struct grok_entry entries[GROK_STATES];
  unsigned side,bytes,period,current,requested,epoch,front,next_frame,states,action;
  unsigned job_slot,job_id,job_frame,job_epoch,result_epoch,result_frame;
  uint32_t due,last_tick,turn_id,settled_at,polled_at,retry_at;
  uint64_t read_us,max_read_us;unsigned displayed,reads,errors,seeks,ticks;
  int error;bool thread_started,quit,pending,busy,ready,initialized;
  bool reduced,settled,celebrating,manual,active;
  bool ambient,mood_armed,mood_active;unsigned mood_index;uint32_t mood_at;
  bool gaze_enabled,black,job_gaze,suspended,home_nest,nest_away,nest_state_valid;
  int gaze_x,gaze_y,job_gaze_x,job_gaze_y;
  struct grok_gaze_scratch *gaze_scratch;
  struct grok_frame_cache idle_cache;
  struct stat bank_stat;
  uint64_t bank_checked_us;
  unsigned sd_reads,cache_hits;
  size_t cache_bytes;
  const char *loading_text,*unavailable_text;
};
static struct glass_grok *instances[8]; /* UI thread only. */
static const unsigned ids[GROK_STATES]={0,1,2,3,4,5,6,7,10,11,12,13,14,15,16,17,18,19,20,21,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51};
static uint64_t grok_micros(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000+t.tv_nsec/1000;}
static unsigned grok_normalize(unsigned id){for(unsigned i=0;i<32;i++)if(ids[i]==id)return id;return 2;}




static void *grok_reader(void *opaque)
{
  struct glass_grok *b=opaque;
  pthread_mutex_lock(&b->lock);
  for(;;){
    while(!b->pending&&!b->quit)pthread_cond_wait(&b->wake,&b->lock);
    if(b->quit)break;
    unsigned id=b->job_id,frame=b->job_frame,epoch=b->job_epoch,slot=b->job_slot;
    bool gaze=b->job_gaze;int gaze_x=b->job_gaze_x,gaze_y=b->job_gaze_y;
    b->pending=false;pthread_mutex_unlock(&b->lock);
    uint64_t began=grok_micros();
    int ret=glass_seagull_render(b->pixels[slot],b->side,id,frame,gaze,gaze_x,gaze_y);
    uint64_t duration=grok_micros()-began;
    pthread_mutex_lock(&b->lock);
    b->read_us+=duration;if(duration>b->max_read_us)b->max_read_us=duration;
    b->reads++;b->cache_hits++;if(ret)b->errors++;
    b->error=ret;b->result_epoch=epoch;b->result_frame=frame;
    b->ready=true;b->busy=false;
  }
  pthread_mutex_unlock(&b->lock);return NULL;
}
/* Input invalidates a prefetched frame's deadline on the UI thread. Restore
 * the original polling bound without letting the reader call LVGL. */
static void grok_wake(struct glass_grok *b)
{
  if(b->timer){
    lv_timer_set_period(b->timer,GROK_POLL_MS);
    if(b->suspended){b->suspended=false;lv_timer_resume(b->timer);}
  }
}
static bool grok_visible(const struct glass_grok *b)
{return b->active&&lv_obj_is_visible(b->root)&&glass_ui_can_paint(b->root)&&!(b->overlay&&lv_obj_is_visible(b->overlay));}
/* Revealing a parent or dismissing an overlay invalidates this retained root.
 * Resume from its draw event, without a second timer polling hidden players. */
static void grok_revealed(lv_event_t *e)
{
  struct glass_grok *b=lv_event_get_user_data(e);
  if(b->suspended&&grok_visible(b))grok_wake(b);
}
static void grok_select_id(struct glass_grok *b,unsigned id)
{
  id=grok_normalize(id);if(id!=b->current||b->action){b->current=id;b->action=0;b->epoch++;b->next_frame=0;b->due=lv_tick_get();b->retry_at=0;grok_wake(b);}
}
/* Keep the requested service state separate from its idle presentation.
 * Repeated idle polls must not restart a mood or reset its deadline. */
static void grok_request_id(struct glass_grok *b,unsigned id)
{
  id=grok_normalize(id);
  if(id!=b->requested){b->requested=id;b->mood_armed=b->mood_active=false;grok_select_id(b,id);}
  else if(id!=2||!b->ambient||b->action)grok_select_id(b,id);
}
static void grok_poll_mood(struct glass_grok *b,uint32_t now)
{
  if(b->home_nest)return; /* Nest gestures have their own bounded cadence. */
  static const unsigned moods[]={3,10,19,14,4,16};
  if(!b->ambient||b->reduced||b->requested!=2||b->action){
    b->mood_armed=b->mood_active=false;
    if(b->requested==2&&!b->action&&b->current!=2)grok_select_id(b,2);
    return;
  }
  if(!b->mood_armed){b->mood_armed=true;b->mood_at=now+6000;return;}
  if((int32_t)(now-b->mood_at)<0)return;
  b->mood_active=!b->mood_active;
  if(b->mood_active){
    grok_select_id(b,moods[b->mood_index++%(sizeof(moods)/sizeof(*moods))]);
    b->mood_at=now+4000;
  }else{
    grok_select_id(b,2);b->mood_at=now+6000+(b->mood_index%3)*977;
  }
}
static void grok_nest_motion(struct glass_grok *b,uint32_t now)
{
  if(!b->home_nest)return;
  bool allowed=b->ambient&&!b->nest_away&&b->requested==2&&!b->action;
  if(!allowed){
    b->nest_motion_due=now+7000;
    if(!b->nest_motion)return;
    b->nest_motion=0;b->nest_motion_frame=~0u;
  }else{
    if(!b->nest_motion_due)b->nest_motion_due=now+7000;
    if(!b->nest_motion&&(int32_t)(now-b->nest_motion_due)>=0){
      b->nest_motion=1+b->nest_motion_index++%SEAGULL_DOZE;
      b->nest_motion_at=now;b->nest_motion_frame=~0u;
    }
    if(!b->nest_motion)return;
    if(now-b->nest_motion_at>=SEAGULL_MOTION_MS){
      b->nest_motion=0;b->nest_motion_due=now+7000+(b->nest_motion_index%3)*1000;
      b->nest_motion_frame=~0u;
    }
  }
  unsigned elapsed=b->nest_motion?(now-b->nest_motion_at)/100*100:0;
  if(b->nest_motion_frame==elapsed)return;
  b->nest_motion_frame=elapsed;
  struct seagull_pose pose=seagull_motion_pose(b->nest_motion,elapsed);
  lv_image_set_pivot(b->image,64,64);
  lv_image_set_scale_x(b->image,144*pose.sx/256);
  lv_image_set_scale_y(b->image,144*pose.sy/256);
  lv_image_set_rotation(b->image,(pose.angle+3600)%3600);
  lv_obj_set_pos(b->image,18+pose.x*72/128,-29+pose.y*72/128);
  grok_select_id(b,allowed?pose.emotion:b->requested);
}
static void grok_poll_state(struct glass_grok *b,uint32_t now)
{
  if(b->manual||(now-b->polled_at<250&&b->polled_at))return;
  struct chat_activity_snapshot chat;struct glass_voice_snapshot voice;
  glass_chat_activity_get(&chat);glass_voice_get(&voice);unsigned emotion=glass_grok_emotion(&chat,&voice);
  if(chat.turn_id!=b->turn_id){b->turn_id=chat.turn_id;b->settled=chat.phase==CHAT_IDLE;b->celebrating=false;}
  if(emotion==33){
    if(!b->settled){b->settled_at=now;b->settled=true;b->celebrating=true;}
    if(!b->celebrating||now-b->settled_at>=2500){emotion=2;b->celebrating=false;}
  }
  grok_request_id(b,emotion);b->polled_at=now;
}
static void grok_nest_sync(struct glass_grok *b)
{
  if(!b->home_nest)return;
  if(lv_obj_is_visible(b->root)){
    lv_obj_update_layout(b->root);lv_area_t area,origin;lv_obj_get_coords(b->root,&area);
    lv_obj_t *screen=b->root;
    for(unsigned depth=0;depth<3&&lv_obj_get_parent(screen);depth++)screen=lv_obj_get_parent(screen);
    lv_obj_get_coords(screen,&origin);pet_engine_set_home(area.x1+82-origin.x1,area.y1+64-origin.y1);
  }
  struct pet_state_s pet;pet_engine_get(&pet);
  if(b->nest_state_valid&&b->nest_away==pet.visible)return;
  b->nest_state_valid=true;b->nest_away=pet.visible;
  b->nest_taps=0;b->nest_last_tap=0;
  if(pet.visible)b->nest_departed_at=lv_tick_get();
  if(pet.visible)lv_obj_add_flag(b->image,LV_OBJ_FLAG_HIDDEN);
  else if(b->initialized)lv_obj_remove_flag(b->image,LV_OBJ_FLAG_HIDDEN);
}
static void grok_nest_toggle(lv_event_t *event)
{
  struct glass_grok *b=lv_event_get_user_data(event);
  struct pet_state_s pet;pet_engine_get(&pet);
  if(pet.visible&&lv_tick_get()-b->nest_departed_at<3000)return;
  if(!pet.visible){
    uint32_t now=lv_tick_get();
    if(now-b->nest_last_tap>3000)b->nest_taps=0;
    b->nest_last_tap=now;
    if(++b->nest_taps<5&&lv_rand(1,100)>b->nest_taps*20){
      b->nest_motion=SEAGULL_TILT;b->nest_motion_at=now;
      b->nest_motion_frame=~0u;grok_wake(b);return;
    }
  }
  lv_area_t area;lv_obj_update_layout(b->root);lv_obj_get_coords(b->root,&area);
  /* Home and pet share the desktop content coordinate space. */
  lv_obj_t *screen=b->root;
    for(unsigned depth=0;depth<3&&lv_obj_get_parent(screen);depth++)screen=lv_obj_get_parent(screen);
  lv_area_t origin;lv_obj_get_coords(screen,&origin);
  pet_engine_set_home(area.x1+82-origin.x1,area.y1+64-origin.y1);
  pet_engine_toggle_visible();pet_lvgl_sync();grok_nest_sync(b);
}

static void grok_display_frame(struct glass_grok *b,unsigned slot)
{
  b->front=slot;
  lv_image_cache_drop(&b->images[slot]);
  lv_image_set_src(b->image,&b->images[slot]);
  if(!b->home_nest||!b->nest_away)lv_obj_remove_flag(b->image,LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(b->notice,LV_OBJ_FLAG_HIDDEN);
  b->initialized=true;b->displayed++;
}

/* Wake popups use a native 64px built-in frame. Decode on the UI thread so
 * microphone/model work cannot starve a separate first-frame worker. */
static bool grok_wake_sprite_frame(struct glass_grok *b)
{
  unsigned slot=1-b->front;
  if(glass_seagull_render(b->pixels[slot],b->side,b->current,b->next_frame,
                          b->gaze_enabled,b->gaze_x,b->gaze_y))return false;
  grok_display_frame(b,slot);
  b->next_frame++;b->due=lv_tick_get()+b->period;
  return true;
}

static void grok_tick(lv_timer_t *timer)
{
  struct glass_grok *b=lv_timer_get_user_data(timer);uint32_t now=lv_tick_get();
  b->ticks++;
  if(!grok_visible(b)){b->suspended=true;lv_timer_pause(timer);return;}
  grok_wake(b);grok_nest_sync(b);
  if(now-b->last_tick>500)b->due=now;b->last_tick=now;grok_poll_state(b,now);grok_poll_mood(b,now);
  grok_nest_motion(b,now);
  if(!b->thread_started){
    if((int32_t)(now-b->due)>=0&&(!b->reduced||!b->next_frame))
      grok_wake_sprite_frame(b);
    lv_timer_set_period(timer,b->period);
    return;
  }
  pthread_mutex_lock(&b->lock);
  if(b->ready&&(b->result_epoch!=b->epoch||(int32_t)(now-b->due)>=0)){
    if(b->result_epoch==b->epoch){
      if(!b->error){
        grok_display_frame(b,b->job_slot);
        b->next_frame=b->result_frame+1;b->due=now+b->period;
        if(b->action==42&&b->next_frame>=30){b->action=0;b->epoch++;b->next_frame=0;}
      }else{
        if(!b->retry_at)fprintf(stderr,"[grok] Built-in seagull frame unavailable (%d): %s\n",b->error,b->path);
        b->retry_at=now+5000;lv_label_set_text(b->notice,b->unavailable_text?b->unavailable_text:"小球素材未就绪");lv_obj_remove_flag(b->notice,LV_OBJ_FLAG_HIDDEN);
      }
    }
    b->ready=false;
  }
  if(!b->busy&&!b->ready&&(!b->retry_at||(int32_t)(now-b->retry_at)>=0)&&(!b->reduced||!b->initialized||b->next_frame==0)){
    b->job_slot=1-b->front;b->job_id=b->action?b->action:b->current;b->job_frame=b->reduced?0:b->next_frame;b->job_epoch=b->epoch;
    b->job_gaze=b->gaze_enabled;b->job_gaze_x=b->gaze_x;b->job_gaze_y=b->gaze_y;
    b->pending=b->busy=true;pthread_cond_signal(&b->wake);
  }
  /* Once a frame is ready, polling cannot make it display sooner. Sleep to
   * its deadline on the original 20 ms grid, preserving chat polling cadence.
   * An in-flight read, stale epoch, hidden view or error keeps fast polling. */
  if(b->ready&&!b->error&&b->result_epoch==b->epoch&&(int32_t)(b->due-now)>0){
    uint32_t wait=b->due-now;
    if(!b->manual){uint32_t elapsed=now-b->polled_at;uint32_t poll=elapsed<250?250-elapsed:GROK_POLL_MS;if(wait>poll)wait=poll;}
    wait=((wait+GROK_POLL_MS-1)/GROK_POLL_MS)*GROK_POLL_MS;
    lv_timer_set_period(timer,wait);
  }
  pthread_mutex_unlock(&b->lock);
}
static void grok_deleted(lv_event_t *e)
{
  struct glass_grok *b=lv_event_get_user_data(e);if(b->timer)lv_timer_delete(b->timer);
  if(b->thread_started){pthread_mutex_lock(&b->lock);b->quit=true;pthread_cond_signal(&b->wake);pthread_mutex_unlock(&b->lock);pthread_join(b->worker,NULL);}
  for(unsigned i=0;i<8;i++)if(instances[i]==b)instances[i]=NULL;
  if(b->image)lv_image_set_src(b->image,NULL);
  for(unsigned i=0;i<2;i++){lv_image_cache_drop(&b->images[i]);free(b->pixels[i]);}
  free(b->gaze_scratch);pthread_cond_destroy(&b->wake);pthread_mutex_destroy(&b->lock);free(b);
}
/* Small, code-native nest: no asset I/O and no additional timer. */
static lv_obj_t *grok_nest_shape(lv_obj_t *parent,int x,int y,int w,int h,
                                  uint32_t color)
{
  lv_obj_t *shape=lv_obj_create(parent);
  lv_obj_remove_style_all(shape);
  lv_obj_set_pos(shape,x,y);lv_obj_set_size(shape,w,h);
  lv_obj_set_style_radius(shape,LV_RADIUS_CIRCLE,0);
  lv_obj_set_style_bg_color(shape,lv_color_hex(color),0);
  lv_obj_set_style_bg_opa(shape,LV_OPA_COVER,0);
  lv_obj_remove_flag(shape,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
  return shape;
}
static void grok_nest_front(lv_obj_t *parent,bool dark)
{
  grok_nest_shape(parent,22,58,120,32,dark?0xb98a59:0xc99b68);
  grok_nest_shape(parent,19,54,126,15,dark?0xd7b585:0xe0bf8f);
  /* Short crossing straw strokes keep the tiny silhouette legible. */
  static const lv_point_precise_t straw[][2]={
    {{32,68},{55,80}},{{48,67},{77,84}},{{70,68},{98,84}},
    {{96,68},{122,79}},{{36,80},{70,68}},{{69,85},{105,69}},
    {{101,84},{132,69}}
  };
  for(unsigned i=0;i<sizeof(straw)/sizeof(straw[0]);i++){
    lv_obj_t *line=lv_line_create(parent);
    lv_line_set_points(line,straw[i],2);
    lv_obj_set_style_line_width(line,3,0);
    lv_obj_set_style_line_rounded(line,true,0);
    lv_obj_set_style_line_color(line,lv_color_hex(i%2?0xead0a6:0xa97b4d),0);
    lv_obj_remove_flag(line,LV_OBJ_FLAG_CLICKABLE);
  }
}

static struct glass_grok *grok_create(lv_obj_t *parent,int width,unsigned style,glass_grok_font_t font)
{
  if(!parent||!font||width<1||width>1024)return NULL;
  struct glass_grok *b=calloc(1,sizeof(*b));if(!b)return NULL;
  pthread_mutex_init(&b->lock,NULL);pthread_cond_init(&b->wake,NULL);
  b->side=(style==0||style==GROK_WAKE_SPRITE)?64:128;b->bytes=b->side*b->side*3;b->period=100;b->current=b->requested=2;b->epoch=1;b->active=true;b->states=style>=5?42:32;
  const char *name=style==0?"mini-white":style==4||style==2?"home-black":style==5?"app-white":style==6?"app-black":"home-white";
  snprintf(b->path,sizeof(b->path),"builtin:seagull/%s",name);
  b->root=lv_obj_create(parent);if(!b->root){pthread_cond_destroy(&b->wake);pthread_mutex_destroy(&b->lock);free(b);return NULL;}
  lv_obj_remove_style_all(b->root);lv_obj_set_size(b->root,width,style==0?64:96);
  lv_obj_clear_flag(b->root,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_user_data(b->root,b);
  lv_obj_add_event_cb(b->root,grok_deleted,LV_EVENT_DELETE,b);
  lv_obj_add_event_cb(b->root,grok_revealed,LV_EVENT_DRAW_MAIN_BEGIN,b);
  b->image=lv_image_create(b->root);lv_obj_set_pos(b->image,style==0?0:12,style==0?0:1);lv_obj_add_flag(b->image,LV_OBJ_FLAG_HIDDEN);
  if(style!=0){
    lv_image_set_pivot(b->image,0,0);
    if(style==3||style==4){
      lv_obj_set_height(b->root,96);
      lv_image_set_scale(b->image,144);
      lv_obj_set_pos(b->image,46,-1);
      lv_image_set_antialias(b->image,true);
      grok_nest_front(b->root,style==4);b->home_nest=true;
      lv_obj_t *toggle=lv_obj_create(b->root);lv_obj_remove_style_all(toggle);
      lv_obj_set_pos(toggle,19,0);lv_obj_set_size(toggle,126,94);
      lv_obj_add_flag(toggle,LV_OBJ_FLAG_CLICKABLE);
      lv_obj_remove_flag(toggle,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_EVENT_BUBBLE);
      lv_obj_add_event_cb(toggle,grok_nest_toggle,LV_EVENT_CLICKED,b);
      grok_nest_sync(b);
    }else{lv_image_set_scale(b->image,192);lv_obj_set_pos(b->image,(width-96)/2,0);}
  }
  b->notice=lv_label_create(b->root);lv_obj_set_style_text_font(b->notice,font(12),0);lv_label_set_text(b->notice,"加载小球…");lv_obj_set_pos(b->notice,8,20);
  for(unsigned i=0;i<2;i++){
    b->pixels[i]=malloc(b->bytes);if(!b->pixels[i]){lv_obj_delete(b->root);return NULL;}
    b->images[i].header.magic=LV_IMAGE_HEADER_MAGIC;b->images[i].header.cf=LV_COLOR_FORMAT_RGB565A8;
    b->images[i].header.w=b->images[i].header.h=b->side;b->images[i].header.stride=b->side*2;
    b->images[i].data_size=b->bytes;b->images[i].data=b->pixels[i];
  }
  if(style==GROK_WAKE_SPRITE){
    if(!grok_wake_sprite_frame(b)){lv_obj_delete(b->root);return NULL;}
  }else{
    pthread_attr_t attr;pthread_attr_init(&attr);
#ifdef __NuttX__
    pthread_attr_setstacksize(&attr,4096);
#endif
    int ret=pthread_create(&b->worker,&attr,grok_reader,b);pthread_attr_destroy(&attr);
    if(ret){lv_obj_delete(b->root);return NULL;}b->thread_started=true;
  }
  b->timer=lv_timer_create(grok_tick,GROK_POLL_MS,b);if(!b->timer){lv_obj_delete(b->root);return NULL;}
  for(unsigned i=0;i<8;i++)if(!instances[i]){instances[i]=b;break;}return b;
}
struct glass_grok *glass_grok_create(lv_obj_t *p,int w,glass_grok_font_t f){return grok_create(p,w,4,f);}
struct glass_grok *glass_grok_create_pixel(lv_obj_t *p,int w,bool black,glass_grok_font_t f){return grok_create(p,w,black?4:3,f);}
struct glass_grok *glass_grok_create_mosaic(lv_obj_t *p,int w,bool black,glass_grok_font_t f){return grok_create(p,w,black?4:3,f);}
struct glass_grok *glass_grok_create_avatar(lv_obj_t *p,glass_grok_font_t f){return grok_create(p,64,0,f);}
struct glass_grok *glass_grok_create_wake_sprite(lv_obj_t *parent,int x,int y,glass_grok_font_t font)
{
  struct glass_grok *b=grok_create(parent,72,GROK_WAKE_SPRITE,font);
  if(b){b->manual=true;glass_grok_geometry(b,x,y,72,72,64);}
  return b;
}
struct glass_grok *glass_grok_create_sprite(lv_obj_t *p,int x,int y,int w,int h,int diameter,bool black,glass_grok_font_t f)
{
  if(diameter<48||diameter>480||w<64||h<64||w>1024||h>600)return NULL;
  struct glass_grok *b=grok_create(p,w,black?6:5,f);if(!b)return NULL;b->manual=true;
  b->black=black;
  glass_grok_geometry(b,x,y,w,h,diameter);return b;
}
bool glass_grok_geometry(struct glass_grok *b,int x,int y,int w,int h,int diameter)
{
  if(!b||b->states!=42||diameter<48||diameter>480||w<64||h<64||w>1024||h>600)return false;
  lv_obj_set_pos(b->root,x,y);lv_obj_set_size(b->root,w,h);
  lv_image_set_scale(b->image,diameter*256/(int)b->side);lv_image_set_antialias(b->image,true);
  lv_image_set_pivot(b->image,0,0);int image_side=diameter;
  lv_obj_set_pos(b->image,(w-image_side)/2,(h-image_side)/2);return true;
}
lv_obj_t *glass_grok_root(struct glass_grok *b){return b?b->root:NULL;}
void glass_grok_select(struct glass_grok *b,unsigned id){if(b){b->manual=true;grok_request_id(b,id);}}
void glass_grok_set_ambient(struct glass_grok *b,bool enabled){if(b){b->ambient=enabled;b->mood_armed=b->mood_active=false;grok_select_id(b,b->requested);grok_wake(b);}}
bool glass_grok_gaze(struct glass_grok *b,int x,int y)
{
  if(!b||b->states!=42)return false;
  if(x<-1000)x=-1000;if(x>1000)x=1000;if(y<-1000)y=-1000;if(y>1000)y=1000;
  /* Quantize to the actual pixel offset so detection jitter does not reread SD. */
  if(!b->gaze_enabled||x*8/1000!=b->gaze_x*8/1000||y*6/1000!=b->gaze_y*6/1000){
    b->gaze_x=x;b->gaze_y=y;b->gaze_enabled=true;b->epoch++;
    if(b->reduced)b->next_frame=0; /* Keep expression animation progressing. */
    b->due=lv_tick_get();grok_wake(b);
  }
  return true;
}
void glass_grok_action(struct glass_grok *b,unsigned action){if(b&&b->states==42&&(action==0||(action>=42&&action<=51))&&action!=b->action){b->action=action;b->epoch++;b->next_frame=0;b->due=lv_tick_get();grok_wake(b);}}
void glass_grok_set_active(struct glass_grok *b,bool active){if(b&&b->active!=active){b->active=active;grok_wake(b);}}
void glass_grok_refresh(struct glass_grok *b){if(b)grok_tick(b->timer);}
void glass_grok_follow_chat(struct glass_grok *b,bool follow){if(b){b->manual=!follow;b->polled_at=0;grok_wake(b);}}
void glass_grok_set_reduced_motion(struct glass_grok *b,bool reduced){if(b&&b->reduced!=reduced){b->reduced=reduced;grok_wake(b);}}
void glass_grok_set_notice_text(struct glass_grok *b,const char *loading,const char *unavailable)
{
  if(!b)return;
  b->loading_text=loading;b->unavailable_text=unavailable;
  if(b->notice)lv_label_set_text(b->notice,b->retry_at?unavailable:loading);
}
void glass_grok_set_obscured_by(struct glass_grok *b,lv_obj_t *o){if(b){b->overlay=o;grok_wake(b);}}
unsigned glass_grok_current(const struct glass_grok *b){return b?b->current:2;}
bool glass_grok_ready(const struct glass_grok *b){return b&&b->initialized;}
bool glass_grok_hit_test(const struct glass_grok *b,int x,int y)
{
  if(!b||!b->initialized||!lv_obj_is_visible(b->image))return false;
  lv_area_t area;lv_obj_get_coords(b->image,&area);
  int sx=lv_image_get_scale_x(b->image),sy=lv_image_get_scale_y(b->image);
  if(sx<=0||sy<=0||x<area.x1||y<area.y1)return false;
  unsigned px=(x-area.x1)*256/sx,py=(y-area.y1)*256/sy;
  return px<b->side&&py<b->side&&
    b->pixels[b->front][b->side*b->side*2+py*b->side+px]>=32;
}
void glass_grok_debug_status(void)
{
  for(unsigned i=0;i<8;i++)if(instances[i]){struct glass_grok *b=instances[i];pthread_mutex_lock(&b->lock);
    printf("seagull-native %s id=%02u frames=%u reads=%u mean_us=%llu max_us=%llu errors=%u error=%d buffer=%u ready=%d visible=%d seeks=%u ticks=%u sd_reads=%u cache_hits=%u cache_bytes=%zu cache_total=%zu\n",b->path,b->current,b->displayed,b->reads,(unsigned long long)(b->reads?b->read_us/b->reads:0),(unsigned long long)b->max_read_us,b->errors,b->error,b->bytes*2,b->initialized,grok_visible(b),b->seeks,b->ticks,b->sd_reads,b->cache_hits,b->cache_bytes,grok_frame_cache_total());pthread_mutex_unlock(&b->lock);}
}
#ifdef GROK_HOST_TEST
void glass_grok_test_stats(struct glass_grok *b,unsigned out[3])
{pthread_mutex_lock(&b->lock);out[0]=b->reads;out[1]=b->displayed;out[2]=b->ticks;pthread_mutex_unlock(&b->lock);}
void glass_grok_test_step(struct glass_grok *b){if(b)grok_tick(b->timer);}
void glass_grok_test_step_visible(void){for(unsigned i=0;i<8;i++)if(instances[i])grok_tick(instances[i]->timer);}
unsigned glass_grok_test_expression(struct glass_grok *b,const char *id)
{
  unsigned value=2;if(id&&strlen(id)>0&&strlen(id)<=2&&strspn(id,"0123456789")==strlen(id))value=grok_normalize(strtoul(id,NULL,10));
  glass_grok_select(b,value);return value;
}
#endif
