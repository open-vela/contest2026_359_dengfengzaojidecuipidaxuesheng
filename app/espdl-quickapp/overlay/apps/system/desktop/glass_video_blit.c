/* SPDX-License-Identifier: Apache-2.0 */
#include <nuttx/config.h>
#ifdef GLASS_VIDEO_BLIT
#include "glass_video_blit.h"
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <nuttx/video/p4_ppa.h>
#include <lvgl/lvgl.h>
#include <lvgl/src/draw/lv_draw_private.h>
#include <lvgl/src/draw/lv_image_decoder_private.h>

extern int64_t esp_timer_get_time(void);

/* All entry points run in the single LVGL draw thread. The existing PPA ioctl
 * owns DMA2D, completion, timeout cancellation and C2M/M2C cache handling.
 * This bridge never changes scanout addresses or writes the visible page.
 */
static struct {
  const void *source;
  int fd_plus_one;
  bool enabled;
  bool failed;
  bool profiling;
  uint64_t started;
  uint64_t us[8];
  unsigned calls[8];
  uint64_t dma_us;
  unsigned image_dma,copy_dma,rejected,errors;
  unsigned clears;
  unsigned covers;
  uint64_t clear_us;
  unsigned bound_calls,source_calls;
  struct {
    bool valid,same_descriptor,same_data;
    unsigned sw,sh,ss,sbytes,scf,dw,dh,ds,dbytes,dcf;
    unsigned source_alignment,dest_alignment,opa,recolor,scale_x,scale_y;
    lv_area_t image,clip,layer;
  } sample;
} g_blit = {.enabled=true};

uint64_t glass_video_profile_now(void)
{
  return g_blit.profiling ? (uint64_t)esp_timer_get_time() : 0;
}

void glass_video_profile_add(unsigned kind,uint64_t started)
{
  if(started && kind<8){
    g_blit.us[kind]+=(uint64_t)esp_timer_get_time()-started;
    g_blit.calls[kind]++;
  }
}

void glass_video_blit_bind(const void *source)
{
  g_blit.source=source;
}

void glass_video_blit_release(void)
{
  g_blit.source=NULL;
  if(g_blit.fd_plus_one)close(g_blit.fd_plus_one-1);
  g_blit.fd_plus_one=0;
  g_blit.failed=false;
}

void glass_video_blit_set_enabled(bool enabled)
{
  g_blit.enabled=enabled;
}

bool glass_video_blit_cover_source(const void *source)
{
  if(!g_blit.enabled || !g_blit.source || source!=g_blit.source)return false;
  const lv_image_dsc_t *bound=g_blit.source;
  if(!bound->data || bound->header.cf!=LV_COLOR_FORMAT_RGB565 ||
     bound->header.w!=1024 || bound->header.h!=600 ||
     bound->header.stride!=2048 || bound->data_size!=600*2048)return false;
  g_blit.covers++;
  return true;
}

void glass_video_profile_reset(void)
{
  memset(g_blit.us,0,sizeof(g_blit.us));
  memset(g_blit.calls,0,sizeof(g_blit.calls));
  g_blit.dma_us=0;
  g_blit.image_dma=g_blit.copy_dma=g_blit.rejected=g_blit.errors=0;
  g_blit.clears=0;g_blit.clear_us=0;g_blit.covers=0;
  g_blit.bound_calls=g_blit.source_calls=0;
  memset(&g_blit.sample,0,sizeof(g_blit.sample));
  g_blit.started=(uint64_t)esp_timer_get_time();
  g_blit.profiling=true;
}

void glass_video_profile_report(void (*emit)(const char *,...))
{
  static const char *const names[]={"image","blend","copy","fill",
                                  "transform","sync","flush","shadow"};
  uint64_t elapsed=(uint64_t)esp_timer_get_time()-g_blit.started;
  /* Stop before serial formatting. Nested stages are inclusive, not additive. */
  g_blit.profiling=false;
  emit("VIDEO_DRAW elapsed_us=%llu enabled=%u image_dma=%u copy_dma=%u dma_us=%llu rejected=%u errors=%u\n",
       (unsigned long long)elapsed,g_blit.enabled,g_blit.image_dma,
       g_blit.copy_dma,(unsigned long long)g_blit.dma_us,
       g_blit.rejected,g_blit.errors);
  emit("VIDEO_SOURCE bound=%u bound_calls=%u source_calls=%u\n",g_blit.source!=NULL,g_blit.bound_calls,g_blit.source_calls);
  emit("VIDEO_FILL clears=%u clear_us=%llu\n",g_blit.clears,(unsigned long long)g_blit.clear_us);
  emit("VIDEO_COVER hits=%u\n",g_blit.covers);
  if(g_blit.sample.valid){
    emit("VIDEO_LAYOUT src=%u/%u/%u/%u/%u dst=%u/%u/%u/%u/%u align=%u/%u identity=%u/%u\n",
      g_blit.sample.sw,g_blit.sample.sh,g_blit.sample.ss,g_blit.sample.sbytes,g_blit.sample.scf,
      g_blit.sample.dw,g_blit.sample.dh,g_blit.sample.ds,g_blit.sample.dbytes,g_blit.sample.dcf,
      g_blit.sample.source_alignment,g_blit.sample.dest_alignment,
      g_blit.sample.same_descriptor,g_blit.sample.same_data);
    emit("VIDEO_AREA image=%ld/%ld/%ld/%ld clip=%ld/%ld/%ld/%ld layer=%ld/%ld/%ld/%ld\n",
      (long)g_blit.sample.image.x1,(long)g_blit.sample.image.y1,(long)g_blit.sample.image.x2,(long)g_blit.sample.image.y2,
      (long)g_blit.sample.clip.x1,(long)g_blit.sample.clip.y1,(long)g_blit.sample.clip.x2,(long)g_blit.sample.clip.y2,
      (long)g_blit.sample.layer.x1,(long)g_blit.sample.layer.y1,(long)g_blit.sample.layer.x2,(long)g_blit.sample.layer.y2);
    emit("VIDEO_STYLE opa=%u recolor=%u scale=%u/%u\n",g_blit.sample.opa,g_blit.sample.recolor,
      g_blit.sample.scale_x,g_blit.sample.scale_y);
  }
  for(unsigned i=0;i<8;i++)
    emit("VIDEO_STAGE name=%s calls=%u us=%llu\n",names[i],
         g_blit.calls[i],(unsigned long long)g_blit.us[i]);
}

static int copy_rgb565(void *dest,size_t dest_size,unsigned dest_width,
                      unsigned dest_height,unsigned dest_y,
                      const void *src,size_t src_size,unsigned height)
{
  if(!g_blit.enabled || !g_blit.source || g_blit.failed ||
     !src || !dest || ((uintptr_t)src&127) || ((uintptr_t)dest&127) ||
     dest_width!=1024 || dest_height!=600 || !height ||
     height>dest_height || dest_y>dest_height-height ||
     src_size!=(size_t)dest_width*height*2 ||
     dest_size!=(size_t)dest_width*dest_height*2 ||
     (uintptr_t)src>UINTPTR_MAX-src_size ||
     (uintptr_t)dest>UINTPTR_MAX-dest_size ||
     ((uintptr_t)src<(uintptr_t)dest+dest_size &&
      (uintptr_t)dest<(uintptr_t)src+src_size))return 0;
  if(!g_blit.fd_plus_one){
    int fd=open("/dev/ppa0",O_RDWR);
    if(fd<0){g_blit.failed=true;g_blit.errors++;return 0;}
    g_blit.fd_plus_one=fd+1;
  }
  struct p4_ppa_job_s job={.input=src,.output=dest,
    .input_size=src_size,.output_size=dest_size,
    .width=dest_width,.height=height,.out_width=dest_width,
    .out_height=dest_height,.out_y=dest_y,.scale256=256};
  uint64_t start=(uint64_t)esp_timer_get_time();
  int ret=ioctl(g_blit.fd_plus_one-1,P4_PPA_SRM_RGB565,(unsigned long)&job);
  g_blit.dma_us+=(uint64_t)esp_timer_get_time()-start;
  if(ret<0){g_blit.failed=true;g_blit.errors++;return 0;}
  return 1;
}

int glass_video_blit_image(void *unit,const void *image,const void *decoded,
                           const void *coords,const void *clip)
{
  lv_draw_unit_t *draw_unit=unit;
  const lv_draw_image_dsc_t *dsc=image;
  const lv_image_decoder_dsc_t *decoder=decoded;
  const lv_area_t *area=coords,*clipped=clip;
  if(!g_blit.source)return 0;
  g_blit.bound_calls++;
  const lv_image_dsc_t *bound=g_blit.source;
  const lv_draw_buf_t *candidate=decoder->decoded;
  lv_layer_t *layer=draw_unit->target_layer;
  lv_draw_buf_t *dest=layer->draw_buf;
  if(g_blit.profiling && !g_blit.sample.valid && candidate && dest &&
     candidate->header.w==1024 && candidate->header.h==600){
    g_blit.sample.valid=true;
    g_blit.sample.same_descriptor=dsc->src==g_blit.source;
    g_blit.sample.same_data=candidate->data==bound->data;
    g_blit.sample.sw=candidate->header.w;g_blit.sample.sh=candidate->header.h;
    g_blit.sample.ss=candidate->header.stride;g_blit.sample.sbytes=candidate->data_size;g_blit.sample.scf=candidate->header.cf;
    g_blit.sample.dw=dest->header.w;g_blit.sample.dh=dest->header.h;
    g_blit.sample.ds=dest->header.stride;g_blit.sample.dbytes=dest->data_size;g_blit.sample.dcf=dest->header.cf;
    g_blit.sample.source_alignment=(uintptr_t)candidate->data&127;
    g_blit.sample.dest_alignment=(uintptr_t)dest->data&127;
    g_blit.sample.image=*area;g_blit.sample.clip=*clipped;g_blit.sample.layer=layer->buf_area;
    g_blit.sample.opa=dsc->opa;g_blit.sample.recolor=dsc->recolor_opa;
    g_blit.sample.scale_x=dsc->scale_x;g_blit.sample.scale_y=dsc->scale_y;
  }
  if(dsc->src!=g_blit.source &&
     (!decoder->decoded || decoder->decoded->data!=bound->data))return 0;
  g_blit.source_calls++;
  if(!g_blit.enabled)return 0;
  const lv_draw_buf_t *src=decoder->decoded;
  if(!src || !dest || dsc->rotation || dsc->scale_x!=256 ||
     dsc->scale_y!=256 || dsc->skew_x || dsc->skew_y || dsc->tile ||
     dsc->clip_radius || dsc->bitmap_mask_src || dsc->opa<LV_OPA_MAX ||
     dsc->recolor_opa>LV_OPA_MIN || dsc->blend_mode!=LV_BLEND_MODE_NORMAL ||
     src->header.cf!=LV_COLOR_FORMAT_RGB565 ||
     dest->header.cf!=LV_COLOR_FORMAT_RGB565 ||
     src->header.w!=1024 || src->header.h!=600 ||
     src->header.stride!=2048 || dest->header.w!=1024 ||
     dest->header.h!=600 || dest->header.stride!=2048 ||
     area->x1!=0 || area->y1!=0 || area->x2!=1023 || area->y2!=599 ||
     memcmp(area,clipped,sizeof(*area)) ||
     memcmp(area,&layer->buf_area,sizeof(*area))){
    g_blit.rejected++;return 0;
  }
  if(copy_rgb565(dest->data,dest->data_size,1024,600,0,
                 src->data,src->data_size,600)){
    g_blit.image_dma++;return 1;
  }
  return 0;
}

int glass_video_blit_copy(void *destination,const void *dest_rectangle,
                          const void *source,const void *src_rectangle)
{
  lv_draw_buf_t *dest=destination;
  const lv_draw_buf_t *src=source;
  const lv_area_t *da=dest_rectangle,*sa=src_rectangle;
  if(!g_blit.enabled || !g_blit.source || !dest || !src ||
     dest->header.cf!=LV_COLOR_FORMAT_RGB565 ||
     src->header.cf!=LV_COLOR_FORMAT_RGB565 ||
     dest->header.w!=1024 || dest->header.h!=600 ||
     src->header.w!=1024 || src->header.h!=600 ||
     dest->header.stride!=2048 || src->header.stride!=2048)return 0;
  lv_area_t full={0,0,1023,599};if(!da)da=&full;if(!sa)sa=&full;
  if(da->x1 || sa->x1 || da->x2!=1023 || sa->x2!=1023 ||
     da->y1<0 || sa->y1<0 || da->y2>=600 || sa->y2>=600 ||
     da->y2-da->y1!=sa->y2-sa->y1 || da->y2-da->y1<63)return 0;
  unsigned height=da->y2-da->y1+1;
  if(copy_rgb565(dest->data,dest->data_size,1024,600,da->y1,
      src->data+(size_t)sa->y1*2048,(size_t)height*2048,height)){
    g_blit.copy_dma++;return 1;
  }
  return 0;
}

int glass_video_blit_fill(void *unit,const void *fill,const void *coords)
{
  if(!g_blit.enabled || !g_blit.source || !unit || !fill || !coords)return 0;
  const lv_draw_unit_t *draw_unit=unit;
  const lv_draw_fill_dsc_t *dsc=fill;
  const lv_area_t *area=coords;
  const lv_layer_t *layer=draw_unit->target_layer;
  if(!layer || !layer->draw_buf || !draw_unit->clip_area)return 0;
  lv_draw_buf_t *dest=layer->draw_buf;
  const lv_image_dsc_t *bound=g_blit.source;
  if(!dest->data || !bound->data || !bound->data_size || dsc->opa<LV_OPA_MAX || dsc->radius ||
     dsc->grad.dir!=LV_GRAD_DIR_NONE || dsc->color.red || dsc->color.green || dsc->color.blue ||
     dest->header.cf!=LV_COLOR_FORMAT_RGB565 || dest->header.w!=1024 ||
     dest->header.h!=600 || dest->header.stride!=2048 || dest->data_size!=600*2048 ||
     area->x1 || area->y1 || area->x2!=1023 || area->y2!=599 ||
     memcmp(area,draw_unit->clip_area,sizeof(*area)) || memcmp(area,&layer->buf_area,sizeof(*area)) ||
     (uintptr_t)dest->data>UINTPTR_MAX-dest->data_size ||
     (uintptr_t)bound->data>UINTPTR_MAX-bound->data_size ||
     ((uintptr_t)dest->data<(uintptr_t)bound->data+bound->data_size &&
      (uintptr_t)bound->data<(uintptr_t)dest->data+dest->data_size))return 0;
  /* Exact opaque RGB565 black, full clip, no gradients or rounded edges.
   * LVGL still owns this draw page and subsequent overlay ordering. */
  uint64_t start=(uint64_t)esp_timer_get_time();
  memset(dest->data,0,dest->data_size);
  g_blit.clear_us+=(uint64_t)esp_timer_get_time()-start;
  g_blit.clears++;
  return 1;
}
#endif
