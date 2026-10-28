/* SPDX-License-Identifier: Apache-2.0 */
#include "local_video_ui.h"
#include "glass_input_guard.h"
#include "local_album.h"
#include "local_video_media.h"
#include "bili_ppa.h"
#include "bili_perf.h"
#include "glass_music.h"
#include "glass_app_controls.h"
#include "glass_media_controls.h"
#include "glass_music_pcm.h"
#include "glass_device_controls.h"
#ifdef GLASS_VIDEO_BLIT
#include "glass_video_blit.h"
#include <lvgl/src/core/lv_obj_event_private.h>
#endif
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#ifndef MSDOS_SUPER_MAGIC
#define MSDOS_SUPER_MAGIC 0x4d44
#endif
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <nuttx/video/fb.h>
#define W 640
#define H 360
#define FILES 64
struct play_session {
  struct local_video *media;
  struct music_pcm pcm;
  struct bili_ppa ppa;
  struct bili_ppa display_ppa;
  uint16_t *full_pixels;
  unsigned full_width,full_height;
  pthread_t thread;
  atomic_bool done;
  pthread_mutex_t lock;
  uint16_t *back,*pending,*shown;
  bool dirty;
  unsigned pending_width,pending_height;
  int result;
  unsigned displayed,overwritten;
  struct bili_perf_window perf;
};
struct entry { char name[128]; bool directory; };
struct browser {
  pthread_t thread;
  atomic_bool done;
  char path[512];
  struct entry entries[FILES];
  unsigned count;
  int error;
  bool truncated;
};
static struct play_session *session;
static struct browser *scan;
static lv_obj_t *page,*image,*label,*list,*path_label,*pause_button;
static lv_obj_t *viewport,*fullscreen,*metrics;
static lv_obj_t *volume_slider;
static lv_timer_t *timer;
static lv_image_dsc_t picture;
static lv_image_dsc_t full_picture;
static const lv_font_t *font;
static char directory[512]="/sdcard",requested[512];
static struct entry displayed[FILES];
static unsigned displayed_count;
static uint32_t status_updated;
static bool album_playback_view;
static bool fullscreen_ppa_enabled=true;
static bool fullscreen_on_screen=true;
#ifdef GLASS_VIDEO_BLIT
static void local_video_cover(lv_event_t *event)
{
  lv_cover_check_info_t *check=lv_event_get_param(event);
  lv_obj_t *obj=lv_event_get_current_target(event);
  if(!check || !check->area || check->res==LV_COVER_RES_MASKED || obj!=image ||
     lv_image_get_src(obj)!=&full_picture ||
     lv_obj_get_style_opa(obj,0)!=LV_OPA_COVER ||
     lv_obj_get_style_image_opa(obj,0)!=LV_OPA_COVER ||
     lv_obj_get_style_radius(obj,0) || lv_image_get_rotation(obj) ||
     lv_image_get_scale_x(obj)!=LV_SCALE_NONE || lv_image_get_scale_y(obj)!=LV_SCALE_NONE ||
     lv_image_get_offset_x(obj) || lv_image_get_offset_y(obj) ||
     lv_image_get_bitmap_map_src(obj) || lv_image_get_blend_mode(obj)!=LV_BLEND_MODE_NORMAL ||
     lv_obj_get_width(obj)!=(int32_t)full_picture.header.w ||
     lv_obj_get_height(obj)!=(int32_t)full_picture.header.h)return;
  lv_area_t area;lv_obj_get_coords(obj,&area);
  if(check->area->x1>=area.x1 && check->area->y1>=area.y1 &&
     check->area->x2<=area.x2 && check->area->y2<=area.y2 &&
     glass_video_blit_cover_source(&full_picture))
    check->res=LV_COVER_RES_COVER;
}
#endif
/* GUI-thread ownership keeps the shown native frame stable during DMA.
 * Decoders keep their native triple buffers. One separate full-screen image
 * is reused, including when a paused/finished picture changes display mode.
 */
static const lv_image_dsc_t *local_video_scaled_picture(unsigned width,
                                                      unsigned height)
{
  struct play_session *s=session;
  if(!s||!picture.data||s->display_ppa.failed||width<2||width>1024||
     height<2||height>600)return NULL;
  if(!s->full_pixels){
    s->full_pixels=bili_ppa_pixels((size_t)width*height);
    if(!s->full_pixels){s->display_ppa.failed=1;return NULL;}
    s->full_width=width;s->full_height=height;
  }
  if(s->full_width!=width||s->full_height!=height)return NULL;
  /* Never submit a failed/partial DMA result to LVGL. Native pixels remain
   * available for the established software transform fallback.
   */
  lv_image_cache_drop(&full_picture);
  if(bili_ppa_rgb565_fit(&s->display_ppa,s->full_pixels,
      (const uint16_t *)picture.data,picture.header.w,picture.header.h,
      width,height)<0)return NULL;
  memset(&full_picture,0,sizeof(full_picture));
  full_picture.header.magic=LV_IMAGE_HEADER_MAGIC;
  full_picture.header.cf=LV_COLOR_FORMAT_RGB565;
  full_picture.header.w=width;full_picture.header.h=height;
  full_picture.header.stride=width*2;full_picture.data_size=width*height*2;
  full_picture.data=(const uint8_t *)s->full_pixels;
  return &full_picture;
}
static void picture_layout(void)
{
  if(!image)return;
#ifdef GLASS_VIDEO_BLIT
  glass_video_blit_bind(NULL);
#endif
  const lv_image_dsc_t *shown=&picture;
  if(fullscreen&&fullscreen_ppa_enabled){
    lv_obj_t *overlay=lv_obj_get_parent(image);
    const lv_image_dsc_t *scaled=local_video_scaled_picture(
      lv_obj_get_width(overlay),lv_obj_get_height(overlay));
    if(scaled){
      shown=scaled;
#ifdef GLASS_VIDEO_BLIT
      glass_video_blit_bind(scaled);
#endif
    }
  }
  lv_image_set_src(image,shown);
  unsigned w=shown->header.w?shown->header.w:W;
  unsigned h=shown->header.h?shown->header.h:H;
  lv_obj_set_size(image,w,h);
  unsigned scale=LV_SCALE_NONE;
  if(fullscreen&&shown==&picture){
    lv_obj_t *overlay=lv_obj_get_parent(image);
    unsigned sw=lv_obj_get_width(overlay),sh=lv_obj_get_height(overlay);
    scale=sw*LV_SCALE_NONE/w;
    unsigned vertical=sh*LV_SCALE_NONE/h;
    if(vertical<scale)scale=vertical;
  }
  lv_image_set_pivot(image,w/2,h/2);
  lv_image_set_scale(image,scale);
  lv_image_set_antialias(image,false);
  lv_obj_center(image);
}
void local_video_set_upscale(bool enabled)
{
  fullscreen_ppa_enabled=enabled;
  if(fullscreen){picture_layout();lv_obj_invalidate(image);}
  printf("[video-scale] enabled=%u\n",enabled);
}
void local_video_set_screen_layer(bool enabled)
{
  fullscreen_on_screen=enabled;
  if(fullscreen && page){
    lv_obj_set_parent(fullscreen,enabled?lv_obj_get_screen(page):
      lv_display_get_layer_top(lv_obj_get_display(page)));
    picture_layout();lv_obj_invalidate(fullscreen);
  }
  printf("[video-layer] screen=%u\n",enabled);
}
void local_video_draw_status(void (*emit)(const char *,...))
{
  emit("VIDEO_GEOMETRY fullscreen=%u screen_layer=%u visible=%u source_full=%u parent_full=%u width=%u height=%u scale_x=%u scale_y=%u opa=%u image_opa=%u\n",
    fullscreen!=NULL,fullscreen_on_screen,image?lv_obj_is_visible(image):0,
    image?lv_image_get_src(image)==&full_picture:0,
    image?lv_obj_get_parent(image)==fullscreen:0,
    image?(unsigned)lv_obj_get_width(image):0,image?(unsigned)lv_obj_get_height(image):0,
    image?(unsigned)lv_image_get_scale_x(image):0,image?(unsigned)lv_image_get_scale_y(image):0,
    image?lv_obj_get_style_opa(image,0):0,image?lv_obj_get_style_image_opa(image,0):0);
  if(fullscreen && session && full_picture.header.w==1024 &&
     full_picture.header.h==600 && full_picture.data){
    struct local_video_snapshot st;local_video_snapshot(session->media,&st);
    if(!st.paused){emit("VIDEO_PIXELS paused=0\n");return;}
    struct fb_planeinfo_s plane;memset(&plane,0,sizeof(plane));
    int fd=open("/dev/fb0",O_RDONLY);
    int ret=fd<0?-1:ioctl(fd,FBIOGET_PLANEINFO,(unsigned long)&plane);
    if(fd>=0)close(fd);
    if(ret<0 || plane.bpp!=16 || plane.stride<2048 ||
       (size_t)(plane.yoffset+600)*plane.stride>plane.fblen){
      emit("VIDEO_PIXELS paused=1 ready=0\n");return;
    }
    unsigned matches=0,nonblack=0,samples=0;
    const uint16_t *expected=(const uint16_t *)full_picture.data;
    for(unsigned y=80;y<500;y+=70)for(unsigned x=48;x<960;x+=96){
      const uint16_t *actual=(const uint16_t *)((const uint8_t *)plane.fbmem+
        (size_t)(plane.yoffset+y)*plane.stride);
      uint16_t pixel=expected[y*1024+x];
      samples++;if(pixel)nonblack++;if(actual[x]==pixel)matches++;
    }
    emit("VIDEO_PIXELS paused=1 ready=1 samples=%u matches=%u nonblack=%u yoffset=%u\n",samples,matches,nonblack,(unsigned)plane.yoffset);
  }
}
static void fullscreen_exit(lv_event_t *e)
{
  (void)e;if(!fullscreen)return;
  lv_obj_t *overlay=fullscreen;fullscreen=NULL;
  if(image&&viewport){lv_obj_set_parent(image,viewport);picture_layout();}
  if(metrics&&page){lv_obj_set_parent(metrics,page);lv_obj_set_pos(metrics,10,476);lv_obj_set_style_text_color(metrics,lv_color_black(),0);lv_obj_set_style_bg_opa(metrics,LV_OPA_TRANSP,0);}
  lv_obj_delete(overlay);
}
bool local_video_back(lv_obj_t *owner)
{
  if(!owner||!fullscreen)return false;
  lv_obj_t *parent=page;
  while(parent&&parent!=owner)parent=lv_obj_get_parent(parent);
  if(!parent)return false;
  fullscreen_exit(NULL);
  return true;
}
static void fullscreen_clicked(lv_event_t *e)
{
  (void)e;if(!page||!image||fullscreen)return;
  /* Active-screen cover search can skip the desktop below this opaque view.
   * Top-layer overlays are drawn only after LVGL has redrawn that desktop.
   * Use this page's display, independent of the global default display.
   */
  lv_obj_t *top=fullscreen_on_screen?lv_obj_get_screen(page):
    lv_display_get_layer_top(lv_obj_get_display(page));
  fullscreen=lv_obj_create(top);
  lv_obj_set_pos(fullscreen,0,0);
  lv_obj_set_size(fullscreen,lv_display_get_horizontal_resolution(lv_obj_get_display(top)),
                   lv_display_get_vertical_resolution(lv_obj_get_display(top)));
  lv_obj_set_style_pad_all(fullscreen,0,0);
  lv_obj_set_style_border_width(fullscreen,0,0);
  lv_obj_set_style_radius(fullscreen,0,0);
  lv_obj_set_style_bg_color(fullscreen,lv_color_black(),0);
  lv_obj_set_style_bg_opa(fullscreen,LV_OPA_COVER,0);
  lv_obj_remove_flag(fullscreen,LV_OBJ_FLAG_SCROLLABLE);
  if(font)lv_obj_set_style_text_font(fullscreen,font,0);
  lv_obj_set_parent(image,fullscreen);
  lv_obj_update_layout(fullscreen);picture_layout();
  if(metrics){lv_obj_set_parent(metrics,fullscreen);lv_obj_align(metrics,LV_ALIGN_BOTTOM_LEFT,12,-12);lv_obj_set_style_text_color(metrics,lv_color_white(),0);lv_obj_set_style_bg_color(metrics,lv_color_black(),0);lv_obj_set_style_bg_opa(metrics,LV_OPA_70,0);}
  glass_media_exit(fullscreen,font,fullscreen_exit,NULL);
}

static void status(const char *text){if(label)lv_label_set_text(label,text);}
static int clip(int x){return x<0?0:x>255?255:x;}
static inline uint16_t rgb_pixel(int y,int vr,int ugvg,int ub)
{
  int base=298*(y-16)+128;
  int r=clip((base+vr)>>8),g=clip((base-ugvg)>>8),b=clip((base+ub)>>8);
  return (r>>3)<<11|(g>>2)<<5|(b>>3);
}
static int frame(void *opaque,const uint8_t *const p[3],const int stride[2],int w,int h)
{
  struct play_session *s=opaque;
  if(bili_ppa_frame(&s->ppa,s->back,p,stride,w,h,W,H)==0)goto publish;
  /* Native 640x360 path: each 2x2 tile shares one chroma pair.
   * Avoid scale maps and recomputing the same chroma products four times. */
  if(w==W&&h==H){
    for(int y=0;y<H;y+=2){
      const uint8_t *y0=p[0]+y*stride[0],*y1=y0+stride[0];
      const uint8_t *u=p[1]+y/2*stride[1],*v=p[2]+y/2*stride[1];
      uint16_t *d0=s->back+y*W,*d1=d0+W;
      for(int x=0;x<W;x+=2){
        int U=u[x/2]-128,V=v[x/2]-128;
        int vr=409*V,ugvg=100*U+208*V,ub=516*U;
        d0[x]=rgb_pixel(y0[x],vr,ugvg,ub);d0[x+1]=rgb_pixel(y0[x+1],vr,ugvg,ub);
        d1[x]=rgb_pixel(y1[x],vr,ugvg,ub);d1[x+1]=rgb_pixel(y1[x+1],vr,ugvg,ub);
      }
    }
    goto publish;
  }
  int dw=W,dh=h*W/w;if(dh>H){dh=H;dw=w*H/h;}
  int ox=(W-dw)/2,oy=(H-dh)/2;int xs[W];
  if(dw!=W||dh!=H)memset(s->back,0,W*H*2);
  for(int x=0;x<dw;x++)xs[x]=x*w/dw;
  for(int y=0;y<dh;y++){
    int sy=y*h/dh;
    const uint8_t *yp=p[0]+sy*stride[0],*up=p[1]+sy/2*stride[1],*vp=p[2]+sy/2*stride[1];
    for(int x=0;x<dw;x++){
      int sx=xs[x],Y=yp[sx]-16,U=up[sx/2]-128,V=vp[sx/2]-128;
      int r=clip((298*Y+409*V+128)>>8),g=clip((298*Y-100*U-208*V+128)>>8),b=clip((298*Y+516*U+128)>>8);
      s->back[(y+oy)*W+x+ox]=(r>>3)<<11|(g>>2)<<5|(b>>3);
    }
  }
publish:
  pthread_mutex_lock(&s->lock);if(s->dirty)s->overwritten++;uint16_t *old=s->pending;s->pending=s->back;s->back=old;s->pending_width=W;s->pending_height=H;s->dirty=true;pthread_mutex_unlock(&s->lock);return 0;
}
static uint16_t *native_buffer(void *opaque,unsigned w,unsigned h)
{
  struct play_session *s=opaque;
  return w&&h&&w<=W&&h<=H?s->back:NULL;
}
static int native_frame(void *opaque,const uint16_t *pixels,unsigned w,unsigned h)
{
  struct play_session *s=opaque;
  if(pixels!=s->back||!w||!h||w>W||h>H)return -EINVAL;
  pthread_mutex_lock(&s->lock);
  if(s->dirty)s->overwritten++;
  uint16_t *old=s->pending;s->pending=s->back;s->back=old;
  s->pending_width=w;s->pending_height=h;s->dirty=true;
  pthread_mutex_unlock(&s->lock);return 0;
}
static bool audio_cancelled(void *p){return local_video_cancelled(((struct play_session *)p)->media);}
static int audio_open(void *p,unsigned rate,unsigned vol)
{
  (void)vol;
  /* The shared PCM writer applies the live system gain. A second codec gain
   * can strand playback at zero after a diagnostic or player mute. */
  return music_pcm_foreground_open(&((struct play_session *)p)->pcm,rate,100,5000,audio_cancelled,p);
}
static int audio_write(void *p,const int16_t *samples,unsigned n,unsigned c){return music_pcm_write(&((struct play_session *)p)->pcm,samples,n,c);}
static int audio_pause(void *p,bool pause){return music_pcm_pause(&((struct play_session *)p)->pcm,pause);}
static int audio_volume(void *p,unsigned vol)
{
  (void)p;
  /* Media snapshots retain their volume field; only GUI setters own the
   * system value. A delayed worker update must not overwrite a newer value. */
  return vol<=100?0:-EINVAL;
}
static int audio_drain(void *p){return music_pcm_drain(&((struct play_session *)p)->pcm);}
static unsigned audio_time(void *p){struct music_pcm *pcm=&((struct play_session *)p)->pcm;return pcm->rate?(uint64_t)pcm->played_samples*1000/pcm->rate:0;}
static void audio_close(void *p){music_pcm_close(&((struct play_session *)p)->pcm);}
static const struct local_video_ops ops={frame,audio_open,audio_write,audio_pause,audio_volume,audio_drain,audio_time,audio_close,native_buffer,native_frame};
static void *play_worker(void *opaque)
{
  struct play_session *s=opaque;
  int r=glass_music_stop_wait(2000);
  if(r){s->result=r;}else s->result=local_video_run(s->media);
  printf("[local-video] completed result=%d\n",s->result);
  atomic_store(&s->done,true);return NULL;
}
static void release(void)
{
  if(!session)return;
  if(!atomic_load(&session->done))return;
  pthread_join(session->thread,NULL);
#ifdef GLASS_VIDEO_BLIT
  glass_video_blit_release();
#endif
  bili_ppa_close(&session->ppa);
  bili_ppa_close(&session->display_ppa);
  if(image){lv_image_set_src(image,NULL);lv_image_cache_drop(&picture);}
  lv_image_cache_drop(&full_picture);
  memset(&full_picture,0,sizeof(full_picture));
  local_video_destroy(session->media);pthread_mutex_destroy(&session->lock);
  free(session->full_pixels);free(session->back);free(session->pending);free(session->shown);free(session);session=NULL;
}
static bool video_extension(const char *p)
{
  const char *ext=strrchr(p,'.');
  return ext&&(!strcasecmp(ext,".mp4")||!strcasecmp(ext,".mov")||!strcasecmp(ext,".m4v")||!strcasecmp(ext,".qt"));
}
static bool valid_path(const char *p)
{
  return local_album_valid_path(p) && strrchr(p,'/') && strlen(p)<512;
}
void local_video_play_path(const char *path)
{
  if(!page||!valid_path(path)){status("请选择可播放的视频文件");return;}
  if(session&&!atomic_load(&session->done)){
    snprintf(requested,sizeof(requested),"%s",path);local_video_cancel(session->media);status("正在停止当前视频…");return;
  }
  requested[0]=0;release();
  struct play_session *s=calloc(1,sizeof(*s));if(!s){status("内存不足");return;}
  s->pcm.fd=-1;s->pcm.mq=(mqd_t)-1;
  s->back=bili_ppa_pixels(W*H);s->pending=bili_ppa_pixels(W*H);s->shown=bili_ppa_pixels(W*H);
  s->media=local_video_create(path,&ops,s);
  if(!s->back||!s->pending||!s->shown||!s->media){free(s->back);free(s->pending);free(s->shown);local_video_destroy(s->media);free(s);status("内存不足");return;}
  pthread_mutex_init(&s->lock,NULL);atomic_init(&s->done,false);local_video_volume(s->media,glass_speaker_volume());
  session=s;memset(&picture,0,sizeof(picture));picture.header.magic=LV_IMAGE_HEADER_MAGIC;picture.header.cf=LV_COLOR_FORMAT_RGB565;
  picture.header.w=W;picture.header.h=H;picture.header.stride=W*2;picture.data_size=W*H*2;picture.data=(const uint8_t *)s->shown;
  lv_image_set_src(image,&picture);picture_layout();status("正在打开视频和音频…");
  pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,65536);
  int r=pthread_create(&s->thread,&attr,play_worker,s);pthread_attr_destroy(&attr);
  if(r){session=NULL;local_video_destroy(s->media);pthread_mutex_destroy(&s->lock);lv_image_set_src(image,NULL);free(s->back);free(s->pending);free(s->shown);free(s);status("无法启动播放器");}
}
static void *scan_worker(void *opaque)
{
  struct browser *b=opaque;
  if(!strcmp(b->path,"/sdcard")){
    struct statfs fs;
    bool mounted=statfs(b->path,&fs)==0 && fs.f_type==MSDOS_SUPER_MAGIC;
    if(!mounted && mount("/dev/mmcsd0","/sdcard","vfat",0,NULL)<0){
      int error=errno;
      /* Another storage app can finish mounting while this worker starts. */
      if(statfs(b->path,&fs)<0 || fs.f_type!=MSDOS_SUPER_MAGIC){b->error=-error;goto done;}
    }
  }
  DIR *d=opendir(b->path);if(!d){b->error=-errno;goto done;}
  struct dirent *e;
  while((e=readdir(d))){
    if(e->d_name[0]=='.'||strlen(e->d_name)>=128)continue;
    char p[512];if(snprintf(p,sizeof(p),"%s/%s",b->path,e->d_name)>=(int)sizeof(p))continue;
    struct stat st;if(lstat(p,&st))continue;
    bool dir=S_ISDIR(st.st_mode);
    if(!dir&&(!S_ISREG(st.st_mode)||!video_extension(e->d_name)))continue;
    if(b->count==FILES){b->truncated=true;break;}
    snprintf(b->entries[b->count].name,128,"%s",e->d_name);b->entries[b->count++].directory=dir;
  }
  closedir(d);
done:atomic_store(&b->done,true);return NULL;
}
static void refresh(lv_event_t *e)
{
  (void)e;if(scan)return;
  scan=calloc(1,sizeof(*scan));if(!scan){status("内存不足");return;}
  atomic_init(&scan->done,false);snprintf(scan->path,sizeof(scan->path),"%s",directory);
  pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,32768);
  int r=pthread_create(&scan->thread,&attr,scan_worker,scan);pthread_attr_destroy(&attr);
  if(r){free(scan);scan=NULL;status("无法读取目录");}else status("正在读取 SD 卡…");
}
static void entry_clicked(lv_event_t *e)
{
  unsigned index=(uintptr_t)lv_event_get_user_data(e);
  if(index>=displayed_count || scan)return;
  const char *name=displayed[index].name;
  bool dir=displayed[index].directory;
  char next[512];if(snprintf(next,sizeof(next),"%s/%s",directory,name)>=(int)sizeof(next)){status("路径太长");return;}
  if(dir){snprintf(directory,sizeof(directory),"%s",next);refresh(NULL);}else local_video_play_path(next);
}
static void up(lv_event_t *e){(void)e;if(scan)return;char *p=strrchr(directory,'/');if(p&&p>directory+7)*p=0;else strcpy(directory,"/sdcard");refresh(NULL);}
static void stop_clicked(lv_event_t *e){(void)e;requested[0]=0;if(session)local_video_cancel(session->media);status("正在停止…");}
static void pause_clicked(lv_event_t *e){(void)e;if(session&&!atomic_load(&session->done)){struct local_video_snapshot st;local_video_snapshot(session->media,&st);local_video_pause(session->media,!st.paused);}}
void local_video_set_volume(unsigned value)
{
  if(glass_speaker_volume_set(value))return;
  if(session)local_video_volume(session->media,value);
}
static void volume_changed(lv_event_t *e){local_video_set_volume(lv_slider_get_value(lv_event_get_target(e)));}
void local_video_stop(void)
{
  requested[0]=0;if(session)local_video_cancel(session->media);
  fullscreen_exit(NULL);
  page=image=label=list=path_label=pause_button=viewport=volume_slider=NULL;
  metrics=NULL;
}
static void deleted(lv_event_t *e){if(lv_event_get_target(e)==page)local_video_stop();}
static void tick(lv_timer_t *t)
{
  if(volume_slider&&!lv_obj_has_state(volume_slider,LV_STATE_PRESSED))
    lv_slider_set_value(volume_slider,glass_speaker_volume(),LV_ANIM_OFF);
  if(scan&&atomic_load(&scan->done)){
    pthread_join(scan->thread,NULL);
    if(page&&list){
      lv_obj_clean(list);displayed_count=scan->count;memcpy(displayed,scan->entries,sizeof(displayed));lv_label_set_text(path_label,scan->path);
      if(scan->error)status("SD 卡未就绪，请插入 FAT32 卡后点刷新");
      else{
        for(unsigned i=0;i<scan->count;i++){
          lv_obj_t *b=lv_button_create(list);lv_obj_set_width(b,270);lv_obj_set_height(b,42);
          lv_obj_t *l=lv_label_create(b);lv_label_set_text(l,scan->entries[i].name);lv_obj_set_width(l,250);lv_label_set_long_mode(l,LV_LABEL_LONG_DOT);lv_obj_center(l);
          glass_input_guard_install(b);lv_obj_add_event_cb(b,entry_clicked,LV_EVENT_CLICKED,(void *)(uintptr_t)i);
        }
        status(scan->count?(scan->truncated?"显示前 64 项，请进入子目录选择视频":"选择 MP4 视频播放") : "此目录没有 MP4 视频");
      }
    }
    free(scan);scan=NULL;
  }
  if(session){
    struct local_video_snapshot st;local_video_snapshot(session->media,&st);
    if(st.volume!=glass_speaker_volume())local_video_volume(session->media,glass_speaker_volume());
    if(image){pthread_mutex_lock(&session->lock);bool dirty=session->dirty;unsigned native_w=session->pending_width,native_h=session->pending_height;
      if(dirty){uint16_t *p=session->shown;session->shown=session->pending;session->pending=p;session->dirty=false;}
      pthread_mutex_unlock(&session->lock);
      if(dirty){
        lv_image_cache_drop(&picture);picture.data=(const uint8_t *)session->shown;
        picture.header.w=native_w;picture.header.h=native_h;picture.header.stride=native_w*2;picture.data_size=native_w*native_h*2;
        lv_image_set_src(image,&picture);picture_layout();lv_obj_invalidate(image);
        session->displayed++;
      }
    }
    if(page){
      unsigned overwritten;pthread_mutex_lock(&session->lock);overwritten=session->overwritten;pthread_mutex_unlock(&session->lock);
      if(metrics&&bili_perf_update(&session->perf,lv_tick_get(),st.decoded,st.frames,session->displayed,st.decoder_us,st.render_us)){
        struct bili_perf_window *p=&session->perf;char text[256];
        snprintf(text,sizeof(text),"解码 %u.%02u FPS · 画面 %u.%02u FPS · 解码 %u ms · 转换 %u ms · 覆盖 %u · 丢帧 %u",
          p->decode_fps_x100/100,p->decode_fps_x100%100,p->display_fps_x100/100,p->display_fps_x100%100,
          p->decode_ms,p->convert_ms,overwritten,st.dropped);lv_label_set_text(metrics,text);
        printf("[video-fps] kind=local decode_x100=%u lvgl_x100=%u pure_decode_ms=%u convert_ms=%u decoded=%u submitted=%u lvgl=%u render_skip=%u overwritten=%u lag=%u scale_frames=%u scale_us=%llu scale_failed=%d\n",
          p->decode_fps_x100,p->display_fps_x100,p->decode_ms,p->convert_ms,st.decoded,st.frames,session->displayed,st.dropped,overwritten,st.late_ms,
          session->display_ppa.frames,(unsigned long long)(session->display_ppa.pack_us+session->display_ppa.dma_us),session->display_ppa.failed);
      }
      if(st.running && lv_tick_elaps(status_updated)>=250){status_updated=lv_tick_get();char text[192];snprintf(text,sizeof(text),"%s %02u:%02u / %02u:%02u · %s · 显示 %u 帧 · 落后 %u ms",st.paused?"已暂停":"播放中",st.position_ms/60000,st.position_ms/1000%60,st.duration_ms/60000,st.duration_ms/1000%60,st.has_audio?(st.mjpeg?"MJPEG · AAC":"AAC 音频"):"无音轨",st.frames,st.late_ms);
        if(album_playback_view)snprintf(text,sizeof(text),"%s  %02u:%02u / %02u:%02u",st.paused?"已暂停":"播放中",st.position_ms/60000,st.position_ms/1000%60,st.duration_ms/60000,st.duration_ms/1000%60);
        status(text);}
      if(pause_button){lv_obj_t *l=lv_obj_get_child(pause_button,0);const char *text=st.paused?"继续":"暂停";if(strcmp(lv_label_get_text(l),text))lv_label_set_text(l,text);}
    }
    if(atomic_load(&session->done)){
      if(page){int r=session->result;
        status(r==0?"播放结束":r==-ECANCELED?"已停止":r==-ENOTSUP?"编码不支持，需 H.264/MJPEG 视频和 AAC 音频":r==-EFBIG?"视频过大，最高支持 1280×720":r==-EBUSY?"音频设备正被其他应用使用":r==-ETIMEDOUT?"音乐或音频设备未能及时停止，请稍后重试":r==-ENOENT?"文件不存在或 SD 卡已拔出":"播放失败，请检查视频文件或 SD 卡");
      }
      /* Keep the final picture until stop, next play, or page exit. */
      if(!page||requested[0]){
        char next[512];snprintf(next,sizeof(next),"%s",requested);requested[0]=0;release();if(page&&next[0])local_video_play_path(next);
      }
    }
  }
  if(!page&&!session&&!scan){lv_timer_delete(t);timer=NULL;}
}
static lv_obj_t *button(lv_obj_t *p,const char *text,int x,int y,lv_event_cb_t cb)
{
  lv_obj_t *b=lv_button_create(p);lv_obj_set_pos(b,x,y);lv_obj_set_size(b,90,40);
  lv_obj_t *l=lv_label_create(b);lv_label_set_text(l,text);lv_obj_center(l);glass_input_guard_install(b);lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,NULL);return b;
}
void local_video_show(lv_obj_t *parent,const lv_font_t *zh)
{
  local_video_stop();release();page=parent;font=zh;album_playback_view=false;
  lv_obj_add_event_cb(parent,deleted,LV_EVENT_DELETE,NULL);if(font)lv_obj_set_style_text_font(parent,font,0);
  lv_obj_set_style_pad_all(parent,0,0);lv_obj_remove_flag(parent,LV_OBJ_FLAG_SCROLLABLE);
  button(parent,"刷新",10,12,refresh);button(parent,"上一级",110,12,up);
  pause_button=button(parent,"暂停",310,12,pause_clicked);button(parent,"停止",410,12,stop_clicked);
  button(parent,"全屏",510,12,fullscreen_clicked);
  lv_obj_t *vol=lv_slider_create(parent);volume_slider=vol;lv_obj_set_pos(vol,680,28);lv_obj_set_size(vol,230,12);lv_slider_set_range(vol,0,100);lv_slider_set_value(vol,glass_speaker_volume(),LV_ANIM_OFF);lv_obj_add_event_cb(vol,volume_changed,LV_EVENT_VALUE_CHANGED,NULL);
  lv_obj_t *vl=lv_label_create(parent);lv_label_set_text(vl,"音量");lv_obj_set_pos(vl,620,18);
  path_label=lv_label_create(parent);lv_obj_set_pos(path_label,10,60);lv_obj_set_width(path_label,290);lv_label_set_long_mode(path_label,LV_LABEL_LONG_DOT);lv_label_set_text(path_label,directory);
  list=lv_obj_create(parent);lv_obj_set_pos(list,10,90);lv_obj_set_size(list,290,355);lv_obj_set_flex_flow(list,LV_FLEX_FLOW_COLUMN);lv_obj_set_style_pad_all(list,4,0);
  viewport=lv_obj_create(parent);lv_obj_set_pos(viewport,310,80);lv_obj_set_size(viewport,W,H);lv_obj_set_style_pad_all(viewport,0,0);lv_obj_set_style_border_width(viewport,0,0);lv_obj_set_style_bg_color(viewport,lv_color_black(),0);lv_obj_remove_flag(viewport,LV_OBJ_FLAG_SCROLLABLE);
  image=lv_image_create(viewport);lv_obj_set_size(image,W,H);
#ifdef GLASS_VIDEO_BLIT
  lv_obj_add_event_cb(image,local_video_cover,LV_EVENT_COVER_CHECK,NULL);
#endif
  label=lv_label_create(parent);lv_obj_set_pos(label,10,452);lv_obj_set_width(label,940);lv_label_set_text(label,"MP4 · H.264/MJPEG + AAC");
  metrics=lv_label_create(parent);lv_obj_set_pos(metrics,10,476);lv_obj_set_width(metrics,1000);lv_label_set_long_mode(metrics,LV_LABEL_LONG_DOT);
  lv_label_set_text(metrics,"解码 FPS / 画面 FPS · 每 2 秒采样，画面统计为 LVGL 提交");
  if(!timer)timer=lv_timer_create(tick,33,NULL);
  refresh(NULL);
}


/* Album playback retains the established decoder/session ownership, with a
 * quiet viewer instead of a second file browser. The old entry stays usable. */
static lv_obj_t *video_view_button(lv_obj_t *parent,const char *text,int x,int y,
                                  lv_event_cb_t cb,uint32_t soft,uint32_t ink)
{
  lv_obj_t *b=button(parent,text,x,y,cb);
  glass_app_control_style(b,soft,soft);
  lv_obj_t *l=lv_obj_get_child(b,0);lv_obj_set_style_text_color(l,lv_color_hex(ink),0);
  if(font)lv_obj_set_style_text_font(l,font,0);
  return b;
}
void local_video_view(lv_obj_t *parent,const lv_font_t *zh,const char *path,lv_event_cb_t back)
{
  local_video_stop();release();page=parent;font=zh;album_playback_view=true;
  lv_obj_add_event_cb(parent,deleted,LV_EVENT_DELETE,NULL);
  if(font)lv_obj_set_style_text_font(parent,font,0);
  lv_obj_set_style_pad_all(parent,0,0);lv_obj_remove_flag(parent,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_update_layout(parent);int width=lv_obj_get_width(parent),height=lv_obj_get_height(parent);
  bool light=lv_color_brightness(lv_obj_get_style_bg_color(parent,0))>128;
  struct glass_app_palette palette=glass_app_palette(light);
  uint32_t ink=palette.ink,soft=palette.soft;
  lv_obj_set_style_bg_color(parent,lv_color_hex(palette.canvas),0);
  lv_obj_set_style_text_color(parent,lv_color_hex(ink),0);
  if(back)video_view_button(parent,"返回相册",16,10,back,soft,ink);
  video_view_button(parent,"全屏",width-106,10,fullscreen_clicked,soft,ink);
  const char *name=strrchr(path,'/');path_label=lv_label_create(parent);
  lv_label_set_text(path_label,name?name+1:path);lv_obj_set_pos(path_label,back?132:24,21);lv_obj_set_width(path_label,width-(back?256:148));
  lv_label_set_long_mode(path_label,LV_LABEL_LONG_DOT);lv_obj_set_style_text_color(path_label,lv_color_hex(ink),0);
  viewport=lv_obj_create(parent);lv_obj_remove_style_all(viewport);
  lv_obj_set_pos(viewport,(width-W)/2,60);lv_obj_set_size(viewport,W,H);
  lv_obj_set_style_bg_color(viewport,lv_color_black(),0);lv_obj_set_style_bg_opa(viewport,LV_OPA_COVER,0);
  lv_obj_remove_flag(viewport,LV_OBJ_FLAG_SCROLLABLE);image=lv_image_create(viewport);lv_obj_set_size(image,W,H);
#ifdef GLASS_VIDEO_BLIT
  lv_obj_add_event_cb(image,local_video_cover,LV_EVENT_COVER_CHECK,NULL);
#endif
  pause_button=video_view_button(parent,"暂停",(width-196)/2,height-68,pause_clicked,soft,ink);
  video_view_button(parent,"停止",(width+16)/2,height-68,stop_clicked,soft,ink);
  lv_obj_t *vol=lv_slider_create(parent);lv_obj_set_pos(vol,width-220,height-48);lv_obj_set_size(vol,180,6);
  volume_slider=vol;lv_slider_set_range(vol,0,100);lv_slider_set_value(vol,glass_speaker_volume(),LV_ANIM_OFF);
  lv_obj_set_style_bg_color(vol,lv_color_hex(soft),LV_PART_MAIN);
  lv_obj_set_style_bg_color(vol,lv_color_hex(ink),LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(vol,lv_color_hex(ink),LV_PART_KNOB);
  lv_obj_add_event_cb(vol,volume_changed,LV_EVENT_VALUE_CHANGED,NULL);
  label=lv_label_create(parent);lv_obj_set_pos(label,24,height-22);lv_obj_set_width(label,width-48);
  lv_label_set_long_mode(label,LV_LABEL_LONG_DOT);lv_obj_set_style_text_color(label,lv_color_hex(ink),0);
  metrics=lv_label_create(parent);lv_obj_add_flag(metrics,LV_OBJ_FLAG_HIDDEN);
  if(!timer)timer=lv_timer_create(tick,33,NULL);
  local_video_play_path(path);
}
