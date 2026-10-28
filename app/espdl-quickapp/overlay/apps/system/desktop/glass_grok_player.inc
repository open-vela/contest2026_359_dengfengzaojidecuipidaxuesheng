/* SPDX-License-Identifier: Apache-2.0 */
/* Pre-rendered RGB565+A8 frames. No QuickJS, SVG or image codec. */
#include "glass_grok.h"
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
#define GROK_STATES 42
#define GROK_POLL_MS 20
struct grok_entry {uint32_t id,count,offset;};
struct glass_grok {
  lv_obj_t *root,*image,*notice,*overlay;lv_timer_t *timer;
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
  bool gaze_enabled,black,job_gaze,suspended;
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
static int grok_read_all(int fd,void *data,size_t size){unsigned char *p=data;while(size){ssize_t n=read(fd,p,size);if(n<0&&errno==EINTR)continue;if(n<=0)return n<0?-errno:-EIO;p+=n;size-=n;}return 0;}
static uint32_t grok_le32(const unsigned char *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static int grok_open_bank(struct glass_grok *b)
{
  int fd=open(b->path,O_RDONLY);if(fd<0)return -errno;
  unsigned char h[32],table[GROK_STATES*12];struct stat st={0};int ret=grok_read_all(fd,h,sizeof(h));
  if(!ret&&(memcmp(h,"GROKSD1\0",8)||grok_le32(h+8)!=1||grok_le32(h+12)!=b->side||grok_le32(h+16)!=b->side||grok_le32(h+20)!=b->period||grok_le32(h+24)!=b->states||grok_le32(h+28)!=b->bytes))ret=-EINVAL;
  if(!ret)ret=grok_read_all(fd,table,b->states*12);
  if(!ret&&fstat(fd,&st)<0)ret=-errno;
  for(unsigned i=0;!ret&&i<b->states;i++){
    struct grok_entry *e=&b->entries[i];e->id=grok_le32(table+i*12);e->count=grok_le32(table+i*12+4);e->offset=grok_le32(table+i*12+8);
    if(e->id!=ids[i]||!e->count||e->count>1200||e->offset<sizeof(h)+b->states*12||(uint64_t)e->offset+(uint64_t)e->count*b->bytes>(uint64_t)st.st_size)ret=-EINVAL;
  }
  if(ret){close(fd);return ret;}
  b->bank_stat=st;b->bank_checked_us=grok_micros();return fd;
}
static bool grok_same_bank(const struct stat *a,const struct stat *b)
{
  return a->st_dev==b->st_dev&&a->st_ino==b->st_ino&&a->st_size==b->st_size&&
         a->st_mtime==b->st_mtime&&a->st_ctime==b->st_ctime;
}
static void *grok_reader(void *opaque)
{
  struct glass_grok *b=opaque;int fd=-1;uint64_t cursor=UINT64_MAX;
  pthread_mutex_lock(&b->lock);
  for(;;){
    while(!b->pending&&!b->quit)pthread_cond_wait(&b->wake,&b->lock);
    if(b->quit)break;
    unsigned id=b->job_id,frame=b->job_frame,epoch=b->job_epoch,slot=b->job_slot;
    bool gaze=b->job_gaze;int gaze_x=b->job_gaze_x,gaze_y=b->job_gaze_y;
    b->pending=false;pthread_mutex_unlock(&b->lock);uint64_t began=grok_micros();int ret=0;bool sought=false,hit=false,read_sd=false;
    /* A cached animation must not conceal an unmounted/deleted/replaced bank.
     * Stat once per second, rather than perform SD sector reads every frame.
     * FAT timestamps cannot identify every same-size in-place rewrite. */
    if(fd>=0&&b->idle_cache.entries&&began-b->bank_checked_us>=1000000){
      struct stat current={0};
      if(stat(b->path,&current)<0)ret=-errno;
      if(ret||!grok_same_bank(&b->bank_stat,&current)){
        grok_frame_cache_clear(&b->idle_cache);close(fd);fd=-1;cursor=UINT64_MAX;
      }
      b->bank_checked_us=began;
    }
    if(!ret&&fd<0)fd=grok_open_bank(b);
    if(!ret&&fd<0)ret=fd;
    if(!ret){
      unsigned i=0;while(i<b->states&&b->entries[i].id!=id)i++;
      if(i==b->states)ret=-EINVAL;
      else {
        frame%=b->entries[i].count;uint64_t position=(uint64_t)b->entries[i].offset+(uint64_t)frame*b->bytes;
        off_t offset=(off_t)position;
        if(id==2){
          grok_frame_cache_prepare(&b->idle_cache,b->entries[i].count);
          hit=grok_frame_cache_load(&b->idle_cache,frame,b->pixels[slot],b->side*b->side);
        }
        /* A successful full read already positions this private descriptor at
         * the next frame. Seek only on a state change, loop or reopen. */
        if(!hit){
          if(offset<0||(uint64_t)offset!=position)ret=-EOVERFLOW;
          else if(cursor!=position){sought=true;off_t at=lseek(fd,offset,SEEK_SET);if(at!=offset)ret=at<0?-errno:-EIO;}
          if(!ret){read_sd=true;ret=grok_read_all(fd,b->pixels[slot],b->bytes);if(!ret)cursor=position+b->bytes;}
          /* Cache original pixels before the optional gaze transform. */
          if(!ret&&id==2)grok_frame_cache_store(&b->idle_cache,frame,b->pixels[slot],b->side*b->side);
        }
      }
    }
    if(!ret&&gaze)grok_gaze_frame(b->pixels[slot],b->black,id,gaze_x,gaze_y,b->gaze_scratch);
    if(ret){grok_frame_cache_clear(&b->idle_cache);cursor=UINT64_MAX;if(fd>=0){close(fd);fd=-1;}}
    uint64_t duration=grok_micros()-began;pthread_mutex_lock(&b->lock);
    b->read_us+=duration;if(duration>b->max_read_us)b->max_read_us=duration;
    b->reads++;if(sought)b->seeks++;if(ret)b->errors++;b->error=ret;b->result_epoch=epoch;b->result_frame=frame;
    if(read_sd)b->sd_reads++;
    if(hit)b->cache_hits++;
    b->cache_bytes=b->idle_cache.bytes;
    b->ready=true;b->busy=false;
  }
  pthread_mutex_unlock(&b->lock);if(fd>=0)close(fd);grok_frame_cache_clear(&b->idle_cache);return NULL;
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
static void grok_tick(lv_timer_t *timer)
{
  struct glass_grok *b=lv_timer_get_user_data(timer);uint32_t now=lv_tick_get();
  b->ticks++;
  if(!grok_visible(b)){b->suspended=true;lv_timer_pause(timer);return;}
  grok_wake(b);
  if(now-b->last_tick>500)b->due=now;b->last_tick=now;grok_poll_state(b,now);grok_poll_mood(b,now);
  pthread_mutex_lock(&b->lock);
  if(b->ready&&(b->result_epoch!=b->epoch||(int32_t)(now-b->due)>=0)){
    if(b->result_epoch==b->epoch){
      if(!b->error){
        b->front=b->job_slot;lv_image_cache_drop(&b->images[b->front]);
        lv_image_set_src(b->image,&b->images[b->front]);lv_obj_remove_flag(b->image,LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(b->notice,LV_OBJ_FLAG_HIDDEN);b->initialized=true;b->displayed++;
        b->next_frame=b->result_frame+1;b->due=now+b->period;
        if(b->action==42&&b->next_frame>=30){b->action=0;b->epoch++;b->next_frame=0;}
      }else{
        if(!b->retry_at)fprintf(stderr,"[grok] SD asset unavailable (%d): %s\n",b->error,b->path);
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
static struct glass_grok *grok_create(lv_obj_t *parent,int width,unsigned style,glass_grok_font_t font)
{
  if(!parent||!font||width<1||width>1024)return NULL;
  struct glass_grok *b=calloc(1,sizeof(*b));if(!b)return NULL;
  pthread_mutex_init(&b->lock,NULL);pthread_cond_init(&b->wake,NULL);
  b->side=style==0?64:128;b->bytes=b->side*b->side*3;b->period=100;b->current=b->requested=2;b->epoch=1;b->active=true;b->states=style>=5?42:32;
  const char *name=style==0?"mini-white":style==4||style==2?"home-black":style==5?"app-white":style==6?"app-black":"home-white";
  snprintf(b->path,sizeof(b->path),GROK_ASSET_ROOT "/%s.gbs",name);
  b->root=lv_obj_create(parent);if(!b->root){pthread_cond_destroy(&b->wake);pthread_mutex_destroy(&b->lock);free(b);return NULL;}
  lv_obj_remove_style_all(b->root);lv_obj_set_size(b->root,width,style==0?64:89);
  lv_obj_clear_flag(b->root,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_user_data(b->root,b);
  lv_obj_add_event_cb(b->root,grok_deleted,LV_EVENT_DELETE,b);
  lv_obj_add_event_cb(b->root,grok_revealed,LV_EVENT_DRAW_MAIN_BEGIN,b);
  b->image=lv_image_create(b->root);lv_obj_set_pos(b->image,style==0?0:12,style==0?0:1);lv_obj_add_flag(b->image,LV_OBJ_FLAG_HIDDEN);
  b->notice=lv_label_create(b->root);lv_obj_set_style_text_font(b->notice,font(12),0);lv_label_set_text(b->notice,"加载小球…");lv_obj_set_pos(b->notice,8,20);
  for(unsigned i=0;i<2;i++){
    b->pixels[i]=malloc(b->bytes);if(!b->pixels[i]){lv_obj_delete(b->root);return NULL;}
    b->images[i].header.magic=LV_IMAGE_HEADER_MAGIC;b->images[i].header.cf=LV_COLOR_FORMAT_RGB565A8;
    b->images[i].header.w=b->images[i].header.h=b->side;b->images[i].header.stride=b->side*2;
    b->images[i].data_size=b->bytes;b->images[i].data=b->pixels[i];
  }
  pthread_attr_t attr;pthread_attr_init(&attr);
#ifdef __NuttX__
  pthread_attr_setstacksize(&attr,4096);
#endif
  int ret=pthread_create(&b->worker,&attr,grok_reader,b);pthread_attr_destroy(&attr);
  if(ret){lv_obj_delete(b->root);return NULL;}b->thread_started=true;
  b->timer=lv_timer_create(grok_tick,GROK_POLL_MS,b);if(!b->timer){lv_obj_delete(b->root);return NULL;}
  for(unsigned i=0;i<8;i++)if(!instances[i]){instances[i]=b;break;}return b;
}
struct glass_grok *glass_grok_create(lv_obj_t *p,int w,glass_grok_font_t f){return grok_create(p,w,4,f);}
struct glass_grok *glass_grok_create_pixel(lv_obj_t *p,int w,bool black,glass_grok_font_t f){return grok_create(p,w,black?4:3,f);}
struct glass_grok *glass_grok_create_mosaic(lv_obj_t *p,int w,bool black,glass_grok_font_t f){return grok_create(p,w,black?4:3,f);}
struct glass_grok *glass_grok_create_avatar(lv_obj_t *p,glass_grok_font_t f){return grok_create(p,64,0,f);}
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
  lv_image_set_scale(b->image,diameter*256/96);lv_image_set_antialias(b->image,false);
  lv_image_set_pivot(b->image,0,0);int image_side=128*diameter/96;
  lv_obj_set_pos(b->image,(w-image_side)/2,(h-image_side)/2);return true;
}
lv_obj_t *glass_grok_root(struct glass_grok *b){return b?b->root:NULL;}
void glass_grok_select(struct glass_grok *b,unsigned id){if(b){b->manual=true;grok_request_id(b,id);}}
void glass_grok_set_ambient(struct glass_grok *b,bool enabled){if(b){b->ambient=enabled;b->mood_armed=b->mood_active=false;grok_select_id(b,b->requested);grok_wake(b);}}
bool glass_grok_gaze(struct glass_grok *b,int x,int y)
{
  if(!b||b->states!=42)return false;
  if(!b->gaze_scratch){b->gaze_scratch=calloc(1,sizeof(*b->gaze_scratch));if(!b->gaze_scratch)return false;}
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
void glass_grok_debug_status(void)
{
  for(unsigned i=0;i<8;i++)if(instances[i]){struct glass_grok *b=instances[i];pthread_mutex_lock(&b->lock);
    printf("grok-sd %s id=%02u frames=%u reads=%u mean_us=%llu max_us=%llu errors=%u error=%d buffer=%u ready=%d visible=%d seeks=%u ticks=%u sd_reads=%u cache_hits=%u cache_bytes=%zu cache_total=%zu\n",b->path,b->current,b->displayed,b->reads,(unsigned long long)(b->reads?b->read_us/b->reads:0),(unsigned long long)b->max_read_us,b->errors,b->error,b->bytes*2,b->initialized,grok_visible(b),b->seeks,b->ticks,b->sd_reads,b->cache_hits,b->cache_bytes,grok_frame_cache_total());pthread_mutex_unlock(&b->lock);}
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
