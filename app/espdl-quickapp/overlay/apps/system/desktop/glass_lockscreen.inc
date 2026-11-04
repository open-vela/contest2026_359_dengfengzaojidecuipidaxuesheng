/* SPDX-License-Identifier: Apache-2.0
 * Touch lock, not an authentication boundary. The overlay preserves the page;
 * the service owns playback. Only changed labels/progress are invalidated.
 */
#include "glass_lock_photo.h"
#include "glass_lock_art.h"
static unsigned g_lock_page,g_lock_target;
static bool g_lock_page_pending;
static void lock_page_async(void *unused);
static void lock_weather_update(void);
static void lock_photo_update(void);
static struct {
  lv_obj_t *root,*player,*clock,*calendar,*title,*artist,*status,*lyric[3],*play,*prev,*next;
  lv_obj_t *progress,*elapsed,*duration;
  lv_obj_t *weather_location,*weather_condition,*weather_temp,*weather_meta,*weather_report,*weather_icon,*weather_degree;
  lv_obj_t *photo_status,*photo_date;
  struct glass_weather_snapshot weather;
  bool weather_valid,photo_requested,photo_done;
  lv_timer_t *timer;
  struct music_now_playing *shown;
  struct music_cover_view cover;
  time_t second;
  struct tm local;
  bool time_valid,valid;
} g_lock_ui;

static void lock_text_set(lv_obj_t *label,const char *before,const char *after,bool force)
{ if(label && (force || strcmp(before,after)))lv_label_set_text(label,after); }

static void lock_date_format(char *text,size_t size,const struct tm *local,enum glass_language language)
{
  if(language==GLASS_LANGUAGE_ZH_CN){
    static const char *week[]={"日","一","二","三","四","五","六"};
    snprintf(text,size,"%d月%d日  星期%s",local->tm_mon+1,local->tm_mday,week[local->tm_wday]);
  }else{
    static const char *week[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char *month[]={"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    snprintf(text,size,"%s, %s %d",week[local->tm_wday],month[local->tm_mon],local->tm_mday);
  }
}
static void lock_date_set(lv_obj_t *label,const struct tm *local)
{char text[80];lock_date_format(text,sizeof(text),local,glass_i18n_language());lv_label_set_text(label,text);}

/* Only minute/date changes invalidate the quiet clock face. */
static void lock_clock_update(time_t utc)
{
  if(g_lock_ui.second==utc && g_lock_ui.valid)return;
  g_lock_ui.second=utc;time_t shifted=utc+GLASS_CLOCK_OFFSET_SECONDS;struct tm local;
  bool valid=gmtime_r(&shifted,&local) && local.tm_year>=120;
  bool day_changed=valid!=g_lock_ui.time_valid || !g_lock_ui.valid || (valid &&
    (local.tm_year!=g_lock_ui.local.tm_year || local.tm_yday!=g_lock_ui.local.tm_yday));
  bool minute_changed=day_changed || (valid &&
    (local.tm_hour!=g_lock_ui.local.tm_hour || local.tm_min!=g_lock_ui.local.tm_min));
  if(minute_changed){
    if(valid)lv_label_set_text_fmt(g_lock_ui.clock,"%02d:%02d",local.tm_hour,local.tm_min);
    else lv_label_set_text(g_lock_ui.clock,"--:--");
  }
  if(day_changed){
    if(valid)lock_date_set(g_lock_ui.calendar,&local);
    else lv_label_set_text(g_lock_ui.calendar,glass_tr("等待校时"));
  }
  if(valid)g_lock_ui.local=local;
  g_lock_ui.time_valid=valid;
}

static void lock_refresh(lv_timer_t *timer)
{
  LV_UNUSED(timer);if(!glass_ui_can_paint(g_lock_ui.root))return;
  if(g_lock_page==0)lock_clock_update(time(NULL));
  else if(g_lock_page==2){lock_weather_update();return;}
  else if(g_lock_page==3){lock_photo_update();return;}
  struct music_now_playing now;glass_music_now_playing(&now);
  if(g_lock_page==1)music_cover_tick(&g_lock_ui.cover,&now);
  struct music_now_playing *old=g_lock_ui.shown;bool first=!g_lock_ui.valid;
  if(!first && old->revision==now.revision)return;
  if(g_lock_page==0 && (first || now.state!=old->state || !!now.name[0]!=!!old->name[0])) {
    bool playing=now.name[0] && (now.state==MUSIC_PLAYING || now.state==MUSIC_LOADING || now.state==MUSIC_PAUSED);
    if(playing)lv_obj_remove_flag(g_lock_ui.player,LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(g_lock_ui.player,LV_OBJ_FLAG_HIDDEN);
  }
  if(first || strcmp(old->name,now.name))lv_label_set_text(g_lock_ui.title,now.name[0]?now.name:glass_tr("此刻，安静也很好"));
  if(first || strcmp(old->artist,now.artist) || !!old->name[0]!=!!now.name[0])
    lv_label_set_text(g_lock_ui.artist,!now.name[0]?glass_tr("让喜欢的声音，陪伴此刻"):now.artist[0]?now.artist:glass_tr("未知歌手"));
  const char *status=now.state==MUSIC_ERROR?glass_tr("播放失败 · 可点击播放重试"):now.state==MUSIC_LOADING?
    (now.pause_requested?glass_tr("加载中 · 已暂停"):glass_tr("正在加载音乐")):now.state==MUSIC_PAUSED?glass_tr("已暂停"):
    now.state==MUSIC_PLAYING?"":now.state==MUSIC_ENDED?glass_tr("播放结束"):now.state==MUSIC_STOPPED?glass_tr("已停止"):glass_tr("音乐待播放");
  if(g_lock_ui.status && (first || now.state!=old->state || now.pause_requested!=old->pause_requested))lv_label_set_text(g_lock_ui.status,status);
  const char *fallback=!now.name[0]?glass_tr("让喜欢的声音，陪伴此刻"):now.lyrics_state==MUSIC_LYRICS_LOADING?
    glass_tr("正在获取歌词…"):now.lyrics_state==MUSIC_LYRICS_READY?(now.lyrics_started?glass_tr("间奏"):glass_tr("前奏 · 等待歌词")):"";
  if(g_lock_ui.lyric[1] && (first || now.lyrics_state!=old->lyrics_state || strcmp(now.lyrics[1],old->lyrics[1]) || now.lyrics_started!=old->lyrics_started || strcmp(now.name,old->name)))
    lv_label_set_text(g_lock_ui.lyric[1],now.lyrics[1][0]?now.lyrics[1]:fallback);
  lock_text_set(g_lock_ui.lyric[0],old->lyrics[0],now.lyrics[0],first);
  lock_text_set(g_lock_ui.lyric[2],old->lyrics[2],now.lyrics[2],first);
  if(first || now.state!=old->state || now.pause_requested!=old->pause_requested) {
    lv_label_set_text(g_lock_ui.play,(now.state==MUSIC_PLAYING || now.state==MUSIC_LOADING) && !now.pause_requested?LV_SYMBOL_PAUSE:LV_SYMBOL_PLAY);
    if(g_lock_page==1)lv_obj_invalidate(lv_obj_get_parent(g_lock_ui.play));
  }
  if(first || now.can_skip!=old->can_skip) {
    if(now.can_skip){lv_obj_remove_state(g_lock_ui.prev,LV_STATE_DISABLED);lv_obj_remove_state(g_lock_ui.next,LV_STATE_DISABLED);}
    else{lv_obj_add_state(g_lock_ui.prev,LV_STATE_DISABLED);lv_obj_add_state(g_lock_ui.next,LV_STATE_DISABLED);}
  }
  if(g_lock_ui.elapsed && (first || now.position_ms/1000!=old->position_ms/1000))
    lv_label_set_text_fmt(g_lock_ui.elapsed,"%02u:%02u",now.position_ms/60000,now.position_ms/1000%60);
  if(g_lock_ui.duration && (first || now.duration_ms/1000!=old->duration_ms/1000 || now.position_ms/1000!=old->position_ms/1000)) {
    unsigned remaining=now.duration_ms>now.position_ms?now.duration_ms-now.position_ms:0;
    if(now.duration_ms)lv_label_set_text_fmt(g_lock_ui.duration,"-%02u:%02u",remaining/60000,remaining/1000%60);
    else lv_label_set_text(g_lock_ui.duration,"--:--");
  }
  unsigned progress=now.duration_ms?(uint64_t)now.position_ms*1000/now.duration_ms:0;
  if(g_lock_ui.progress && (first || now.position_ms!=old->position_ms || now.duration_ms!=old->duration_ms))
    lv_bar_set_value(g_lock_ui.progress,progress>1000?1000:progress,LV_ANIM_OFF);
  *old=now;g_lock_ui.valid=true;
}

static void lock_deleted(lv_event_t *e)
{
  if(g_lock_page_pending){lv_async_call_cancel(lock_page_async,NULL);g_lock_page_pending=false;}
  LV_UNUSED(e);music_cover_reset(&g_lock_ui.cover);glass_lock_photo_cancel();if(g_lock_ui.timer)lv_timer_delete(g_lock_ui.timer);
  free(g_lock_ui.shown);memset(&g_lock_ui,0,sizeof(g_lock_ui));
  if(g_music_ui.timer)lv_timer_resume(g_music_ui.timer);
  glass_status_visibility(false);
  glass_edge_lock(false);
}

static void lock_unlock(lv_event_t *e)
{
  LV_UNUSED(e);if(!g_lock_ui.root)return;
  if(g_lock_page_pending){lv_async_call_cancel(lock_page_async,NULL);g_lock_page_pending=false;}
  lv_indev_t *input=lv_indev_active();if(input)lv_indev_wait_release(input);
  lv_obj_delete(g_lock_ui.root);
}

static void lock_page_set(unsigned page)
{
  if(page>=4 || page==g_lock_page || g_lock_page_pending || !g_lock_ui.root)return;
  g_lock_target=page;
  lv_indev_t *input=lv_indev_active();if(input)lv_indev_wait_release(input);
  if(lv_async_call(lock_page_async,NULL)==LV_RESULT_OK)g_lock_page_pending=true;
}
static void lock_page_clicked(void *context,unsigned page)
{(void)context;lock_page_set(page);}
static void lock_page_async(void *unused)
{
  LV_UNUSED(unused);g_lock_page_pending=false;if(!g_lock_ui.root)return;
  unsigned page=g_lock_target;lock_unlock(NULL);g_lock_page=page;glass_lock_open(NULL);
}
static void lock_gesture(lv_event_t *e)
{
  lv_indev_t *input=lv_indev_active();if(!input)return;
  lv_dir_t dir=lv_indev_get_gesture_dir(input);
  if(dir==LV_DIR_TOP)lock_unlock(e);
  else if(dir==LV_DIR_LEFT)lock_page_set((g_lock_page+1)%4);
  else if(dir==LV_DIR_RIGHT)lock_page_set((g_lock_page+3)%4);
}

static void lock_toggle(lv_event_t *e)
{ LV_UNUSED(e);if(!g_lock_ui.shown||!g_lock_ui.shown->name[0])return;glass_music_toggle();lock_refresh(NULL); }
static void lock_skip(lv_event_t *e)
{ glass_music_skip((intptr_t)lv_event_get_user_data(e));lock_refresh(NULL); }

static lv_obj_t *lock_label(lv_obj_t *parent,const char *text,int x,int y,int w,int size,uint32_t color)
{
  lv_obj_t *label=glass_text(parent,text,x,y,w,size,color);
  lv_obj_set_style_text_font(label,lock_font(lv_obj_get_style_text_font(label,0)),0);
  lv_label_set_long_mode(label,LV_LABEL_LONG_DOT);
  lv_obj_set_height(label,lv_obj_get_style_text_font(label,0)->line_height);
  lv_obj_remove_flag(label,LV_OBJ_FLAG_CLICKABLE);
  return label;
}

static lv_obj_t *lock_button(lv_obj_t *root,const char *symbol,int x,int y,int size,lv_event_cb_t cb,void *data)
{
  lv_obj_t *button=glass_box(root,x,y,size,size,0x343447,size/2);
  glass_clickable(button,cb,data);lv_obj_remove_flag(button,LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_set_style_bg_color(button,lv_color_hex(0x565269),LV_STATE_PRESSED);
  lv_obj_set_style_opa(button,LV_OPA_40,LV_STATE_DISABLED);
  lv_obj_t *label=glass_symbol(button,symbol,0,0,0xf8f3fa);lv_obj_center(label);
  return button;
}

static void lock_clock_page(lv_obj_t *root)
{
  g_lock_ui.clock=glass_number(root,"--:--",64,166,896,true,0x362b29);
  lv_obj_set_style_text_font(g_lock_ui.clock,lock_display(),0);
  lv_obj_set_style_text_align(g_lock_ui.clock,LV_TEXT_ALIGN_CENTER,0);
  g_lock_ui.calendar=lock_label(root,"",112,116,800,20,0x71534a);
  lv_obj_set_style_text_align(g_lock_ui.calendar,LV_TEXT_ALIGN_CENTER,0);
  lv_obj_t *card=glass_box(root,192,400,640,70,0x141b23,18);
  g_lock_ui.player=card;lv_obj_remove_flag(card,LV_OBJ_FLAG_CLICKABLE);
  g_lock_ui.title=lock_label(card,"",24,12,390,20,0xe5e9ee);
  g_lock_ui.artist=lock_label(card,"",24,40,390,16,0x89939f);
  g_lock_ui.prev=lock_button(card,LV_SYMBOL_PREV,440,11,48,lock_skip,(void *)-1);
  lv_obj_set_style_bg_opa(g_lock_ui.prev,LV_OPA_TRANSP,0);
  lv_obj_t *play=lock_button(card,LV_SYMBOL_PLAY,500,11,48,lock_toggle,NULL);
  lv_obj_set_style_bg_opa(play,LV_OPA_TRANSP,0);g_lock_ui.play=lv_obj_get_child(play,0);
  g_lock_ui.next=lock_button(card,LV_SYMBOL_NEXT,560,11,48,lock_skip,(void *)1);
  lv_obj_set_style_bg_opa(g_lock_ui.next,LV_OPA_TRANSP,0);
}

#include "glass_lock_pages.inc"

static void glass_lock_open(lv_event_t *e)
{
  home_music_obscure();
  LV_UNUSED(e);if(g_lock_ui.root)return;
  /* Keep growing lyric snapshots in PSRAM and release them with the overlay;
   * internal static RAM is reserved for DMA and the kernel. */
  g_lock_ui.shown=calloc(1,sizeof(*g_lock_ui.shown));
  if(!g_lock_ui.shown){show_toast(glass_tr("内存不足，稍后再试"));return;}
  desktop_navigation_cancel();
  music_full_close(NULL);
  toast_cancel();dialog_cancel();glass_status_visibility(true);
  glass_lock_cover_cancel(GLASS_COVER_MUSIC);g_music_ui.cover.requested=false;
  if(g_music_ui.timer)lv_timer_pause(g_music_ui.timer);
  lv_obj_t *root=glass_box(lv_layer_top(),0,0,1024,600,0x000000,0);
  g_lock_ui.root=root;lv_obj_add_flag(root,LV_OBJ_FLAG_CLICKABLE);
  glass_edge_lock(true);
  lv_obj_remove_flag(root,LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(root,lock_deleted,LV_EVENT_DELETE,NULL);
  lv_obj_add_event_cb(root,lock_gesture,LV_EVENT_GESTURE,NULL);
  lock_art_background(root,g_lock_page);
  if(g_lock_page==1)lock_music_page(root);
  else if(g_lock_page==2)lock_weather_page(root);
  else if(g_lock_page==3)lock_photo_page(root);
  else lock_clock_page(root);
  lv_obj_t *pager=glass_pager_create(root,g_lock_page==0?0xd9b69a:0x17171c,g_lock_page==0?0x49352d:0xf5f5f7,g_lock_page==0?0x846351:0x8e8e95,lock_page_clicked,NULL);
  glass_pager_update(pager,g_lock_page,4,true);
  if(pager){
    /* One home rail carries both page position and unlock affordance. */
    lv_obj_set_height(pager,48);
    lv_obj_align(pager,LV_ALIGN_BOTTOM_MID,0,-4);
    struct glass_pager *p=lv_obj_get_user_data(pager);
    lv_obj_set_style_bg_opa(p->face,LV_OPA_TRANSP,0);
    uint32_t ink=g_lock_page==0?0x715349:0xb9cbd3;
    lv_obj_t *rail=glass_box(pager,10,27,92,4,ink,2);
    lv_obj_set_style_bg_opa(rail,70,0);
    lv_obj_remove_flag(rail,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_to_index(rail,0);
    for(unsigned i=0;i<4;i++){
      lv_obj_set_height(p->slots[i].hit,48);
      lv_obj_set_style_bg_color(p->slots[i].hit,lv_color_hex(ink),LV_STATE_PRESSED);
      lv_obj_set_style_bg_opa(p->slots[i].hit,LV_OPA_10,LV_STATE_PRESSED);
      lv_obj_set_style_radius(p->slots[i].hit,12,LV_STATE_PRESSED);
      lv_obj_set_pos(p->slots[i].mark,2,27);
      lv_obj_set_size(p->slots[i].mark,20,4);
      lv_obj_set_style_radius(p->slots[i].mark,2,0);
      if(i==g_lock_page){
        lv_obj_set_style_bg_color(p->slots[i].mark,lv_color_hex(ink),0);
        lv_obj_add_event_cb(p->slots[i].hit,lock_unlock,LV_EVENT_CLICKED,NULL);
      }else lv_obj_add_flag(p->slots[i].mark,LV_OBJ_FLAG_HIDDEN);
    }
  }
  g_lock_ui.timer=lv_timer_create(lock_refresh,250,NULL);lock_refresh(NULL);
}
