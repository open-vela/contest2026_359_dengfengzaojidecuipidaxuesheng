/* SPDX-License-Identifier: Apache-2.0 */
/* Full-screen presentation only. Playback and lyric ownership stay in music. */
#define MUSIC_FULL_PITCH 76
#define MUSIC_FULL_HEIGHT 372
#define MUSIC_FULL_PAD ((MUSIC_FULL_HEIGHT-MUSIC_FULL_PITCH)/2)
#define MUSIC_FULL_FADE_MS 160
#define MUSIC_FULL_MOVE_MS 220
#define MUSIC_FULL_STEP_MS 18
#define MUSIC_FULL_ARC_WIDTH 112
#define MUSIC_FULL_LINE_WIDTH (538-MUSIC_FULL_ARC_WIDTH)
static const lv_font_t *music_lyric_font(unsigned size)
{
  const lv_font_t *base=size==72&&g_glass.music_font72?g_glass.music_font72:
    size==72&&g_glass.font72?g_glass.font72:size>=48&&g_glass.font48?g_glass.font48:zh_font(size);
  return base;
}
#include "glass_music_pv.inc"
struct music_full_ui {
  lv_obj_t *root,*viewport,*span,*title,*artist,*status,*hint,*follow_button;
  lv_obj_t *rows[MUSIC_LYRICS_WINDOW],*labels[MUSIC_LYRICS_WINDOW];
  lv_obj_t *translations[MUSIC_LYRICS_WINDOW],*bilingual;
  lv_obj_t *play,*favorite,*elapsed,*duration,*progress,*volume;
  lv_obj_t *controls,*previous,*next,*volume_icon;
  lv_obj_t *mode;
  struct music_pv_ui pv;
  struct music_cover_view cover;
  struct music_seek_ui seek;
  struct music_lyrics_window window;
  unsigned track,total;
  unsigned move_ms;
  unsigned row_index[MUSIC_LYRICS_WINDOW];
  lv_opa_t row_opacity[MUSIC_LYRICS_WINDOW];
  bool row_active[MUSIC_LYRICS_WINDOW];
  int current,cascade_delta;
  bool follow,updating,status_hidden,playing,cascade,settle;
  bool palette_valid,palette_pv,palette_favorite;
  uint32_t palette_accent,palette_background,palette_surface,palette_secondary,palette_foreground;
};

static void music_full_text(lv_obj_t *label,const char *text)
{if(strcmp(lv_label_get_text(label),text))lv_label_set_text(label,text);}

static void music_full_lyric_opacity(void *label,int32_t opacity)
{lv_obj_set_style_text_opa(label,(lv_opa_t)opacity,0);}

static void music_full_lyric_offset(void *row,int32_t offset)
{
  lv_obj_t *viewport=lv_obj_get_parent(row);
  int radius=MUSIC_FULL_HEIGHT/2;
  int y=lv_obj_get_style_y(row,0)+MUSIC_FULL_PITCH/2+
    offset-lv_obj_get_scroll_y(viewport)-radius;
  if(y>radius)y=radius;else if(y<-radius)y=-radius;
  /* Project the left half of an ellipse into the lyric column: enter at
   * bottom-right, focus at center-left, leave at top-right. Derive X from
   * the animated screen Y so fast advances retain continuity on both axes.
   * Integer square root avoids float work and rotated text/layer buffers. */
  int x=MUSIC_FULL_ARC_WIDTH-
    MUSIC_FULL_ARC_WIDTH*lv_sqrt32(radius*radius-y*y)/radius;
  lv_obj_set_style_translate_x(row,x,0);
  lv_obj_set_style_translate_y(row,offset,0);
}

static void music_full_row_reset(struct music_full_ui *f,unsigned i)
{
  lv_anim_delete(f->labels[i],music_full_lyric_opacity);
  lv_anim_delete(f->translations[i],music_full_lyric_opacity);
  lv_anim_delete(f->rows[i],music_full_lyric_offset);
  music_full_lyric_offset(f->rows[i],0);
  music_full_lyric_opacity(f->labels[i],LV_OPA_COVER);
  music_full_lyric_opacity(f->translations[i],LV_OPA_COVER);
  f->row_active[i]=false;f->row_opacity[i]=0;
}

static void music_full_draw(void)
{
  struct music_full_ui *f=g_music_ui.full;if(!f||f->updating)return;
  f->updating=true;
  int y=lv_obj_get_scroll_y(f->viewport)-MUSIC_FULL_PAD;
  unsigned first=y>MUSIC_FULL_PITCH?(unsigned)y/MUSIC_FULL_PITCH-1:0;
  glass_music_lyrics_read(first,&f->window);
  if(f->window.track_id!=f->track){f->updating=false;return;}
  bool animate=f->follow&&f->playing&&!g_glass.reduced_motion&&!f->settle;
  f->settle=false;
  bool cascade=f->cascade&&animate;f->cascade=false;
  for(unsigned i=0;i<MUSIC_LYRICS_WINDOW;i++){
    /* A line retains its slot while the viewport moves. Otherwise every
     * scroll boundary would recycle all labels and restart their animations. */
    unsigned line=(i+MUSIC_LYRICS_WINDOW-f->window.first%MUSIC_LYRICS_WINDOW)%MUSIC_LYRICS_WINDOW;
    if(line>=f->window.count){
      music_full_row_reset(f,i);
      lv_obj_add_flag(f->rows[i],LV_OBJ_FLAG_HIDDEN);continue;
    }
    unsigned index=f->window.first+line;
    bool recycled=f->row_index[i]!=index||lv_obj_has_flag(f->rows[i],LV_OBJ_FLAG_HIDDEN);
    if(recycled)music_full_row_reset(f,i);
    lv_obj_remove_flag(f->rows[i],LV_OBJ_FLAG_HIDDEN);
    bool active=(int)index==f->window.current;
    bool entering=active&&(!f->row_active[i]||f->row_index[i]!=index);
    int distance=(int)index-f->window.current;
    unsigned steps=distance<0?(unsigned)-distance:(unsigned)distance;
    lv_opa_t opacity=!f->follow||f->window.current<0||g_glass.reduced_motion?LV_OPA_COVER:
      active?LV_OPA_COVER:steps==1?LV_OPA_70:steps==2?LV_OPA_50:LV_OPA_30;
    lv_obj_set_y(f->rows[i],MUSIC_FULL_PAD+index*MUSIC_FULL_PITCH);
    music_full_text(f->labels[i],f->window.lines[line][0]?f->window.lines[line]:"· · ·");
    const char *translation=g_glass.music_original_only?"":f->window.translations[line];
    lv_label_set_long_mode(f->labels[i],translation[0]?LV_LABEL_LONG_DOT:LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_height(f->labels[i],translation[0]?40:70,0);
    lv_obj_set_style_text_font(f->labels[i],music_lyric_font(active?28:20),0);
    lv_obj_set_style_text_font(f->translations[i],music_lyric_font(16),0);
    lv_obj_set_style_text_color(f->labels[i],lv_color_hex(active?g_music_ui.accent:
      g_desktop.light_theme?0x625b6e:0xbdb5c9),0);
    if(translation[0]){
      lv_obj_align(f->labels[i],LV_ALIGN_TOP_LEFT,0,3);
      music_full_text(f->translations[i],translation);
      lv_obj_set_style_text_color(f->translations[i],lv_color_hex(active?g_music_ui.primary:g_music_ui.secondary),0);
      lv_obj_remove_flag(f->translations[i],LV_OBJ_FLAG_HIDDEN);
    }else{
      lv_obj_align(f->labels[i],LV_ALIGN_LEFT_MID,0,0);
      lv_obj_add_flag(f->translations[i],LV_OBJ_FLAG_HIDDEN);
    }
    if(!animate||recycled){
      if(!recycled)music_full_row_reset(f,i);
      music_full_lyric_opacity(f->labels[i],opacity);
      music_full_lyric_opacity(f->translations[i],opacity);
    }else if(entering||f->row_opacity[i]!=opacity){
      lv_anim_t fade;lv_anim_init(&fade);
      lv_anim_set_exec_cb(&fade,music_full_lyric_opacity);
      int start=lv_obj_get_style_text_opa(f->labels[i],0);
      if(entering&&start>LV_OPA_70)start=LV_OPA_70;
      lv_anim_set_values(&fade,start,opacity);
      lv_anim_set_duration(&fade,f->move_ms<MUSIC_FULL_FADE_MS?f->move_ms:MUSIC_FULL_FADE_MS);
      lv_anim_set_path_cb(&fade,lv_anim_path_ease_out);
      lv_anim_set_var(&fade,f->labels[i]);
      lv_anim_start(&fade);
      lv_anim_set_var(&fade,f->translations[i]);
      lv_anim_start(&fade);
    }
    /* Compensate the viewport's new scroll position before easing each row
     * into place. Retained rows start at their exact previous screen position,
     * including when a fast next line interrupts an unfinished transition. */
    if(cascade){
      lv_anim_t move;lv_anim_init(&move);
      lv_anim_set_var(&move,f->rows[i]);
      lv_anim_set_exec_cb(&move,music_full_lyric_offset);
      lv_anim_set_values(&move,lv_obj_get_style_translate_y(f->rows[i],0)+f->cascade_delta,0);
      unsigned delay=distance<=0?0:distance>3?3:(unsigned)distance;
      delay*=MUSIC_FULL_STEP_MS;
      if(delay>f->move_ms/2)delay=f->move_ms/2;
      lv_anim_set_duration(&move,f->move_ms-delay);
      lv_anim_set_delay(&move,delay);
      lv_anim_set_path_cb(&move,lv_anim_path_ease_out);
      lv_anim_start(&move);
    }else music_full_lyric_offset(f->rows[i],lv_obj_get_style_translate_y(f->rows[i],0));
    f->row_index[i]=index;f->row_active[i]=active;f->row_opacity[i]=opacity;
  }
  f->updating=false;
}

static void music_full_scroll(lv_event_t *event)
{
  struct music_full_ui *f=g_music_ui.full;if(!f)return;
  if(lv_event_get_code(event)==LV_EVENT_SCROLL_BEGIN){
    lv_indev_t *input=lv_indev_active();
    if(input&&lv_indev_get_state(input)==LV_INDEV_STATE_PRESSED){
      f->follow=false;f->cascade=false;
      for(unsigned i=0;i<MUSIC_LYRICS_WINDOW;i++)music_full_row_reset(f,i);
      music_full_text(lv_obj_get_child(f->follow_button,0),glass_tr("恢复跟随"));
    }
  }
  if(lv_event_get_code(event)==LV_EVENT_SCROLL)music_full_draw();
}

static void music_full_follow(lv_event_t *event)
{
  LV_UNUSED(event);struct music_full_ui *f=g_music_ui.full;if(!f)return;
  f->follow=true;f->current=-2;
  struct music_now_playing now;glass_music_now_playing(&now);music_full_refresh(&now);
}

static void music_full_deleted(lv_event_t *event)
{
  struct music_full_ui *f=lv_event_get_user_data(event);
  if(g_music_ui.full==f)g_music_ui.full=NULL;
  if(f->pv.timer)lv_timer_delete(f->pv.timer);
  music_cover_reset(&f->cover);
  if(!f->status_hidden)glass_status_visibility(false);
  free(f);
}

static void music_full_close(lv_event_t *event)
{
  LV_UNUSED(event);
  if(g_music_ui.full)lv_obj_delete(g_music_ui.full->root);
}

static void music_full_palette(struct music_full_ui *f,const struct music_now_playing *now)
{
  bool pv=f->pv.visible;
  uint32_t accent=pv&&f->pv.valid?f->pv.accent:g_music_ui.accent;
  uint32_t background=pv&&f->pv.valid?f->pv.background:pv?(g_desktop.light_theme?0xf5f1e8:0x121518):g_music_ui.background;
  uint32_t surface=pv?lv_color_to_u32(lv_color_mix(lv_color_hex(f->pv.valid?f->pv.foreground:0x888888),lv_color_hex(background),24))&0xffffff:g_music_ui.surface;
  uint32_t secondary=pv&&f->pv.valid?f->pv.secondary:pv?(g_desktop.light_theme?0x74736f:0x999a97):g_music_ui.secondary;
  uint32_t foreground=pv&&f->pv.valid?f->pv.foreground:g_music_ui.primary;
  if(f->palette_valid&&f->palette_pv==pv&&f->palette_favorite==now->favorite&&
     f->palette_accent==accent&&f->palette_background==background&&f->palette_surface==surface&&
     f->palette_secondary==secondary&&f->palette_foreground==foreground)return;
  f->palette_valid=true;f->palette_pv=pv;f->palette_favorite=now->favorite;
  f->palette_accent=accent;f->palette_background=background;f->palette_surface=surface;
  f->palette_secondary=secondary;f->palette_foreground=foreground;
  lv_obj_set_style_bg_color(f->root,lv_color_hex(background),0);
  lv_obj_set_style_bg_color(f->controls,lv_color_hex(surface),0);
  lv_obj_set_style_border_color(f->controls,lv_color_hex(secondary),0);
  lv_obj_set_style_text_color(f->volume_icon,lv_color_hex(secondary),0);
  lv_obj_t *play=lv_obj_get_parent(f->play);
  for(unsigned i=0;i<lv_obj_get_child_count(f->root);i++){
    lv_obj_t *child=lv_obj_get_child(f->root,i);
    if(lv_obj_check_type(child,&lv_button_class)){
      lv_obj_set_style_bg_color(child,lv_color_hex(child==play?accent:surface),0);
      bool header=lv_obj_get_style_y(child,0)==16;
      bool footer=child==play||child==f->previous||child==f->next||child==lv_obj_get_parent(f->favorite);
      lv_obj_set_style_bg_opa(child,footer?(child==play?LV_OPA_20:LV_OPA_TRANSP):header?(child==f->bilingual?LV_OPA_10:LV_OPA_TRANSP):LV_OPA_COVER,0);
      lv_obj_t *label=lv_obj_get_child(child,0);
      if(label){
        uint32_t color=foreground;
        if(footer)color=child==play?accent:secondary;
        lv_obj_set_style_text_color(label,lv_color_hex(color),0);
      }
    }
  }
  lv_obj_set_style_bg_color(f->progress,lv_color_hex(secondary),LV_PART_MAIN);
  lv_obj_set_style_bg_opa(f->progress,LV_OPA_20,LV_PART_MAIN);
  lv_obj_set_style_bg_color(f->progress,lv_color_hex(accent),LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(f->progress,lv_color_hex(accent),LV_PART_KNOB);
  lv_obj_set_style_bg_color(f->volume,lv_color_hex(secondary),LV_PART_MAIN);
  lv_obj_set_style_bg_opa(f->volume,LV_OPA_20,LV_PART_MAIN);
  lv_obj_set_style_bg_color(f->volume,lv_color_hex(accent),LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(f->volume,lv_color_hex(accent),LV_PART_KNOB);
  lv_obj_set_style_text_color(f->favorite,lv_color_hex(now->favorite?accent:secondary),0);
  lv_obj_set_style_text_color(f->status,lv_color_hex(secondary),0);
  lv_obj_set_style_text_color(f->elapsed,lv_color_hex(secondary),0);
  lv_obj_set_style_text_color(f->duration,lv_color_hex(secondary),0);
}

static void music_full_fonts_changed(void)
{
  struct music_full_ui *f=g_music_ui.full;
  if(f){
    f->pv.valid=false;
    lv_obj_set_style_text_font(f->pv.translation,music_lyric_font(20),0);
    lv_obj_invalidate(f->root);
  }
  if(g_music_ui.lyric)lv_obj_set_style_text_font(g_music_ui.lyric,music_lyric_font(16),0);
}

static void music_full_fonts_poll(void)
{glass_global_fonts_poll();}

static void music_full_sync(const struct music_now_playing *now,bool force)
{
  struct music_full_ui *f=g_music_ui.full;
  if(!f||f->pv.visible||!glass_ui_can_paint(f->root))return;
  bool playing=now->state==MUSIC_PLAYING&&!now->pause_requested;
  glass_music_lyrics_read(f->window.first,&f->window);
  if(f->window.track_id!=now->track_id)return;
  bool changed=f->track!=now->track_id||f->total!=f->window.total;
  if(!force&&!changed&&f->current==f->window.current&&f->playing==playing)return;
  f->playing=playing;
  if(f->track!=now->track_id){f->follow=true;f->current=-2;}
  f->track=now->track_id;f->total=f->window.total;
  if(changed){
    for(unsigned i=0;i<MUSIC_LYRICS_WINDOW;i++)lv_obj_add_flag(f->rows[i],LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(f->span,f->total*MUSIC_FULL_PITCH+MUSIC_FULL_PAD*2+1);
    lv_obj_update_layout(f->viewport);
  }
  if(!f->total){
    const char *hint=now->lyrics_state==MUSIC_LYRICS_LOADING?glass_tr("正在获取歌词…"):
      now->name[0]?glass_tr("暂无歌词 · 仍可继续听歌"):glass_tr("选择一首喜欢的歌开始播放");
    music_full_text(f->hint,hint);lv_obj_remove_flag(f->hint,LV_OBJ_FLAG_HIDDEN);
  }else lv_obj_add_flag(f->hint,LV_OBJ_FLAG_HIDDEN);
  if(f->follow&&(changed||f->current!=f->window.current)){
    int target=f->window.current<0?0:f->window.current*MUSIC_FULL_PITCH;
    f->move_ms=MUSIC_FULL_MOVE_MS;
    /* Finish by the audio timestamp deadline, including row stagger. Late
     * ticks catch up immediately; rapid lines never queue old transitions. */
    if(f->window.current>=0&&(unsigned)f->window.current>=f->window.first&&
       (unsigned)f->window.current<f->window.first+f->window.count){
      unsigned slot=(unsigned)f->window.current-f->window.first;
      unsigned start=f->window.start_ms[slot],end=f->window.end_ms[slot];
      unsigned age=now->position_ms>start?now->position_ms-start:0;
      f->move_ms=age<MUSIC_FULL_MOVE_MS?MUSIC_FULL_MOVE_MS-age:0;
      unsigned remaining=end>now->position_ms?end-now->position_ms:0;
      unsigned budget=remaining>50?remaining-50:0;
      if(f->move_ms>budget)f->move_ms=budget;
    }
    bool interrupted=f->current>=0&&lv_anim_get(f->rows[f->current%MUSIC_LYRICS_WINDOW],music_full_lyric_offset);
    f->cascade=!changed&&!interrupted&&f->move_ms>=60&&f->playing&&!g_glass.reduced_motion&&f->current>=0&&f->window.current==f->current+1;
    f->cascade_delta=target-lv_obj_get_scroll_y(f->viewport);
    /* Seeks and restoring follow settle immediately; only normal playback
     * advances use the stagger, so old transitions cannot trail a jump. */
    if(!f->cascade){
      f->settle=true;
      for(unsigned i=0;i<MUSIC_LYRICS_WINDOW;i++)music_full_row_reset(f,i);
    }
    lv_obj_scroll_to_y(f->viewport,target,LV_ANIM_OFF);
  }else if(f->follow&&(!f->playing||g_glass.reduced_motion)){
    f->cascade=false;
    lv_obj_scroll_to_y(f->viewport,f->current<0?0:f->current*MUSIC_FULL_PITCH,LV_ANIM_OFF);
  }
  f->current=f->window.current;
  music_full_text(lv_obj_get_child(f->follow_button,0),f->follow?glass_tr("自动跟随"):glass_tr("恢复跟随"));
  music_full_draw();
}

static void music_full_refresh(const struct music_now_playing *now)
{
  struct music_full_ui *f=g_music_ui.full;if(!f||!glass_ui_can_paint(f->root))return;
  f->playing=now->state==MUSIC_PLAYING&&!now->pause_requested;
  if(f->track!=now->track_id)for(unsigned i=0;i<MUSIC_LYRICS_WINDOW;i++){
    music_full_row_reset(f,i);
    lv_obj_add_flag(f->rows[i],LV_OBJ_FLAG_HIDDEN);
  }
  music_cover_tick(&f->cover,now);
  music_full_text(f->title,now->name[0]?now->name:glass_tr("还没有播放音乐"));
  music_full_text(f->artist,now->artist[0]?now->artist:glass_tr("播放歌曲后，在这里跟随歌词"));
  const char *status=now->state==MUSIC_PLAYING?glass_tr("正在播放"):now->state==MUSIC_PAUSED?glass_tr("已暂停"):
    now->state==MUSIC_LOADING?glass_tr("正在加载音乐"):now->state==MUSIC_ERROR?glass_tr("播放失败"):
    now->state==MUSIC_STOPPED?glass_tr("已停止"):now->state==MUSIC_ENDED?glass_tr("播放结束"):glass_tr("音乐 · 全屏歌词");
  music_full_text(f->status,status);
  music_full_text(f->play,(now->state==MUSIC_PLAYING||now->state==MUSIC_LOADING)&&!now->pause_requested?LV_SYMBOL_PAUSE:LV_SYMBOL_PLAY);
  music_ui_seek_refresh(f->progress, &f->seek, now);
  char time[24];
  if(now->duration_ms)snprintf(time,sizeof(time),"%02u:%02u",now->duration_ms/60000,now->duration_ms/1000%60);
  else snprintf(time,sizeof(time),"--:--");
  music_full_text(f->duration,time);
  if(!lv_obj_has_state(f->volume,LV_STATE_PRESSED))lv_slider_set_value(f->volume,now->volume,LV_ANIM_OFF);
  if(f->pv.visible){music_pv_refresh(&f->pv);music_full_palette(f,now);return;}
  music_full_palette(f,now);
  music_full_sync(now,true);
}

static void music_full_mode(lv_event_t *event)
{
  LV_UNUSED(event);struct music_full_ui *f=g_music_ui.full;if(!f)return;
  f->pv.visible=!f->pv.visible;f->pv.valid=false;
  lv_obj_t *ordinary[]={f->cover.host,f->title,f->artist,f->viewport,f->follow_button};
  for(unsigned i=0;i<sizeof(ordinary)/sizeof(ordinary[0]);i++){
    if(f->pv.visible)lv_obj_add_flag(ordinary[i],LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(ordinary[i],LV_OBJ_FLAG_HIDDEN);
  }
  for(unsigned i=0;i<MUSIC_LYRICS_WINDOW;i++){
    music_full_row_reset(f,i);
  }
  f->cascade=false;
  if(f->pv.visible){
    lv_obj_remove_flag(f->pv.style_button,LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(f->pv.root,LV_OBJ_FLAG_HIDDEN);lv_timer_resume(f->pv.timer);
  }else{
    lv_obj_add_flag(f->pv.style_button,LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(f->pv.root,LV_OBJ_FLAG_HIDDEN);lv_timer_pause(f->pv.timer);
    f->follow=true;f->current=-2;
  }
  music_full_text(lv_obj_get_child(f->mode,0),f->pv.visible?glass_tr("普通歌词"):glass_tr("文字 PV"));
  struct music_now_playing now;glass_music_now_playing(&now);music_full_refresh(&now);
}

static void music_full_bilingual(lv_event_t *event)
{
  LV_UNUSED(event);struct music_full_ui *f=g_music_ui.full;if(!f)return;
  g_glass.music_original_only=!g_glass.music_original_only;f->pv.valid=false;f->palette_valid=false;
  music_full_text(lv_obj_get_child(f->bilingual,0),g_glass.music_original_only?glass_tr("原文"):glass_tr("双语"));
  struct music_now_playing now;glass_music_now_playing(&now);music_full_refresh(&now);
}

static void music_full_open(lv_event_t *event)
{
  LV_UNUSED(event);if(!g_music_ui.root||g_music_ui.full)return;
  struct music_full_ui *f=calloc(1,sizeof(*f));if(!f){show_toast(glass_tr("内存不足，稍后再试"));return;}
  g_music_ui.full=f;f->follow=true;f->track=UINT32_MAX;f->current=-2;
  f->status_hidden=g_desktop.statusbar&&lv_obj_has_flag(g_desktop.statusbar,LV_OBJ_FLAG_HIDDEN);
  f->root=glass_box(lv_layer_top(),0,0,1024,600,g_music_ui.background,0);
  lv_obj_remove_flag(f->root,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_flag(f->root,LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(f->root,music_full_deleted,LV_EVENT_DELETE,f);
  glass_status_visibility(true);
  music_ui_button(f->root,glass_tr("返回音乐"),24,16,136,44,g_music_ui.surface,music_full_close,NULL,false);
  f->status=music_ui_text(f->root,glass_tr("音乐 · 全屏歌词"),194,27,274,16,g_music_ui.secondary);
  f->bilingual=music_ui_button(f->root,g_glass.music_original_only?glass_tr("原文"):glass_tr("双语"),494,16,132,44,g_music_ui.surface,music_full_bilingual,NULL,false);
  f->mode=music_ui_button(f->root,glass_tr("文字 PV"),650,16,162,44,g_music_ui.surface,music_full_mode,NULL,false);
  f->follow_button=music_ui_button(f->root,glass_tr("自动跟随"),832,16,168,44,g_music_ui.surface,music_full_follow,NULL,false);
  f->cover.host=music_ui_sleeve(f->root,48,100,288,20);f->cover.owner=GLASS_COVER_LOCK;
  f->title=music_ui_text(f->root,"",48,404,304,28,g_music_ui.primary);
  f->artist=music_ui_text(f->root,"",48,448,304,16,g_music_ui.secondary);
  f->viewport=glass_box(f->root,386,86,590,MUSIC_FULL_HEIGHT,g_music_ui.background,0);
  lv_obj_add_flag(f->viewport,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(f->viewport,LV_DIR_VER);
  lv_obj_set_scrollbar_mode(f->viewport,LV_SCROLLBAR_MODE_AUTO);
  f->span=glass_box(f->viewport,0,0,1,1,g_music_ui.background,0);
  lv_obj_set_style_bg_opa(f->span,LV_OPA_TRANSP,0);
  lv_obj_remove_flag(f->span,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
  for(unsigned i=0;i<MUSIC_LYRICS_WINDOW;i++){
    f->rows[i]=glass_box(f->viewport,22,0,MUSIC_FULL_LINE_WIDTH,MUSIC_FULL_PITCH,g_music_ui.background,0);
    lv_obj_remove_flag(f->rows[i],LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    f->labels[i]=music_ui_text(f->rows[i],"",0,0,MUSIC_FULL_LINE_WIDTH,20,g_music_ui.secondary);
    lv_obj_set_height(f->labels[i],LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(f->labels[i],70,0);
    lv_label_set_long_mode(f->labels[i],LV_LABEL_LONG_WRAP);
    f->translations[i]=music_ui_text(f->rows[i],"",0,47,MUSIC_FULL_LINE_WIDTH,16,g_music_ui.secondary);
    lv_obj_set_height(f->translations[i],24);lv_label_set_long_mode(f->translations[i],LV_LABEL_LONG_DOT);
    lv_obj_add_flag(f->translations[i],LV_OBJ_FLAG_HIDDEN);
  }
  f->hint=music_ui_text(f->viewport,"",22,MUSIC_FULL_PAD,538,20,g_music_ui.secondary);
  lv_obj_add_flag(f->hint,LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_obj_add_event_cb(f->viewport,music_full_scroll,LV_EVENT_SCROLL,NULL);
  lv_obj_add_event_cb(f->viewport,music_full_scroll,LV_EVENT_SCROLL_BEGIN,NULL);
  /* One quiet control island. All transport targets remain at least 44 px;
   * the slender volume track gets its touch area independently of its ink. */
  f->controls=glass_box(f->root,136,488,752,64,g_music_ui.surface,32);
  lv_obj_set_style_bg_opa(f->controls,LV_OPA_70,0);
  lv_obj_set_style_border_width(f->controls,1,0);lv_obj_set_style_border_opa(f->controls,LV_OPA_10,0);
  lv_obj_remove_flag(f->controls,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
  f->elapsed=music_ui_text(f->root,"00:00",212,511,48,12,g_music_ui.secondary);
  f->duration=music_ui_text(f->root,"--:--",380,511,48,12,g_music_ui.secondary);
  f->progress=lv_slider_create(f->root);lv_obj_set_pos(f->progress,264,519);lv_obj_set_size(f->progress,108,3);
  music_ui_seek_setup(f->progress, &f->seek, f->elapsed);
  lv_obj_set_style_bg_color(f->progress,lv_color_hex(g_music_ui.surface),LV_PART_MAIN);
  lv_obj_set_style_bg_color(f->progress,lv_color_hex(g_music_ui.accent),LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(f->progress,LV_OPA_COVER,LV_PART_MAIN);
  lv_obj_set_style_bg_opa(f->progress,LV_OPA_COVER,LV_PART_INDICATOR);
  lv_obj_set_style_radius(f->progress,LV_RADIUS_CIRCLE,LV_PART_MAIN);
  lv_obj_set_style_radius(f->progress,LV_RADIUS_CIRCLE,LV_PART_INDICATOR);
  lv_obj_t *favorite=music_ui_button(f->root,GLASS_ICON_HEART,152,498,44,44,g_music_ui.surface,music_ui_favorite,NULL,true);
  f->favorite=lv_obj_get_child(favorite,0);
  f->previous=music_ui_button(f->root,LV_SYMBOL_PREV,432,498,44,44,g_music_ui.surface,music_ui_skip,(void *)-1,true);
  lv_obj_t *play=music_ui_button(f->root,LV_SYMBOL_PLAY,488,496,48,48,g_music_ui.accent,music_ui_toggle,NULL,true);
  f->play=lv_obj_get_child(play,0);lv_obj_set_style_text_color(f->play,lv_color_white(),0);
  f->next=music_ui_button(f->root,LV_SYMBOL_NEXT,544,498,44,44,g_music_ui.surface,music_ui_skip,(void *)1,true);
  f->volume_icon=music_ui_text(f->root,LV_SYMBOL_VOLUME_MAX,636,510,28,16,g_music_ui.secondary);
  lv_obj_set_style_text_font(f->volume_icon,glass_icon_font(16),0);
  f->volume=lv_slider_create(f->root);lv_obj_set_pos(f->volume,676,519);lv_obj_set_size(f->volume,148,4);
  lv_obj_set_ext_click_area(f->volume,20);
  lv_obj_set_style_pad_all(f->volume,3,LV_PART_KNOB);
  lv_obj_set_style_bg_opa(f->volume,LV_OPA_TRANSP,LV_PART_KNOB);
  lv_obj_set_style_bg_opa(f->volume,LV_OPA_COVER,LV_PART_KNOB|LV_STATE_PRESSED);
  lv_slider_set_range(f->volume,0,100);
  lv_obj_set_style_bg_color(f->volume,lv_color_hex(g_music_ui.surface),LV_PART_MAIN);
  lv_obj_set_style_bg_color(f->volume,lv_color_hex(g_music_ui.accent),LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(f->volume,lv_color_hex(g_music_ui.accent),LV_PART_KNOB);
  lv_obj_add_event_cb(f->volume,music_ui_volume,LV_EVENT_VALUE_CHANGED,NULL);
  music_pv_create(&f->pv,f->root);
  struct music_now_playing now;glass_music_now_playing(&now);music_full_refresh(&now);
}
