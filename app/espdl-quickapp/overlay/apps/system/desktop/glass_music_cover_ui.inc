/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_music.h"
#include "glass_lock_photo.h"
#include "glass_music_favorite_cache.h"
struct music_cover_view {
  lv_obj_t *host,*canvas,*thumbnail_host,*thumbnail;unsigned owner,track_id;
  bool requested,done;char url[1024];
  uint32_t retry_at;
  int error;
};
static void music_cover_deleted(lv_event_t *e)
{
  lv_image_cache_drop(lv_canvas_get_image(lv_event_get_target(e)));
  free(lv_event_get_user_data(e));
}
static void music_cover_reset(struct music_cover_view *view)
{
  glass_lock_cover_cancel(view->owner);
  /* The thumbnail borrows the primary canvas descriptor and pixels. Retire
   * it first; only the primary canvas owns and frees the decoded buffer. */
  if(view->thumbnail){lv_obj_delete(view->thumbnail);view->thumbnail=NULL;}
  if(view->canvas){lv_obj_delete(view->canvas);view->canvas=NULL;}
  view->requested=view->done=false;view->retry_at=0;view->error=0;
}
static void music_cover_place(lv_obj_t *image,lv_obj_t *host,unsigned width,unsigned height)
{
  int w=lv_obj_get_width(host),h=lv_obj_get_height(host);
  unsigned scale=((unsigned)w*256+width-1)/width;
  if((uint64_t)height*scale/256<(unsigned)h)scale=((unsigned)h*256+height-1)/height;
  lv_image_set_pivot(image,0,0);lv_image_set_scale(image,scale);
  lv_obj_set_pos(image,(w-(int)(width*scale/256))/2,(h-(int)(height*scale/256))/2);
  lv_obj_remove_flag(image,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_clip_corner(host,true,0);
}
static void music_cover_tick(struct music_cover_view *view,const struct music_now_playing *now)
{
  if(!view->host)return;
  /* Stop advances playback's cancellation generation without changing the
   * selected artwork. Keep its canvas (or pending same-URL decode) intact. */
  if(strcmp(view->url,now->cover_url) ||
     (view->track_id!=now->track_id && now->state!=MUSIC_STOPPED)) {
    music_cover_reset(view);view->track_id=now->track_id;memcpy(view->url,now->cover_url,sizeof(view->url));
  }
  view->track_id=now->track_id;
  if(!view->url[0])return;
  if(view->done) {
    if(view->canvas||strncmp(view->url,MUSIC_FAVORITE_COVER_SCHEME,sizeof(MUSIC_FAVORITE_COVER_SCHEME)-1)||
       (int32_t)(lv_tick_get()-view->retry_at)<0)return;
    view->requested=view->done=false;
  }
  if(!view->requested) {
    int ret=glass_lock_cover_request(view->owner,view->url);
    if(ret==-EBUSY)return;
    if(ret){view->error=ret;view->done=true;return;}
    view->requested=true;
  }
  struct glass_lock_photo cover;if(!glass_lock_cover_take(view->owner,&cover))return;
  view->done=true;
  if(cover.error || !cover.pixels || !cover.width || !cover.height){view->error=cover.error?cover.error:-EIO;free(cover.pixels);view->retry_at=lv_tick_get()+5000;return;}
  view->error=0;
  lv_obj_update_layout(view->host);
  lv_obj_t *canvas=lv_canvas_create(view->host);view->canvas=canvas;
  if(!canvas){free(cover.pixels);return;}
  lv_canvas_set_buffer(canvas,cover.pixels,cover.width,cover.height,LV_COLOR_FORMAT_RGB565);
  music_cover_place(canvas,view->host,cover.width,cover.height);
  lv_obj_add_event_cb(canvas,music_cover_deleted,LV_EVENT_DELETE,cover.pixels);
  if(view->thumbnail_host){
    lv_obj_update_layout(view->thumbnail_host);
    view->thumbnail=lv_image_create(view->thumbnail_host);
    if(view->thumbnail){
      lv_image_set_src(view->thumbnail,lv_canvas_get_image(canvas));
      music_cover_place(view->thumbnail,view->thumbnail_host,cover.width,cover.height);
    }
  }
}
