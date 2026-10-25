/* SPDX-License-Identifier: Apache-2.0 */
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
static uint64_t sd_bytes;
static bool missing_bank,replaced_bank;
static ssize_t counted_read(int fd,void *buffer,size_t size)
{
  ssize_t result=read(fd,buffer,size);if(result>0)sd_bytes+=(size_t)result;return result;
}
static int checked_stat(const char *path,struct stat *st)
{
  if(missing_bank){errno=ENOENT;return -1;}
  int result=stat(path,st);if(!result&&replaced_bank)st->st_ino^=1;
  return result;
}
#define read counted_read
/* A function-like macro leaves `struct stat` untouched. */
#define stat(path,st) checked_stat(path,st)
#include "glass_grok_player.inc"
#undef stat
#undef read
#include <assert.h>
#include <inttypes.h>
static struct chat_activity_snapshot chat;
static struct glass_voice_snapshot voice;
void glass_chat_activity_get(struct chat_activity_snapshot *out){*out=chat;}
void glass_voice_get(struct glass_voice_snapshot *out){*out=voice;}
static unsigned char framebuffer[300*180*2];
static FILE *trace;
static unsigned traced_frames;
static bool record_trace=true;
static const lv_font_t *font(int size){(void)size;return LV_FONT_DEFAULT;}
static void flush(lv_display_t *d,const lv_area_t *area,uint8_t *data)
{(void)area;(void)data;lv_display_flush_ready(d);}
static uint64_t frame_hash(const uint8_t *data,size_t size)
{
  uint64_t hash=UINT64_C(14695981039346656037);
  while(size--){hash^=*data++;hash*=UINT64_C(1099511628211);}return hash;
}
static void settle(struct glass_grok *b)
{
  for(unsigned i=0;i<10000;i++){
    pthread_mutex_lock(&b->lock);bool busy=b->busy;pthread_mutex_unlock(&b->lock);
    if(!busy)return;usleep(100);
  }
  assert(!"reader timed out");
}
static void step(struct glass_grok *b)
{
  unsigned before=b->displayed;lv_tick_inc(1);lv_timer_handler();settle(b);
  if(b->displayed!=before){
    if(record_trace)assert(!b->error);lv_refr_now(NULL);if(!record_trace)return;traced_frames++;
    fprintf(trace,"%u %u %u %u %016" PRIx64 " %016" PRIx64 "\n",
      b->side,b->current,b->action,b->result_frame,
      frame_hash(b->pixels[b->front],b->bytes),frame_hash(framebuffer,sizeof(framebuffer)));
  }
}
static void frames(struct glass_grok *b,unsigned count)
{
  unsigned target=b->displayed+count;
  for(unsigned i=0;i<2000+count*150&&b->displayed<target;i++)step(b);
  assert(b->displayed==target);
}
static void expire_bank(struct glass_grok *b)
{
  settle(b);pthread_mutex_lock(&b->lock);b->bank_checked_us=0;pthread_mutex_unlock(&b->lock);
}
int main(int argc,char **argv)
{
  assert(argc==2);trace=fopen(argv[1],"wb");assert(trace);
  lv_init();lv_display_t *display=lv_display_create(300,180);
  lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(display,framebuffer,NULL,sizeof(framebuffer),LV_DISPLAY_RENDER_MODE_FULL);
  lv_display_set_flush_cb(display,flush);
  lv_obj_t *root=lv_screen_active();lv_obj_remove_style_all(root);
  chat.phase=CHAT_IDLE;
  uint64_t repeat_bytes=0;
  for(unsigned style=0;style<5;style++){
    struct glass_grok *b=style==0?glass_grok_create_avatar(root,font):
      style<3?glass_grok_create_pixel(root,285,style==2,font):
      glass_grok_create_sprite(root,0,0,170,170,120,style==4,font);
    assert(b);lv_obj_t *image=b->image;uint8_t *buffers[2]={b->pixels[0],b->pixels[1]};
    frames(b,80);uint64_t warm=sd_bytes;frames(b,160);repeat_bytes+=sd_bytes-warm;
    if(GROK_FRAME_CACHE_BUDGET){assert(b->cache_hits>=159);assert(sd_bytes==warm);}
    else assert(!b->cache_hits);
    if(style>=3){
      for(int x=-1000;x<=1000;x+=500){assert(glass_grok_gaze(b,x,-x));frames(b,4);}
      assert(glass_grok_gaze(b,0,0));frames(b,80);
      glass_grok_select(b,30);frames(b,3);glass_grok_select(b,2);frames(b,80);
      glass_grok_action(b,42);frames(b,31);assert(!b->action);
    }
    glass_grok_set_reduced_motion(b,true);frames(b,1);
    unsigned before=b->displayed;for(unsigned i=0;i<500;i++)step(b);assert(b->displayed==before);
    glass_grok_set_reduced_motion(b,false);frames(b,1);
    glass_grok_set_active(b,false);before=b->displayed;
    for(unsigned i=0;i<300;i++)step(b);assert(b->displayed==before);
    glass_grok_set_active(b,true);frames(b,1);
    assert(b->image==image&&b->pixels[0]==buffers[0]&&b->pixels[1]==buffers[1]);
    lv_obj_delete(b->root);assert(!grok_frame_cache_total());
  }
  /* Fault injection follows the pixel comparison so its error notice does not
   * leave different pixels behind on the intentionally transparent test root. */
  if(GROK_FRAME_CACHE_BUDGET){
    record_trace=false;struct glass_grok *b=glass_grok_create_pixel(root,285,true,font);
    assert(b);frames(b,80);unsigned hits=b->cache_hits;
    replaced_bank=true;expire_bank(b);frames(b,2);
    replaced_bank=false;assert(b->cache_hits==hits);assert(b->cache_bytes<10000);
    missing_bank=true;expire_bank(b);
    for(unsigned i=0;i<300&&!b->error;i++)step(b);
    assert(b->error==-ENOENT&&!b->cache_bytes);
    for(unsigned i=0;i<200&&b->ready;i++)step(b);
    assert(b->retry_at);missing_bank=false;
    b->retry_at=0;frames(b,1);assert(!b->error);
    lv_obj_delete(b->root);assert(!grok_frame_cache_total());
  }
  fclose(trace);lv_obj_delete(root);lv_display_delete(display);lv_deinit();
  printf("CACHE_PLAYER_METRIC {\"frames\":%u,\"sd_bytes\":%" PRIu64 ",\"repeat_bytes\":%" PRIu64 ",\"budget\":%u}\n",
    traced_frames,sd_bytes,repeat_bytes,(unsigned)GROK_FRAME_CACHE_BUDGET);
  puts("GROK_CACHE_PLAYER_OK: three loops; palettes; gaze; actions; hidden/reduced; bank replacement/removal; teardown");
  return 0;
}
