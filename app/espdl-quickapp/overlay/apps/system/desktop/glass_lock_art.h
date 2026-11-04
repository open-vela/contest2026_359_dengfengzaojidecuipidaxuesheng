/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <lvgl/src/draw/lv_image_decoder_private.h>
#include "glass_lock_art_codec.h"
#include "qpk_mjpeg.h"
#include "glass_material.h"
#ifdef GROK_HOST_TEST
static bool lock_art_test_fail;
#endif
/* GUI-owned immutable compressed source. Only the requested scanline is
 * decoded; close releases it. No full-screen decoded pixel cache is retained. */
struct lock_art_source {lv_image_dsc_t image;uint32_t magic;struct lock_art_rle rle;};
struct lock_art_decode {lv_draw_buf_t *line;};
static unsigned lock_art_bytes,lock_art_decode_sessions,lock_art_line_peak;
static lv_image_decoder_t *lock_art_decoder;
static const struct lock_art_source *lock_art_source_get(const lv_image_decoder_dsc_t *dsc)
{
 if(dsc->src_type!=LV_IMAGE_SRC_VARIABLE||!dsc->src)return NULL;
 const lv_image_dsc_t *image=dsc->src;
 if(image->header.cf!=LV_COLOR_FORMAT_RAW||image->data_size!=sizeof(struct lock_art_source)||image->data!=(const uint8_t *)image)return NULL;
 const struct lock_art_source *source=dsc->src;
 return source->magic==0x4c415231u&&source->rle.page<4?source:NULL;
}
static lv_result_t lock_art_info(lv_image_decoder_t *decoder,lv_image_decoder_dsc_t *dsc,lv_image_header_t *header)
{
 LV_UNUSED(decoder);const struct lock_art_source *source=lock_art_source_get(dsc);if(!source)return LV_RESULT_INVALID;
 *header=source->image.header;header->cf=LV_COLOR_FORMAT_RGB565;return LV_RESULT_OK;
}
static lv_result_t lock_art_open(lv_image_decoder_t *decoder,lv_image_decoder_dsc_t *dsc)
{
 LV_UNUSED(decoder);if(!lock_art_source_get(dsc))return LV_RESULT_INVALID;
 struct lock_art_decode *state=calloc(1,sizeof(*state));if(!state)return LV_RESULT_INVALID;
 dsc->args.no_cache=true;dsc->user_data=state;dsc->decoded=NULL;lock_art_decode_sessions++;return LV_RESULT_OK;
}
static lv_result_t lock_art_area(lv_image_decoder_t *decoder,lv_image_decoder_dsc_t *dsc,const lv_area_t *full,lv_area_t *area)
{
 LV_UNUSED(decoder);const struct lock_art_source *source=lock_art_source_get(dsc);struct lock_art_decode *state=dsc->user_data;
 if(!source||!state||full->x1<0||full->y1<0||full->x2>=1024||full->y2>=600||full->x1>full->x2||full->y1>full->y2)return LV_RESULT_INVALID;
 unsigned width=(unsigned)lv_area_get_width(full);
 if(area->y1==LV_COORD_MIN){
  lv_draw_buf_t *line=lv_draw_buf_reshape(state->line,LV_COLOR_FORMAT_RGB565,width,1,LV_STRIDE_AUTO);
  if(!line){if(state->line)lv_draw_buf_destroy(state->line);state->line=NULL;
   line=lv_draw_buf_create(width,1,LV_COLOR_FORMAT_RGB565,LV_STRIDE_AUTO);if(!line)return LV_RESULT_INVALID;state->line=line;
  }
  *area=*full;area->y2=area->y1;
 }else{area->y1++;area->y2++;}
 if(area->y1>full->y2)return LV_RESULT_INVALID;
 if(!lock_art_rle_row(&source->rle,(unsigned)area->y1,(unsigned)area->x1,width,(uint16_t *)state->line->data))return LV_RESULT_INVALID;
 unsigned bytes=state->line->header.stride;if(bytes>lock_art_line_peak)lock_art_line_peak=bytes;
 dsc->decoded=state->line;return LV_RESULT_OK;
}
static void lock_art_close(lv_image_decoder_t *decoder,lv_image_decoder_dsc_t *dsc)
{
 LV_UNUSED(decoder);struct lock_art_decode *state=dsc->user_data;if(!state)return;
 if(state->line)lv_draw_buf_destroy(state->line);free(state);dsc->user_data=NULL;dsc->decoded=NULL;lock_art_decode_sessions--;
}
static void lock_art_deleted(lv_event_t *event)
{
 struct lock_art_source *source=lv_event_get_user_data(event);lv_image_cache_drop(&source->image);
 lock_art_bytes-=(unsigned)(sizeof(*source)+lock_art_storage_bytes(&source->rle));lock_art_rle_release(&source->rle);free(source);
}
static void lock_art_background(lv_obj_t *root,unsigned page)
{
 static const uint32_t top[]={0xf2d8bf,0x665044,0x5485a5,0x9eaa8f};
 if(page>=4)return;lv_obj_set_style_bg_color(root,lv_color_hex(top[page]),0);
#ifdef GROK_HOST_TEST
 if(lock_art_test_fail)return;
#endif
 if(!lock_art_decoder){
  lock_art_decoder=lv_image_decoder_create();if(!lock_art_decoder)return;
  lv_image_decoder_set_info_cb(lock_art_decoder,lock_art_info);lv_image_decoder_set_open_cb(lock_art_decoder,lock_art_open);
  lv_image_decoder_set_get_area_cb(lock_art_decoder,lock_art_area);lv_image_decoder_set_close_cb(lock_art_decoder,lock_art_close);
 }
 struct lock_art_source *source=calloc(1,sizeof(*source));if(!source)return;
 unsigned id=GMAT_WALLPAPERS_LOCK_WALLPAPER_0_JPG+page;
 size_t jpeg_bytes=gmat_size(id);void *jpeg_data=gmat_acquire(id,0,jpeg_bytes);unsigned width=0,height=0;
 if(!jpeg_data||qpk_mjpeg_probe(jpeg_data,jpeg_bytes,&width,&height)||width!=512||height!=300){gmat_release(jpeg_data);free(source);return;}
 uint16_t *native=malloc(512u*300u*2u);void *jpeg=native?qpk_mjpeg_create():NULL;
 int result=jpeg?qpk_mjpeg_decode_still(jpeg,jpeg_data,jpeg_bytes,native,512,300):-ENOMEM;
 qpk_mjpeg_destroy(jpeg);gmat_release(jpeg_data);
 if(result){free(native);free(source);return;}
 if(!lock_art_rle_build(&source->rle,page,native,LOCK_ART_RUN_LIMIT)){free(native);free(source);return;}
 source->magic=0x4c415231u;
 source->image.header.magic=LV_IMAGE_HEADER_MAGIC;source->image.header.cf=LV_COLOR_FORMAT_RAW;
 source->image.header.w=1024;source->image.header.h=600;source->image.header.stride=2048;
 source->image.data=(const uint8_t *)source;source->image.data_size=sizeof(*source);
 lv_obj_t *art=lv_image_create(root);if(!art){lock_art_rle_release(&source->rle);free(source);return;}
 lv_image_set_src(art,&source->image);lv_obj_remove_flag(art,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
 lv_obj_add_event_cb(art,lock_art_deleted,LV_EVENT_DELETE,source);
 lock_art_bytes+=(unsigned)(sizeof(*source)+lock_art_storage_bytes(&source->rle));
}

/* Use the stable system font views so an active lock follows font changes. */
static const lv_font_t *lock_font(const lv_font_t *font)
{return font;}
static lv_font_t *lock_display_font;
static const lv_font_t *lock_display(void)
{
#ifndef GROK_HOST_TEST
  if(!lock_display_font)lock_display_font=lv_tiny_ttf_create_data_ex(
    g_desktop_font_start,(size_t)(g_desktop_font_end-g_desktop_font_start),192,LV_FONT_KERNING_NORMAL,24);
#endif
  if(lock_display_font)glass_global_font_bind(lock_display_font,192);
  return lock_display_font?lock_display_font:g_glass.font72;
}
