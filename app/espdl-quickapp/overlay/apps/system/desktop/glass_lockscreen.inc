/* SPDX-License-Identifier: Apache-2.0
 * Touch lock, not an authentication boundary. The overlay preserves the page;
 * the service owns playback. Only changed labels/progress are invalidated.
 */
#include "glass_lock_photo.h"
static unsigned g_lock_page,g_lock_target;
static bool g_lock_page_pending;
static void lock_page_async(void *unused);
static void lock_weather_update(void);
static void lock_photo_update(void);
static struct {
  lv_obj_t *root,*player,*clock,*calendar,*hand[3],*pin,*title,*artist,*status,*lyric[3],*play,*prev,*next;
  lv_obj_t *progress,*elapsed,*duration;
  lv_obj_t *weather_location,*weather_condition,*weather_temp,*weather_meta,*weather_report,*weather_icon;
  lv_obj_t *photo_status,*photo_date;
  struct glass_weather_snapshot weather;
  bool weather_valid,photo_requested,photo_done;
  lv_timer_t *timer;
  struct music_now_playing *shown;
  struct music_cover_view cover;
  time_t second;
  struct tm local;
  lv_point_precise_t points[3][2];
  int angles[3];
  bool time_valid,valid;
} g_lock_ui;

static void lock_text_set(lv_obj_t *label,const char *before,const char *after,bool force)
{ if(label && (force || strcmp(before,after)))lv_label_set_text(label,after); }

static lv_point_t lock_dial_point(int angle,int radius)
{
  return (lv_point_t){210+(int32_t)lv_trigo_sin(angle)*radius/32767,
                     210-(int32_t)lv_trigo_sin(angle+90)*radius/32767};
}

static void lock_draw_text(lv_layer_t *layer,const char *text,int x,int y,int w,
                           const lv_font_t *font,uint32_t color,lv_text_align_t align,bool local)
{
  lv_draw_label_dsc_t d;lv_draw_label_dsc_init(&d);
  d.text=text;d.font=font;d.color=lv_color_hex(color);d.align=align;d.text_local=local;
  lv_area_t area={x,y,x+w-1,y+font->line_height-1};lv_draw_label(layer,&d,&area);
}

static void lock_dial_draw(lv_event_t *e)
{
  lv_layer_t *layer=lv_event_get_layer(e);lv_area_t a;lv_obj_get_coords(lv_event_get_target(e),&a);
  lv_draw_line_dsc_t d;lv_draw_line_dsc_init(&d);d.round_start=d.round_end=1;
  for(int i=0;i<60;i++) {
    lv_point_t p=lock_dial_point(i*6,i%5?184:180),q=lock_dial_point(i*6,194);
    d.p1=(lv_point_precise_t){a.x1+p.x,a.y1+p.y};d.p2=(lv_point_precise_t){a.x1+q.x,a.y1+q.y};
    d.width=i%5?3:4;d.color=lv_color_hex(i%5?0x525255:0xd7d7db);lv_draw_line(layer,&d);
  }
  static const char *numbers[]={"12","1","2","3","4","5","6","7","8","9","10","11"};
  const lv_font_t *font=g_home_font40?g_home_font40:zh_font(28);
  for(int i=0;i<12;i++) {
    lv_point_t p=lock_dial_point(i*30,151);
    lock_draw_text(layer,numbers[i],a.x1+p.x-30,a.y1+p.y-font->line_height/2,60,font,0xf5f5f7,LV_TEXT_ALIGN_CENTER,false);
  }
  if(!g_lock_ui.time_valid)lock_draw_text(layer,"--:--",a.x1+135,a.y1+188,150,zh_font(28),0x8f8f95,LV_TEXT_ALIGN_CENTER,false);
}

static unsigned lock_month_days(unsigned year,unsigned month)
{
  static const uint8_t days[]={31,28,31,30,31,30,31,31,30,31,30,31};
  return days[month]+(month==1 && (!(year%400)||(!(year%4)&&year%100)));
}

static void lock_calendar_draw(lv_event_t *e)
{
  lv_layer_t *layer=lv_event_get_layer(e);lv_area_t a;lv_obj_get_coords(lv_event_get_target(e),&a);
  if(!g_lock_ui.time_valid) {
    lock_draw_text(layer,glass_tr("联网后自动校准时间"),a.x1,a.y1+160,420,zh_font(20),0x8f8f95,LV_TEXT_ALIGN_CENTER,false);return;
  }
  static const char *months[]={"JANUARY","FEBRUARY","MARCH","APRIL","MAY","JUNE","JULY","AUGUST","SEPTEMBER","OCTOBER","NOVEMBER","DECEMBER"};
  const struct tm *t=&g_lock_ui.local;
  lock_draw_text(layer,months[t->tm_mon],a.x1,a.y1,310,zh_font(28),0xff3b44,LV_TEXT_ALIGN_LEFT,false);
  char year[12];snprintf(year,sizeof(year),"%d",t->tm_year+1900);
  lock_draw_text(layer,year,a.x1+320,a.y1+8,100,zh_font(16),0x87878d,LV_TEXT_ALIGN_RIGHT,true);
  static const char *week[]={"S","M","T","W","T","F","S"};
  for(int i=0;i<7;i++)lock_draw_text(layer,week[i],a.x1+i*60,a.y1+56,60,zh_font(20),i==0||i==6?0x86868c:0xf2f2f7,LV_TEXT_ALIGN_CENTER,false);
  int start=(t->tm_wday-(t->tm_mday-1)%7+7)%7;
  unsigned count=lock_month_days(t->tm_year+1900,t->tm_mon);
  const lv_font_t *font=g_home_font32?g_home_font32:zh_font(28);
  for(unsigned day=1;day<=count;day++) {
    int cell=start+day-1,col=cell%7,row=cell/7;
    int cx=a.x1+col*60+30,cy=a.y1+118+row*46;
    if(day==(unsigned)t->tm_mday) {
      lv_draw_rect_dsc_t d;lv_draw_rect_dsc_init(&d);d.bg_color=lv_color_hex(0xff3b44);d.radius=LV_RADIUS_CIRCLE;
      lv_area_t dot={cx-21,cy-21,cx+21,cy+21};lv_draw_rect(layer,&d,&dot);
    }
    char text[4];snprintf(text,sizeof(text),"%u",day);
    lock_draw_text(layer,text,cx-30,cy-font->line_height/2,60,font,
      day==(unsigned)t->tm_mday|| (col!=0&&col!=6)?0xf5f5f7:0x86868c,LV_TEXT_ALIGN_CENTER,true);
  }
}

static void lock_hand_set(unsigned index,int angle)
{
  static const int length[]={98,146,174},tail[]={12,16,18},padding[]={8,8,4};
  lv_point_t p=lock_dial_point(angle+180,tail[index]),q=lock_dial_point(angle,length[index]);
  int x=(p.x<q.x?p.x:q.x)-padding[index],y=(p.y<q.y?p.y:q.y)-padding[index];
  lv_obj_invalidate(g_lock_ui.hand[index]); /* Invalidate the old tight bounds. */
  g_lock_ui.points[index][0]=(lv_point_precise_t){p.x-x,p.y-y};
  g_lock_ui.points[index][1]=(lv_point_precise_t){q.x-x,q.y-y};
  lv_obj_set_pos(g_lock_ui.hand[index],x,y);
  lv_line_set_points(g_lock_ui.hand[index],g_lock_ui.points[index],2);
  g_lock_ui.angles[index]=angle;
}

static void lock_clock_update(time_t utc)
{
  if(g_lock_ui.second==utc && g_lock_ui.valid)return;
  g_lock_ui.second=utc;time_t shifted=utc+GLASS_CLOCK_OFFSET_SECONDS;struct tm local;
  bool valid=gmtime_r(&shifted,&local) && local.tm_year>=120;
  bool day_changed=valid!=g_lock_ui.time_valid || !g_lock_ui.valid || (valid &&
    (local.tm_year!=g_lock_ui.local.tm_year || local.tm_yday!=g_lock_ui.local.tm_yday));
  if(day_changed) {
    lv_obj_invalidate(g_lock_ui.calendar);lv_obj_invalidate(g_lock_ui.clock);
    for(unsigned i=0;i<3;i++) {
      if(valid)lv_obj_remove_flag(g_lock_ui.hand[i],LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(g_lock_ui.hand[i],LV_OBJ_FLAG_HIDDEN);
    }
    if(valid)lv_obj_remove_flag(g_lock_ui.pin,LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(g_lock_ui.pin,LV_OBJ_FLAG_HIDDEN);
  }
  if(valid) {
    int angles[]={local.tm_hour%12*30+local.tm_min/2,local.tm_min*6+local.tm_sec/10,local.tm_sec*6};
    for(unsigned i=0;i<3;i++)if(day_changed||angles[i]!=g_lock_ui.angles[i])lock_hand_set(i,angles[i]);
    g_lock_ui.local=local;
  }
  g_lock_ui.time_valid=valid;
}

static void lock_refresh(lv_timer_t *timer)
{
  LV_UNUSED(timer);if(!g_lock_ui.root)return;
  lv_obj_t *top=lv_obj_get_parent(g_lock_ui.root);
  if(lv_obj_get_child(top,-1)!=g_lock_ui.root)lv_obj_move_foreground(g_lock_ui.root);
  if(g_lock_page==0)lock_clock_update(time(NULL));
  else if(g_lock_page==2){lock_weather_update();return;}
  else if(g_lock_page==3){lock_photo_update();return;}
  struct music_now_playing now;glass_music_now_playing(&now);
  if(g_lock_page==1)music_cover_tick(&g_lock_ui.cover,&now);
  struct music_now_playing *old=g_lock_ui.shown;bool first=!g_lock_ui.valid;
  if(!first && old->revision==now.revision)return;
  if(g_lock_page==0 && (first || !!now.name[0]!=!!old->name[0])) {
    lv_obj_t *items[]={g_lock_ui.player,g_lock_ui.status,g_lock_ui.lyric[1],g_lock_ui.lyric[2]};
    for(unsigned i=0;i<sizeof(items)/sizeof(items[0]);i++) {
      if(now.name[0])lv_obj_remove_flag(items[i],LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(items[i],LV_OBJ_FLAG_HIDDEN);
    }
  }
  if(first || strcmp(old->name,now.name))lv_label_set_text(g_lock_ui.title,now.name[0]?now.name:glass_tr("此刻，安静也很好"));
  if(first || strcmp(old->artist,now.artist))lv_label_set_text(g_lock_ui.artist,now.artist[0]?now.artist:glass_tr("未知歌手"));
  const char *status=now.state==MUSIC_ERROR?glass_tr("播放失败 · 可点击播放重试"):now.state==MUSIC_LOADING?
    (now.pause_requested?glass_tr("加载中 · 已暂停"):glass_tr("正在加载音乐")):now.state==MUSIC_PAUSED?glass_tr("已暂停"):
    now.state==MUSIC_PLAYING?glass_tr("后台播放中"):now.state==MUSIC_ENDED?glass_tr("播放结束"):now.state==MUSIC_STOPPED?glass_tr("已停止"):glass_tr("音乐待播放");
  if(first || now.state!=old->state || now.pause_requested!=old->pause_requested)lv_label_set_text(g_lock_ui.status,status);
  const char *fallback=!now.name[0]?glass_tr("让喜欢的声音，陪伴此刻"):now.lyrics_state==MUSIC_LYRICS_LOADING?
    glass_tr("正在获取歌词…"):now.lyrics_state==MUSIC_LYRICS_READY?(now.lyrics_started?glass_tr("间奏"):glass_tr("前奏 · 等待歌词")):glass_tr("暂无歌词");
  if(first || now.lyrics_state!=old->lyrics_state || strcmp(now.lyrics[1],old->lyrics[1]) || now.lyrics_started!=old->lyrics_started || strcmp(now.name,old->name))
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
  if(first || now.position_ms!=old->position_ms || now.duration_ms!=old->duration_ms)
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
  /* StandBy reference: the dial and calendar each draw as one LVGL object.
   * No sixty tick widgets, calendar cell widgets, canvas or bitmap buffers. */
  lv_obj_set_style_bg_color(root,lv_color_black(),0);
  g_lock_ui.clock=glass_box(root,44,46,420,420,0x000000,0);
  lv_obj_remove_flag(g_lock_ui.clock,LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(g_lock_ui.clock,lock_dial_draw,LV_EVENT_DRAW_MAIN,NULL);
  for(unsigned i=0;i<3;i++) {
    g_lock_ui.hand[i]=lv_line_create(g_lock_ui.clock);
    lv_obj_remove_flag(g_lock_ui.hand[i],LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_line_color(g_lock_ui.hand[i],lv_color_hex(i==2?0xffad0b:0xf5f5f7),0);
    lv_obj_set_style_line_width(g_lock_ui.hand[i],i==2?2:10,0);
    lv_obj_set_style_line_rounded(g_lock_ui.hand[i],true,0);
  }
  g_lock_ui.pin=glass_box(g_lock_ui.clock,202,202,16,16,0xf5f5f7,8);
  lv_obj_remove_flag(g_lock_ui.pin,LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *pin=glass_box(g_lock_ui.pin,3,3,10,10,0xffad0b,5);lv_obj_remove_flag(pin,LV_OBJ_FLAG_CLICKABLE);
  pin=glass_box(pin,3,3,4,4,0x000000,2);lv_obj_remove_flag(pin,LV_OBJ_FLAG_CLICKABLE);
  g_lock_ui.calendar=glass_box(root,534,58,420,400,0x000000,0);
  lv_obj_remove_flag(g_lock_ui.calendar,LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(g_lock_ui.calendar,lock_calendar_draw,LV_EVENT_DRAW_MAIN,NULL);
  g_lock_ui.status=lock_label(root,"",344,460,336,16,0x87878d);
  lv_obj_set_style_text_align(g_lock_ui.status,LV_TEXT_ALIGN_CENTER,0);
  lv_obj_t *card=glass_box(root,48,490,928,82,0x1c1c1e,20);
  g_lock_ui.player=card;lv_obj_remove_flag(card,LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *art=glass_box(card,14,19,44,44,0x38383e,10);lv_obj_remove_flag(art,LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *note=glass_symbol(art,LV_SYMBOL_AUDIO,0,0,0xf2f2f7);lv_obj_center(note);
  g_lock_ui.title=lock_label(card,"",72,12,236,20,0xf5f5f7);
  g_lock_ui.artist=lock_label(card,"",72,45,236,16,0x8e8e95);
  g_lock_ui.lyric[1]=lock_label(card,"",330,12,384,20,0xf5f5f7);
  g_lock_ui.lyric[2]=lock_label(card,"",330,45,384,16,0x8e8e95);
  g_lock_ui.prev=lock_button(card,LV_SYMBOL_PREV,736,23,36,lock_skip,(void *)-1);
  lv_obj_set_style_bg_opa(g_lock_ui.prev,LV_OPA_TRANSP,0);
  lv_obj_t *play=lock_button(card,LV_SYMBOL_PLAY,788,17,48,lock_toggle,NULL);
  lv_obj_set_style_bg_color(play,lv_color_hex(0x38383e),0);g_lock_ui.play=lv_obj_get_child(play,0);
  g_lock_ui.next=lock_button(card,LV_SYMBOL_NEXT,852,23,36,lock_skip,(void *)1);
  lv_obj_set_style_bg_opa(g_lock_ui.next,LV_OPA_TRANSP,0);
  /* The narrow progress rail is read-only, matching the decoder's capabilities. */
  g_lock_ui.progress=lv_bar_create(card);lv_obj_remove_style_all(g_lock_ui.progress);
  lv_obj_set_pos(g_lock_ui.progress,18,77);lv_obj_set_size(g_lock_ui.progress,892,2);
  lv_bar_set_range(g_lock_ui.progress,0,1000);lv_obj_remove_flag(g_lock_ui.progress,LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(g_lock_ui.progress,lv_color_hex(0x3a3a3e),LV_PART_MAIN);
  lv_obj_set_style_bg_opa(g_lock_ui.progress,LV_OPA_COVER,LV_PART_MAIN);
  lv_obj_set_style_bg_color(g_lock_ui.progress,lv_color_hex(0xa7a7ad),LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(g_lock_ui.progress,LV_OPA_COVER,LV_PART_INDICATOR);
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
  if(g_lock_page==1)lock_music_page(root);
  else if(g_lock_page==2)lock_weather_page(root);
  else if(g_lock_page==3)lock_photo_page(root);
  else lock_clock_page(root);
  lv_obj_t *pager=glass_pager_create(root,0x17171c,0xf5f5f7,0x8e8e95,lock_page_clicked,NULL);
  glass_pager_update(pager,g_lock_page,4,true);
  if(pager)lv_obj_align(pager,LV_ALIGN_BOTTOM_MID,0,0);
  lock_label(root,glass_tr("左右切换 · 上滑解锁"),24,576,300,16,0x8e8e95);
  lv_obj_t *unlock=glass_box(root,924,568,76,30,0x000000,12);lv_obj_set_style_bg_opa(unlock,LV_OPA_TRANSP,0);
  glass_clickable(unlock,lock_unlock,NULL);lv_obj_t *hint=lock_label(unlock,glass_tr("解锁"),0,0,70,16,0xc6c6cc);lv_obj_center(hint);
  g_lock_ui.timer=lv_timer_create(lock_refresh,250,NULL);lock_refresh(NULL);
}
