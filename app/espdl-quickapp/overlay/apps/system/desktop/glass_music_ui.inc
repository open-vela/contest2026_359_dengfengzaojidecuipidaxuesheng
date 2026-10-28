/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_music.h"
#include "glass_ime.h"
struct music_full_ui;
struct music_seek_ui {
  lv_obj_t *elapsed;
  unsigned track, duration;
  bool dragging;
};

static struct {
  lv_obj_t *root, *list, *heading, *subtitle, *search, *status, *title, *artist;
  lv_obj_t *album, *bottom_title, *bottom_artist, *play_label, *favorite_label, *elapsed;
  lv_obj_t *volume, *progress, *duration, *count, *nav[2], *disc, *input_shade, *input, *keyboard;
  lv_obj_t *lyric;
  struct music_full_ui *full;
  lv_timer_t *timer, *sync_timer;
  struct music_snapshot *snapshot;
  struct music_cover_view cover;
  struct music_seek_ui seek;
  unsigned search_revision, favorite_count;
  bool favorites, valid, return_apps;
  uint32_t primary, secondary, surface, accent, background;
} g_music_ui;

static void music_ui_refresh(lv_timer_t *timer);
static void music_ui_list(void);
static bool music_config_return_apps;
static void music_ui_configure(lv_event_t *e)
{LV_UNUSED(e);music_config_return_apps=g_music_ui.return_apps;glass_portal_show(glass_music);}
static void music_full_close(lv_event_t *e);
static void music_full_refresh(const struct music_now_playing *now);

static lv_obj_t *music_ui_text(lv_obj_t *parent, const char *text, int x, int y, int width, int size, uint32_t color)
{
  lv_obj_t *label=glass_text(parent,text,x,y,width,size,color);
  if(size==48&&g_glass.font48)lv_obj_set_style_text_font(label,g_glass.font48,0);
  return label;
}

/* Quiet, static surface details: no off-screen buffers or animation timers. */
static void music_ui_edge(lv_obj_t *obj, lv_opa_t opacity)
{
  lv_obj_set_style_border_width(obj,1,0);
  lv_obj_set_style_border_color(obj,lv_color_hex(g_desktop.light_theme?0x867c80:0xffffff),0);
  lv_obj_set_style_border_opa(obj,opacity,0);
}

static void music_ui_nav_style(lv_obj_t *button, bool selected)
{
  glass_sidebar_item(button,selected);
  lv_obj_set_style_radius(button,12,0);
  lv_obj_set_style_bg_color(button,lv_color_hex(selected?detail_soft():detail_paper()),0);
  lv_obj_set_style_bg_color(button,lv_color_hex(detail_soft()),LV_STATE_PRESSED);
  lv_obj_set_style_border_width(button,0,0);
  lv_obj_t *label=lv_obj_get_child(button,0);
  lv_obj_set_style_text_color(label,lv_color_hex(selected?g_music_ui.primary:g_music_ui.secondary),0);
  bool icon=lv_obj_get_child_count(button)>1;
  lv_obj_set_width(label,icon?88:112);
  lv_obj_set_style_text_font(label,zh_font(16),0);
  lv_obj_align(label,LV_ALIGN_LEFT_MID,icon?44:16,0);
  if(icon)lv_obj_set_style_text_color(lv_obj_get_child(button,1),lv_color_hex(selected?g_music_ui.accent:g_music_ui.secondary),0);
}

/* The geometric sleeve is also the offline/loading cover. Real artwork is placed
 * over it by music_cover_tick; decoration must never intercept cover taps. */
static lv_obj_t *music_ui_sleeve(lv_obj_t *parent,int x,int y,int size,int radius)
{
  lv_obj_t *sleeve=glass_box(parent,x,y,size,size,0xdcc8cf,radius);
  lv_obj_set_style_clip_corner(sleeve,true,0);
  lv_obj_remove_flag(sleeve,LV_OBJ_FLAG_CLICKABLE);
  int diameter=size*94/100;
  lv_obj_t *disc=glass_box(sleeve,size*30/100,-size*13/100,diameter,diameter,0x443345,LV_RADIUS_CIRCLE);
  lv_obj_remove_flag(disc,LV_OBJ_FLAG_CLICKABLE);
  int inset=diameter*15/100;
  lv_obj_t *ring=glass_box(disc,inset,inset,diameter-inset*2,diameter-inset*2,0x8d7188,LV_RADIUS_CIRCLE);
  lv_obj_remove_flag(ring,LV_OBJ_FLAG_CLICKABLE);
  inset=diameter*30/100;
  lv_obj_t *label=glass_box(disc,inset,inset,diameter-inset*2,diameter-inset*2,0xc7a5b1,LV_RADIUS_CIRCLE);
  lv_obj_remove_flag(label,LV_OBJ_FLAG_CLICKABLE);
  if(size>100){
    music_ui_text(sleeve,glass_tr("声 · 色"),16,size-76,size-32,28,0x443345);
    lv_obj_t *caption=music_ui_text(sleeve,"VELA / MUSIC",16,size-32,size-32,14,0x685169);
    lv_obj_set_style_text_letter_space(caption,1,0);
  }
  return sleeve;
}

static lv_obj_t *music_ui_button(lv_obj_t *parent, const char *text, int x, int y, int width, int height,
                                uint32_t color, lv_event_cb_t cb, void *data, bool symbol)
{
  lv_obj_t *button=lv_button_create(parent);
  lv_obj_remove_style_all(button);
  lv_obj_set_pos(button,x,y); lv_obj_set_size(button,width,height);
  if(height<44)lv_obj_set_ext_click_area(button,(44-height+1)/2);
  lv_obj_set_style_bg_color(button,lv_color_hex(color),0);
  lv_obj_set_style_bg_opa(button,LV_OPA_COVER,0);
  lv_obj_set_style_radius(button,symbol?height/2:12,0);
  glass_clickable(button,cb,data);
  lv_obj_set_style_bg_opa(button,LV_OPA_80,LV_STATE_PRESSED);
  if(symbol&&color!=g_music_ui.accent){
    lv_obj_set_style_bg_opa(button,LV_OPA_TRANSP,0);
    lv_obj_set_style_bg_opa(button,LV_OPA_10,LV_STATE_PRESSED);
  }
  if(!symbol)music_ui_edge(button,g_desktop.light_theme?LV_OPA_20:LV_OPA_10);
  if(color==g_music_ui.accent){
    music_ui_edge(button,LV_OPA_30);
  }
  lv_obj_t *label=music_ui_text(button,text,0,0,width-12,16,g_music_ui.primary);
  if(color==g_music_ui.accent)
    lv_obj_set_style_text_color(label,lv_color_hex(g_desktop.light_theme?0xffffff:0x251922),0);
  if (symbol) lv_obj_set_style_text_font(label,glass_icon_font(24),0);
  lv_label_set_long_mode(label,LV_LABEL_LONG_DOT);
  lv_obj_set_height(label,lv_obj_get_style_text_font(label,0)->line_height);
  lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_CENTER,0);
  lv_obj_center(label);
  return button;
}

static lv_obj_t *music_ui_sidebar_button(lv_obj_t *parent,const char *text,const char *symbol,
                                        int y,lv_event_cb_t cb,void *data)
{
  lv_obj_t *button=music_ui_button(parent,text,12,y,140,44,g_music_ui.surface,cb,data,false);
  lv_obj_t *icon=glass_symbol(button,symbol,16,12,g_music_ui.secondary);
  lv_obj_set_style_text_font(icon,glass_icon_font(20),0);
  lv_obj_align(icon,LV_ALIGN_LEFT_MID,16,0);
  music_ui_nav_style(button,false);
  return button;
}

static void music_ui_play(lv_event_t *e)
{
  uintptr_t index=(uintptr_t)lv_event_get_user_data(e);
  const struct music_song *song=index ? &g_music_ui.snapshot->favorites[index-1] : &g_music_ui.snapshot->result;
  if (song->name[0]) glass_music_play(song);
  music_ui_refresh(NULL);
}

static void music_ui_toggle(lv_event_t *e)
{ LV_UNUSED(e); glass_music_toggle(); music_ui_refresh(NULL); }

static void music_ui_favorite(lv_event_t *e)
{
  const struct music_song *song=lv_event_get_user_data(e) ? &g_music_ui.snapshot->result : &g_music_ui.snapshot->current;
  if (!song->name[0]) { show_toast(glass_tr("先选择一首歌曲")); return; }
  int ret=glass_music_favorite(song);
  if (ret) show_toast(glass_tr("正在保存，请稍后再试"));
}

static void music_ui_skip(lv_event_t *e)
{
  int direction=(intptr_t)lv_event_get_user_data(e);
  if(glass_music_skip(direction)) show_toast(glass_tr("收藏歌曲后可切换播放"));
  music_ui_refresh(NULL);
}

static void music_ui_volume(lv_event_t *e)
{ glass_music_volume(lv_slider_get_value(lv_event_get_target(e))); }

static void music_ui_seek_event(lv_event_t *e)
{
  struct music_seek_ui *seek = lv_event_get_user_data(e);
  lv_obj_t *slider = lv_event_get_target(e);
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) {
    struct music_now_playing now; glass_music_now_playing(&now);
    seek->track = now.track_id; seek->duration = now.duration_ms;
    seek->dragging = seek->duration != 0;
  }
  if (code == LV_EVENT_VALUE_CHANGED && seek->dragging) {
    unsigned ms = (uint64_t)lv_slider_get_value(slider) * seek->duration / 1000;
    lv_label_set_text_fmt(seek->elapsed, "%02u:%02u", ms/60000, ms/1000%60);
  }
  if (code == LV_EVENT_RELEASED && seek->dragging) {
    unsigned ms = (uint64_t)lv_slider_get_value(slider) * seek->duration / 1000;
    seek->dragging = false;
    if (glass_music_seek(seek->track, ms)) show_toast(glass_tr("当前歌曲暂时无法跳转"));
    music_ui_refresh(NULL);
  }
  if (code == LV_EVENT_PRESS_LOST) seek->dragging = false;
}

static void music_ui_seek_setup(lv_obj_t *slider, struct music_seek_ui *seek, lv_obj_t *elapsed)
{
  seek->elapsed = elapsed;
  lv_slider_set_range(slider, 0, 1000);
  lv_obj_set_ext_click_area(slider, 18);
  lv_obj_remove_flag(slider, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN_HOR |
                             LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_set_style_bg_color(slider, lv_color_hex(g_music_ui.accent), LV_PART_KNOB);
  lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
  lv_obj_set_style_pad_all(slider, 4, LV_PART_KNOB);
  lv_obj_add_event_cb(slider, music_ui_seek_event, LV_EVENT_ALL, seek);
}

static void music_ui_seek_refresh(lv_obj_t *slider, struct music_seek_ui *seek,
                                  const struct music_now_playing *now)
{
  if (seek->dragging) return;
  bool enabled = now->duration_ms && (now->state == MUSIC_PLAYING ||
    now->state == MUSIC_PAUSED || now->state == MUSIC_LOADING);
  if (enabled) lv_obj_remove_state(slider, LV_STATE_DISABLED);
  else lv_obj_add_state(slider, LV_STATE_DISABLED);
  unsigned progress = now->duration_ms ? (uint64_t)now->position_ms*1000/now->duration_ms : 0;
  lv_slider_set_value(slider, progress > 1000 ? 1000 : progress, LV_ANIM_OFF);
  lv_label_set_text_fmt(seek->elapsed, "%02u:%02u", now->position_ms/60000, now->position_ms/1000%60);
}

#include "glass_music_full.inc"

static void music_ui_lyric_refresh(const struct music_now_playing *now)
{
  const char *line=now->lyrics[1][0]?now->lyrics[1]:
    now->lyrics_state==MUSIC_LYRICS_LOADING?"正在获取歌词…":
    now->lyrics_state==MUSIC_LYRICS_READY?(now->lyrics_started?"间奏":"前奏 · 等待歌词"):now->name[0]?"暂无歌词":"";
  char bilingual[MUSIC_LYRIC_TEXT_MAX*2+2];
  if(!g_glass.music_original_only&&now->translations[1][0]){
    snprintf(bilingual,sizeof(bilingual),"%s\n%s",line,now->translations[1]);line=bilingual;
  }
  if(strcmp(lv_label_get_text(g_music_ui.lyric),line))lv_label_set_text(g_music_ui.lyric,line);
}

static void music_ui_sync_tick(lv_timer_t *timer)
{
  LV_UNUSED(timer);
  if(!g_music_ui.root)return;
  if(g_music_ui.full&&g_music_ui.full->pv.visible)return; /* PV owns its 50 ms tick. */
  if(!g_music_ui.full&&!glass_ui_can_paint(g_music_ui.root))return;
  struct music_now_playing now;glass_music_now_playing(&now);
  if(g_music_ui.full)music_full_sync(&now,false);
  else music_ui_lyric_refresh(&now);
}

static void music_ui_nav(lv_event_t *e)
{
  g_music_ui.favorites=lv_event_get_user_data(e)!=NULL;
  music_ui_list();
}

static void music_ui_suggestion(lv_event_t *e)
{
  const char *query=lv_event_get_user_data(e);
  lv_label_set_text(lv_obj_get_child(g_music_ui.search,0),query);
  g_music_ui.favorites=false;
  glass_music_search(query);
  music_ui_list(); music_ui_refresh(NULL);
}

static void music_ui_input_close(lv_event_t *e)
{
  LV_UNUSED(e);
  if (g_music_ui.input_shade) lv_obj_delete(g_music_ui.input_shade);
  g_music_ui.input_shade=g_music_ui.input=g_music_ui.keyboard=NULL;
}

static void music_ui_input_submit(lv_event_t *e)
{
  LV_UNUSED(e);
  glass_ime_commit(g_music_ui.keyboard);
  char query[128];
  const char *text=lv_textarea_get_text(g_music_ui.input);
  if (!text[0]) { show_toast(glass_tr("请输入歌名")); return; }
  if (strlen(text)>=sizeof(query)) { show_toast(glass_tr("搜索词太长，请缩短一些")); return; }
  strcpy(query,text);
  lv_label_set_text(lv_obj_get_child(g_music_ui.search,0),query);
  music_ui_input_close(NULL);
  g_music_ui.favorites=false;
  glass_music_search(query);
  music_ui_list(); music_ui_refresh(NULL);
}

static void music_ui_input_open(lv_event_t *e)
{
  LV_UNUSED(e);
  if (g_music_ui.input_shade) return;
  lv_obj_t *shade=glass_box(g_music_ui.root,188,8,818,482,g_music_ui.background,20);
  g_music_ui.input_shade=shade;
  music_ui_text(shade,glass_tr("搜索音乐"),24,20,420,28,g_music_ui.primary);
  music_ui_button(shade,glass_tr("取消"),590,18,84,44,g_music_ui.surface,music_ui_input_close,NULL,false);
  lv_obj_t *submit=music_ui_button(shade,glass_tr("搜索"),688,18,102,44,g_music_ui.accent,music_ui_input_submit,NULL,false);
  lv_obj_set_style_text_color(lv_obj_get_child(submit,0),lv_color_white(),0);
  music_ui_text(shade,glass_tr("连续输入拼音，按词频选词 · 空格确认 · 中 / EN 切换"),24,72,756,16,g_music_ui.secondary);
  g_music_ui.input=lv_textarea_create(shade);
  glass_input_style(g_music_ui.input);
  lv_obj_set_pos(g_music_ui.input,24,108); lv_obj_set_size(g_music_ui.input,766,54);
  lv_textarea_set_one_line(g_music_ui.input,true); lv_textarea_set_max_length(g_music_ui.input,40);
  lv_textarea_set_placeholder_text(g_music_ui.input,glass_tr("输入歌名，如：晴天；不要追加歌手"));
  g_music_ui.keyboard=lv_keyboard_create(shade);
  glass_keyboard_style(g_music_ui.keyboard);
  lv_obj_set_size(g_music_ui.keyboard,782,240);
  lv_obj_align(g_music_ui.keyboard,LV_ALIGN_TOP_LEFT,16,226);
  lv_keyboard_set_mode(g_music_ui.keyboard,LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_keyboard_set_textarea(g_music_ui.keyboard,g_music_ui.input);
  glass_ime_attach(g_music_ui.keyboard,zh_font(20),true,g_music_ui.primary,g_music_ui.surface,g_music_ui.accent);
  lv_obj_add_event_cb(g_music_ui.keyboard,music_ui_input_submit,LV_EVENT_READY,NULL);
  lv_obj_add_event_cb(g_music_ui.keyboard,music_ui_input_close,LV_EVENT_CANCEL,NULL);
}

static void music_ui_row(lv_obj_t *list, const struct music_song *song, unsigned index, int y)
{
  lv_obj_t *row=glass_box(list,0,y,494,92,g_music_ui.surface,18);
  music_ui_edge(row,LV_OPA_10);
  glass_clickable(row,music_ui_play,(void *)(uintptr_t)index);
  music_ui_sleeve(row,14,14,64,12);
  music_ui_text(row,song->name,94,18,320,20,g_music_ui.primary);
  char meta[sizeof(song->artist)+sizeof(song->album)+8];
  snprintf(meta,sizeof(meta),"%s%s%s",song->artist[0]?song->artist:glass_tr("未知歌手"),song->album[0]?" · ":"",song->album);
  music_ui_text(row,meta,94,51,320,16,g_music_ui.secondary);
  lv_obj_t *play=glass_box(row,432,24,44,44,g_music_ui.accent,LV_RADIUS_CIRCLE);
  lv_obj_remove_flag(play,LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *icon=glass_symbol(play,LV_SYMBOL_PLAY,0,0,g_desktop.light_theme?0xffffff:0x251922);
  lv_obj_set_style_text_font(icon,glass_icon_font(20),0);lv_obj_center(icon);
}

static void music_ui_list(void)
{
  if (!g_music_ui.list) return;
  lv_obj_clean(g_music_ui.list);
  lv_obj_scroll_to_y(g_music_ui.list,0,LV_ANIM_OFF);
  for (unsigned i=0;i<2;i++) {
    bool selected=(i==1)==g_music_ui.favorites;
    music_ui_nav_style(g_music_ui.nav[i],selected);
  }
  struct music_snapshot *s=g_music_ui.snapshot;
  lv_label_set_text(g_music_ui.heading,g_music_ui.favorites?glass_tr("收藏的声音"):glass_tr("听见好时光"));
  lv_label_set_text(g_music_ui.subtitle,g_music_ui.favorites?glass_tr("每一首喜欢，都值得再听一遍。"):glass_tr("一首好歌，一段只属于你的时间。"));
  if (g_music_ui.favorites) {
    if (!s->favorite_count) {
      music_ui_text(g_music_ui.list,glass_tr("让喜欢的歌，留在这里"),22,52,418,28,g_music_ui.primary);
      music_ui_text(g_music_ui.list,glass_tr("播放时点一下收藏，下次打开还能听见。"),22,102,418,16,g_music_ui.secondary);
    }
    for (unsigned i=0;i<s->favorite_count;i++) music_ui_row(g_music_ui.list,&s->favorites[i],i+1,i*104);
  } else if (s->searching) {
    music_ui_text(g_music_ui.list,glass_tr("正在寻找好音乐…"),22,52,418,28,g_music_ui.primary);
    music_ui_text(g_music_ui.list,glass_tr("搜索完成后，点选歌曲开始播放"),22,102,418,16,g_music_ui.secondary);
  } else if (s->has_result) {
    music_ui_text(g_music_ui.list,glass_tr("搜索结果"),0,0,420,16,g_music_ui.secondary);
    music_ui_row(g_music_ui.list,&s->result,0,34);
    bool favorite=glass_music_is_favorite(s,&s->result);
    lv_obj_t *save=music_ui_button(g_music_ui.list,favorite?glass_tr("已收藏"):glass_tr("收藏这首歌"),0,146,140,44,g_music_ui.surface,music_ui_favorite,(void *)1,false);
    lv_obj_set_style_text_color(lv_obj_get_child(save,0),lv_color_hex(g_music_ui.accent),0);
    lv_obj_set_style_border_width(save,0,0);
    music_ui_text(g_music_ui.list,glass_tr("点选歌曲，开始聆听"),164,160,310,16,g_music_ui.secondary);
  } else if (s->search_error==MUSIC_ERROR_KEY_REQUIRED) {
    music_ui_text(g_music_ui.list,glass_tr("先配置音乐搜索服务"),0,14,452,28,g_music_ui.primary);
    music_ui_text(g_music_ui.list,glass_tr("扫码配置 → 音乐服务\n保存 JKAPI Key 后，返回这里重新搜索。"),0,64,452,16,g_music_ui.secondary);
    music_ui_button(g_music_ui.list,glass_tr("扫码配置"),0,144,160,44,g_music_ui.accent,music_ui_configure,NULL,false);
  } else {
    music_ui_text(g_music_ui.list,s->search_error?glass_tr("暂时没有找到"):glass_tr("从一首喜欢的歌开始"),0,0,490,28,g_music_ui.primary);
    music_ui_text(g_music_ui.list,s->search_error?glass_tr("试试只输入歌名，或检查网络后重试。"):glass_tr("试着搜索"),0,42,490,16,g_music_ui.secondary);
    const char *queries[]={"晴天","稻香","夜曲","青花瓷","海阔天空","红豆"};
    for (unsigned i=0;i<6;i++) music_ui_button(g_music_ui.list,queries[i],(i%2)*254,78+(i/2)*50,240,44,g_music_ui.surface,music_ui_suggestion,(void *)queries[i],false);
  }
}

static void music_ui_refresh(lv_timer_t *timer)
{
  LV_UNUSED(timer);
  if (!g_music_ui.root) return;
  if(!g_music_ui.full&&!glass_ui_can_paint(g_music_ui.root)){g_music_ui.valid=false;return;}
  music_full_fonts_poll();
  unsigned previous_revision=g_music_ui.snapshot->revision;
  unsigned previous_search=g_music_ui.snapshot->search_revision;
  unsigned previous_count=g_music_ui.snapshot->favorite_count;
  bool searching=g_music_ui.snapshot->searching;
  int storage_error=g_music_ui.snapshot->storage_error;
  glass_music_get(g_music_ui.snapshot);
  struct music_snapshot *s=g_music_ui.snapshot;
  struct music_now_playing now;glass_music_now_playing(&now);
  music_full_refresh(&now);
  /* Keep the song snapshot current for full-screen favorite actions, but
   * defer covered-page widgets and artwork until the full-screen view closes. */
  if(g_music_ui.full){g_music_ui.valid=false;return;}
  music_cover_tick(&g_music_ui.cover,&now);
  /* Settings can change the master while the track is paused, without a
   * music revision. Keep the control current before the song-data shortcut. */
  if (!lv_obj_has_state(g_music_ui.volume,LV_STATE_PRESSED)&&lv_slider_get_value(g_music_ui.volume)!=(int)s->volume)
    lv_slider_set_value(g_music_ui.volume,s->volume,LV_ANIM_OFF);
  if (g_music_ui.valid && s->revision==previous_revision) return;
  if (!g_music_ui.valid || s->search_revision!=previous_search || s->favorite_count!=previous_count || s->searching!=searching) music_ui_list();
  if (s->storage_error && s->storage_error!=storage_error) show_toast(s->storage_error==-ENOSPC?glass_tr("收藏空间已满"):glass_tr("收藏未保存，请检查存储"));
  g_music_ui.valid=true;
  const char *title=s->current.name[0]?s->current.name:glass_tr("还没有播放音乐");
  const char *artist=s->current.artist[0]?s->current.artist:glass_tr("让好音乐，陪伴此刻");
  lv_label_set_text(g_music_ui.title,title); lv_label_set_text(g_music_ui.bottom_title,title);
  lv_label_set_text(g_music_ui.artist,artist); lv_label_set_text(g_music_ui.bottom_artist,artist);
  lv_label_set_text(g_music_ui.album,s->current.album);
  music_ui_lyric_refresh(&now);
  const char *status=s->state==MUSIC_LOADING?glass_tr("正在加载音乐"):s->state==MUSIC_PLAYING?glass_tr("正在播放"):s->state==MUSIC_PAUSED?glass_tr("已暂停"):
    s->state==MUSIC_ERROR?glass_tr("播放失败 · 点击播放重试"):s->state==MUSIC_ENDED?glass_tr("播放结束"):s->state==MUSIC_STOPPED?glass_tr("已停止"):glass_tr("现在播放");
  lv_label_set_text(g_music_ui.status,status);
  lv_label_set_text(g_music_ui.play_label,(s->state==MUSIC_PLAYING || s->state==MUSIC_LOADING) && !now.pause_requested?LV_SYMBOL_PAUSE:LV_SYMBOL_PLAY);
  lv_obj_set_style_text_color(g_music_ui.favorite_label,lv_color_hex(
    glass_music_is_favorite(s,&s->current)?g_music_ui.accent:g_music_ui.secondary),0);
  lv_label_set_text_fmt(g_music_ui.count,glass_tr("音乐库 · %u 首"),s->favorite_count);
  music_ui_seek_refresh(g_music_ui.progress, &g_music_ui.seek, &now);
  if (s->duration_ms)
    lv_label_set_text_fmt(g_music_ui.duration,"%02u:%02u",s->duration_ms/60000,(s->duration_ms/1000)%60);
  else lv_label_set_text(g_music_ui.duration,s->current.name[0]?"--:--":"00:00");
}

static void music_ui_deleted(lv_event_t *e)
{
  LV_UNUSED(e);
  music_full_close(NULL);
  music_cover_reset(&g_music_ui.cover);
  if (g_music_ui.timer) lv_timer_delete(g_music_ui.timer);
  if (g_music_ui.sync_timer) lv_timer_delete(g_music_ui.sync_timer);
  free(g_music_ui.snapshot);
  memset(&g_music_ui,0,sizeof(g_music_ui));
}

static void music_ui_back(lv_event_t *event)
{
  LV_UNUSED(event);
  if(g_music_ui.return_apps)glass_apps(NULL);
  else panel_hide(NULL);
}

static void glass_music_app(lv_obj_t *card,bool return_apps)
{
  /* This CPU-only library copy belongs in the initialized user heap, not
   * internal BSS, where it would displace the kernel/DMA heap by 55 KiB.
   * Allocate before changing the page so failure preserves its old state.
   */
  struct music_snapshot *snapshot=calloc(1,sizeof(*snapshot));
  if(!snapshot){show_toast(glass_tr("音乐页面内存不足"));return;}
  lv_obj_clean(card);
  free(g_music_ui.snapshot);
  memset(&g_music_ui,0,sizeof(g_music_ui));
  g_music_ui.snapshot=snapshot;
  g_music_ui.root=card;
  g_music_ui.return_apps=return_apps;
  bool light=g_desktop.light_theme;
  g_music_ui.primary=detail_ink();
  g_music_ui.secondary=detail_muted();
  g_music_ui.background=detail_canvas();
  g_music_ui.surface=detail_soft();
  g_music_ui.accent=detail_accent();
  lv_obj_set_size(card,1024,GLASS_CONTENT_HEIGHT); lv_obj_set_pos(card,0,0);
  lv_obj_set_style_radius(card,0,0); lv_obj_set_style_border_width(card,0,0);
  lv_obj_set_style_bg_color(card,lv_color_hex(g_music_ui.background),0);
  lv_obj_add_event_cb(card,music_ui_deleted,LV_EVENT_DELETE,NULL);
  lv_obj_t *sidebar=glass_box(card,0,0,GLASS_SIDEBAR_WIDTH,GLASS_CONTENT_HEIGHT,detail_paper(),0);
  glass_sidebar_surface(sidebar);
  lv_obj_set_style_bg_color(sidebar,lv_color_hex(detail_paper()),0);
  music_ui_edge(sidebar,LV_OPA_10);
  lv_obj_set_style_border_side(sidebar,LV_BORDER_SIDE_RIGHT,0);
  lv_obj_t *brand=glass_box(sidebar,24,28,30,30,g_music_ui.accent,10);
  lv_obj_remove_flag(brand,LV_OBJ_FLAG_CLICKABLE);
  glass_symbol(brand,GLASS_ICON_HEADPHONES,3,3,light?0xffffff:0x251922);
  music_ui_text(sidebar,glass_tr("音乐"),64,29,90,20,g_music_ui.primary);
  g_music_ui.count=music_ui_text(sidebar,glass_tr("音乐库 · 0 首"),24,88,128,14,g_music_ui.secondary);
  g_music_ui.nav[0]=music_ui_sidebar_button(sidebar,glass_tr("搜索发现"),GLASS_ICON_GRID,116,music_ui_nav,NULL);
  g_music_ui.nav[1]=music_ui_sidebar_button(sidebar,glass_tr("我的收藏"),GLASS_ICON_HEART,168,music_ui_nav,(void *)1);
  music_ui_nav_style(g_music_ui.nav[0],true);music_ui_nav_style(g_music_ui.nav[1],false);
  lv_obj_t *divider=glass_box(sidebar,24,232,116,1,g_music_ui.surface,0);
  lv_obj_remove_flag(divider,LV_OBJ_FLAG_CLICKABLE);
  music_ui_text(sidebar,glass_tr("播放"),24,256,116,14,g_music_ui.secondary);
  music_ui_sidebar_button(sidebar,glass_tr("全屏歌词"),GLASS_ICON_HEADPHONES,284,music_full_open,NULL);
  music_ui_sidebar_button(sidebar,glass_tr("锁屏播放"),GLASS_ICON_RADIO,336,glass_lock_open,NULL);
  lv_obj_t *return_divider=glass_box(sidebar,24,GLASS_CONTENT_HEIGHT-72,116,1,g_music_ui.surface,0);
  lv_obj_remove_flag(return_divider,LV_OBJ_FLAG_CLICKABLE);
  detail_back_button(sidebar,return_apps?glass_tr("返回应用"):glass_tr("返回桌面"),music_ui_back);
  lv_obj_t *kicker=music_ui_text(card,"MUSIC / FOR YOU",196,24,490,16,g_music_ui.accent);
  lv_obj_set_style_text_letter_space(kicker,2,0);
  g_music_ui.heading=music_ui_text(card,glass_tr("听见好时光"),196,52,494,48,g_music_ui.primary);
  g_music_ui.subtitle=music_ui_text(card,glass_tr("一首好歌，一段只属于你的时间。"),196,116,494,16,g_music_ui.secondary);
  g_music_ui.search=music_ui_button(card,glass_tr("搜索歌名，发现喜欢的声音"),196,154,494,52,g_music_ui.surface,music_ui_input_open,NULL,false);
  lv_obj_set_width(lv_obj_get_child(g_music_ui.search,0),430);
  lv_obj_t *search_text=lv_obj_get_child(g_music_ui.search,0);
  lv_obj_set_style_text_align(search_text,LV_TEXT_ALIGN_LEFT,0);
  lv_obj_align(search_text,LV_ALIGN_LEFT_MID,52,0);
  lv_obj_t *search_ring=glass_box(g_music_ui.search,18,14,15,15,g_music_ui.surface,8);
  lv_obj_set_style_bg_opa(search_ring,LV_OPA_TRANSP,0);
  lv_obj_set_style_border_width(search_ring,2,0);
  lv_obj_set_style_border_color(search_ring,lv_color_hex(g_music_ui.secondary),0);
  lv_obj_remove_flag(search_ring,LV_OBJ_FLAG_CLICKABLE);
  static const lv_point_precise_t search_handle[]={{0,0},{6,6}};
  lv_obj_t *handle=lv_line_create(g_music_ui.search);lv_line_set_points(handle,search_handle,2);
  lv_obj_set_pos(handle,30,26);lv_obj_set_style_line_width(handle,2,0);
  lv_obj_set_style_line_color(handle,lv_color_hex(g_music_ui.secondary),0);
  lv_obj_set_style_line_rounded(handle,true,0);lv_obj_remove_flag(handle,LV_OBJ_FLAG_CLICKABLE);
  g_music_ui.list=glass_box(card,196,230,494,224,g_music_ui.background,0);
  lv_obj_add_flag(g_music_ui.list,LV_OBJ_FLAG_SCROLLABLE); lv_obj_set_scroll_dir(g_music_ui.list,LV_DIR_VER);
  lv_obj_t *now=glass_box(card,714,24,286,434,light?0xfffbf8:0x252029,24);
  music_ui_edge(now,light?LV_OPA_20:LV_OPA_10);
  lv_obj_t *status_dot=glass_box(now,22,25,5,5,g_music_ui.accent,LV_RADIUS_CIRCLE);
  lv_obj_remove_flag(status_dot,LV_OBJ_FLAG_CLICKABLE);
  g_music_ui.status=music_ui_text(now,glass_tr("现在播放"),36,18,226,16,g_music_ui.secondary);
  g_music_ui.disc=music_ui_sleeve(now,33,52,220,16);
  g_music_ui.cover.host=g_music_ui.disc;g_music_ui.cover.owner=GLASS_COVER_MUSIC;
  lv_obj_set_style_shadow_width(g_music_ui.disc,14,0);
  lv_obj_set_style_shadow_ofs_y(g_music_ui.disc,5,0);
  lv_obj_set_style_shadow_color(g_music_ui.disc,lv_color_hex(0x0b0810),0);
  lv_obj_set_style_shadow_opa(g_music_ui.disc,LV_OPA_20,0);
  g_music_ui.title=music_ui_text(now,glass_tr("还没有播放音乐"),24,288,238,28,g_music_ui.primary);
  g_music_ui.artist=music_ui_text(now,glass_tr("让好音乐，陪伴此刻"),24,328,238,16,g_music_ui.secondary);
  g_music_ui.album=music_ui_text(now,"",24,352,238,16,g_music_ui.secondary);
  lv_obj_t *lyric_rule=glass_box(now,24,375,238,1,light?0xd9c7c9:0x493741,0);
  lv_obj_remove_flag(lyric_rule,LV_OBJ_FLAG_CLICKABLE);
  g_music_ui.lyric=music_ui_text(now,"",24,382,238,16,g_music_ui.accent);
  lv_obj_set_height(g_music_ui.lyric,48);lv_label_set_long_mode(g_music_ui.lyric,LV_LABEL_LONG_DOT);
  glass_clickable(g_music_ui.disc,music_full_open,NULL);
  glass_clickable(g_music_ui.lyric,music_full_open,NULL);
  lv_obj_t *bar=glass_box(card,180,GLASS_CONTENT_HEIGHT-80,832,68,light?0xfffbf8:0x201c24,20);
  music_ui_edge(bar,LV_OPA_20);
  g_music_ui.cover.thumbnail_host=music_ui_sleeve(bar,12,12,44,10);
  g_music_ui.bottom_title=music_ui_text(bar,glass_tr("还没有播放音乐"),70,10,112,20,g_music_ui.primary);
  g_music_ui.bottom_artist=music_ui_text(bar,glass_tr("让好音乐，陪伴此刻"),70,39,112,14,g_music_ui.secondary);
  lv_label_set_long_mode(g_music_ui.bottom_title,LV_LABEL_LONG_DOT);
  lv_label_set_long_mode(g_music_ui.bottom_artist,LV_LABEL_LONG_DOT);
  music_ui_button(bar,LV_SYMBOL_PREV,234,16,36,36,g_music_ui.surface,music_ui_skip,(void *)-1,true);
  lv_obj_t *play=music_ui_button(bar,LV_SYMBOL_PLAY,276,12,44,44,g_music_ui.accent,music_ui_toggle,NULL,true);
  g_music_ui.play_label=lv_obj_get_child(play,0);
  music_ui_button(bar,LV_SYMBOL_NEXT,326,16,36,36,g_music_ui.surface,music_ui_skip,(void *)1,true);
  g_music_ui.elapsed=music_ui_text(bar,"00:00",376,25,48,14,g_music_ui.secondary);
  g_music_ui.progress=lv_slider_create(bar);
  lv_obj_remove_style_all(g_music_ui.progress);
  lv_obj_set_pos(g_music_ui.progress,432,32); lv_obj_set_size(g_music_ui.progress,180,3);
  music_ui_seek_setup(g_music_ui.progress, &g_music_ui.seek, g_music_ui.elapsed);
  lv_obj_set_style_radius(g_music_ui.progress,LV_RADIUS_CIRCLE,LV_PART_MAIN);
  lv_obj_set_style_radius(g_music_ui.progress,LV_RADIUS_CIRCLE,LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(g_music_ui.progress,LV_OPA_COVER,LV_PART_MAIN);
  lv_obj_set_style_bg_opa(g_music_ui.progress,LV_OPA_COVER,LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(g_music_ui.progress,lv_color_hex(g_music_ui.surface),LV_PART_MAIN);
  lv_obj_set_style_bg_color(g_music_ui.progress,lv_color_hex(g_music_ui.accent),LV_PART_INDICATOR);
  g_music_ui.duration=music_ui_text(bar,"00:00",624,25,48,14,g_music_ui.secondary);
  lv_obj_t *favorite=music_ui_button(bar,GLASS_ICON_HEART,190,16,36,36,g_music_ui.surface,music_ui_favorite,NULL,true);
  g_music_ui.favorite_label=lv_obj_get_child(favorite,0);
  glass_symbol(bar,LV_SYMBOL_VOLUME_MAX,688,22,g_music_ui.secondary);
  g_music_ui.volume=lv_slider_create(bar); lv_obj_set_pos(g_music_ui.volume,728,31); lv_obj_set_size(g_music_ui.volume,72,4);
  lv_obj_set_ext_click_area(g_music_ui.volume,18);
  lv_slider_set_range(g_music_ui.volume,0,100);
  lv_obj_set_style_bg_color(g_music_ui.volume,lv_color_hex(g_music_ui.surface),LV_PART_MAIN);
  lv_obj_set_style_pad_all(g_music_ui.volume,2,LV_PART_KNOB);
  lv_obj_set_style_bg_color(g_music_ui.volume,lv_color_hex(g_music_ui.accent),LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(g_music_ui.volume,lv_color_hex(g_music_ui.accent),LV_PART_KNOB);
  lv_obj_add_event_cb(g_music_ui.volume,music_ui_volume,LV_EVENT_VALUE_CHANGED,NULL);
  glass_music_start();
  g_music_ui.timer=lv_timer_create(music_ui_refresh,250,NULL);
  g_music_ui.sync_timer=lv_timer_create(music_ui_sync_tick,50,NULL);
  lv_obj_update_layout(card);
  music_ui_refresh(NULL);
}
