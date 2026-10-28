/* SPDX-License-Identifier: Apache-2.0
 * Native LVGL implementation of the reviewed 1024 x 600 refined home.
 * No screenshot background, browser, per-frame bitmap or simulated telemetry.
 * Shared helpers and application lifetimes remain owned by glass_ui.inc.
 */
#include "glass_icons.h"
#include "glass_motion.h"
#include "glass_heading.h"

static lv_font_t *g_home_font12;
static lv_font_t *g_home_font14;
static lv_font_t *g_home_font32;
static lv_font_t *g_home_font34;
static lv_font_t *g_home_font40;
static lv_font_t g_hass_font20;
static lv_font_t *g_home_font64;

static void glass_home_fonts_init(const void *data, size_t bytes)
{
  g_home_font12 = lv_tiny_ttf_create_data_ex(data, bytes, 12,
    LV_FONT_KERNING_NORMAL, 128);
  g_home_font14 = lv_tiny_ttf_create_data_ex(data, bytes, 14,
    LV_FONT_KERNING_NORMAL, 192);
  g_home_font32 = lv_tiny_ttf_create_data_ex(data, bytes, 32,
    LV_FONT_KERNING_NORMAL, 24);
  g_home_font34 = lv_tiny_ttf_create_data_ex(data, bytes, 34,
    LV_FONT_KERNING_NORMAL, 32);
  g_home_font40 = lv_tiny_ttf_create_data_ex(data, bytes, 40,
    LV_FONT_KERNING_NORMAL, 32);
  g_home_font64 = lv_tiny_ttf_create_data_ex(data, bytes, 64,
    LV_FONT_KERNING_NORMAL, 20);
  /* The bundled CJK font has no degree mark. Native fallback avoids a tofu
   * box in temperature labels while keeping all text independently rendered. */
  if (g_home_font64) g_home_font64->fallback = glass_icon_font(64);
  if (g_home_font40) g_home_font40->fallback = glass_icon_font(32);
  /* A degree mark is text, so use its compact typographic advance rather
   * than the square icon-cell advance used by the shared icon fallback. */
  if (g_desktop.font20) {
    g_hass_font20 = *g_desktop.font20;
    g_hass_font20.fallback = &lv_font_montserrat_24;
  }
}

static uint32_t home_ink(void)
{ return g_desktop.light_theme ? 0x242430 : 0xf0edf6; }
static uint32_t home_muted(void)
{ return g_desktop.light_theme ? 0x787682 : 0xa6a2b2; }
static uint32_t home_paper(void)
{ return g_desktop.light_theme ? 0xfcfbfd : 0x272830; }
static const lv_font_t *home_font(int size)
{
  if (size == 12 && g_home_font12) return g_home_font12;
  if (size == 14 && g_home_font14) return g_home_font14;
  if (size == 32 && g_home_font32) return g_home_font32;
  if (size == 34 && g_home_font34) return g_home_font34;
  if (size == 64 && g_home_font64) return g_home_font64;
  return zh_font(size);
}

/* Only HA requests this reading face; other quick apps retain their fonts. */
static const lv_font_t *hass_qpk_font(int size)
{
  if (size == 40 && g_home_font40) return g_home_font40;
  if (size == 20 && g_desktop.font20) return &g_hass_font20;
  return zh_font(size);
}

static lv_obj_t *home_text(lv_obj_t *parent, const char *text,
                           int x, int y, int w, int size, uint32_t color)
{
  lv_obj_t *label = glass_text(parent, text, x, y, w, size, color);
  lv_obj_set_style_text_font(label, home_font(size), 0);
  return label;
}

static lv_obj_t *home_heading(lv_obj_t *parent,const char *text,
                              int x,int y,int w,int size,uint32_t color)
{
  lv_obj_t *label=home_text(parent,text,x,y,w,size,color);
  lv_obj_set_style_text_font(label,glass_heading_font(size,home_font(size)),0);
  return label;
}

static lv_obj_t *home_icon(lv_obj_t *parent, const char *symbol,
                           int x, int y, int size, uint32_t color)
{
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, symbol);
  lv_obj_set_style_text_font(label, glass_icon_font(size), 0);
  lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
  lv_obj_set_pos(label, x, y);
  lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  return label;
}

static unsigned g_home_devices_painted;

static void home_device_open(lv_event_t *event)
{
  unsigned index = (uintptr_t)lv_event_get_user_data(event);
  if (index >= g_home_devices.count) return;
  /* Capture identity before launching: app startup can publish a new ranking. */
  struct home_device_s row = g_home_devices.rows[index];
  char server[sizeof(g_home_devices.server)];
  strcpy(server, g_home_devices.server);
  glass_homeassistant(NULL);
  qpk_runtime_open_ha_device(server, row.entity);
}

static void glass_home_devices_paint(void)
{
  if (!g_glass.device_empty || g_home_devices_painted == g_home_devices.revision) return;
  g_home_devices_painted = g_home_devices.revision;
  for (unsigned i = 0; i < 2; i++)
    {
      if (i >= g_home_devices.count) lv_obj_add_flag(g_glass.device_rows[i], LV_OBJ_FLAG_HIDDEN);
      else
        {
          lv_obj_remove_flag(g_glass.device_rows[i], LV_OBJ_FLAG_HIDDEN);
          lv_label_set_text(g_glass.device_names[i], g_home_devices.rows[i].name);
          lv_label_set_text(g_glass.device_states[i], g_home_devices.rows[i].state);
          lv_label_set_text(g_glass.device_pins[i], g_home_devices.rows[i].pinned ? glass_tr("固定") : glass_tr("常用"));
        }
    }
  if (g_home_devices.count) lv_obj_add_flag(g_glass.device_empty, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(g_glass.device_empty, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *home_button(lv_obj_t *parent, const char *text,
                             const char *symbol, int x, int y, int w,
                             uint32_t fill, uint32_t color,
                             lv_event_cb_t callback, void *data)
{
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_remove_style_all(button);
  lv_obj_set_pos(button, x, y);
  lv_obj_set_size(button, w, 44);
  lv_obj_set_style_radius(button, 11, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(fill), 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_opa(button, 210, LV_STATE_PRESSED);
  glass_motion_button(button);
  lv_obj_set_style_bg_opa(button, LV_OPA_40, LV_STATE_DISABLED);
  lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
  glass_input_guard_install(button);
  if (callback) lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, data);
  if (text && text[0])
    {
      int offset = symbol ? 27 : 0;
      lv_obj_t *label = home_text(button, text, offset, 0,
        w - offset - 6, 14, color);
      lv_label_set_long_mode(label,LV_LABEL_LONG_DOT);
      lv_obj_set_height(label,lv_obj_get_style_text_font(label,0)->line_height);
      lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_align(label, LV_ALIGN_LEFT_MID, offset, 0);
    }
  if (symbol)
    {
      lv_obj_t *label = home_icon(button, symbol, 0, 0, 16, color);
      if (text && text[0]) lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);
      else lv_obj_center(label);
    }
  return button;
}

static lv_obj_t *home_card(lv_obj_t *root, unsigned index,
                           int x, int y, int w, int h, uint32_t color)
{
  lv_obj_t *card = glass_box(root, x, y, w, h, color, 20);
  lv_obj_set_style_clip_corner(card, true, 0);
  g_glass.home_cards[index] = card;
  return card;
}

static void home_rule(lv_obj_t *parent, int x, int y, int w, uint32_t color)
{
  lv_obj_t *rule = glass_box(parent, x, y, w, 1, color, 0);
  lv_obj_set_style_bg_opa(rule, 35, 0);
  lv_obj_remove_flag(rule, LV_OBJ_FLAG_CLICKABLE);
}

static void home_arrow(lv_obj_t *parent, lv_event_cb_t callback)
{
  lv_obj_t *button = home_button(parent, NULL, GLASS_ICON_ARROW_NE,
    0, 7, 44,
    lv_color_to_u32(lv_obj_get_style_bg_color(parent, 0)),
    home_muted(), callback, NULL);
  /* Parent geometry is not resolved while the home is being constructed. */
  lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -11, 7);
  lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
}

static void home_chat_input(lv_event_t *event)
{
  glass_chat_open(event);
  if (g_chat_ui.root) chat_ui_editor_open(NULL);
  if (event && lv_event_get_user_data(event) && g_chat_ui.keyboard)
    glass_ime_start_voice(g_chat_ui.keyboard);
}

static void home_vision_mode(lv_event_t *event)
{
  unsigned mode = (unsigned)(uintptr_t)lv_event_get_user_data(event);
  launch_builtin_espdl_qapp(NULL);
  if (qpk_runtime_select_espdl_mode(mode) < 0)
    show_toast(glass_tr("识别页面未就绪，请重新打开"));
}

/* Improve dashboard reading sizes without changing fonts in other pages. */
static void home_reading_style(lv_obj_t *object)
{
  if(lv_obj_has_class(object,&lv_label_class)){
    const lv_font_t *font=lv_obj_get_style_text_font(object,0);
    if(font==home_font(12))lv_obj_set_style_text_font(object,home_font(14),0);
    else if(font==home_font(14)&&lv_obj_check_type(lv_obj_get_parent(object),&lv_button_class))
      lv_obj_set_style_text_font(object,home_font(16),0);
    if(lv_obj_check_type(lv_obj_get_parent(object),&lv_button_class))
      lv_obj_set_height(object,lv_obj_get_style_text_font(object,0)->line_height);
  }
  for(unsigned i=0;i<lv_obj_get_child_count(object);i++)home_reading_style(lv_obj_get_child(object,i));
}

/* Cards keep their final geometry. Dynamic values use the semantic cache. */
static void glass_home_finish(void)
{
  home_reading_style(g_glass.home);
  glass_motion_enter(g_glass.home,false,0);
}

/* A retained view of the same player used by the Music application. The
 * desktop's existing second tick refreshes it only while home is visible. */
static struct {
  lv_obj_t *root,*title,*artist,*album,*status,*progress,*previous,*next,*fade;
  lv_obj_t *elapsed,*duration_label,*dot,*play;
  struct music_cover_view cover;
  char name[128],singer[96],record[128];
  unsigned seconds,duration,progress_value;
  enum music_state state;
  bool valid,paused;
} g_home_music;

static void home_music_deleted(lv_event_t *event)
{
  if(g_home_music.root==lv_event_get_target(event))
    {music_cover_reset(&g_home_music.cover);memset(&g_home_music,0,sizeof(g_home_music));}
}

static void home_music_obscure(void)
{
  if(g_home_music.cover.requested&&!g_home_music.cover.done){
    glass_lock_cover_cancel(GLASS_COVER_HOME);g_home_music.cover.requested=false;
  }
}

static void home_music_play_draw(lv_event_t *event)
{
  lv_obj_t *button=lv_event_get_target(event);
  lv_area_t area;lv_obj_get_coords(button,&area);
  lv_layer_t *layer=lv_event_get_layer(event);
  lv_color_t ink=lv_obj_get_style_text_color(button,0);
  /* Paint the visible shape about the circle's center, independent of font
   * ascenders, baseline and glyph bearings. Even heights give equal padding
   * inside the even-sized button, including its half-pixel center. */
  int left=area.x1+(lv_area_get_width(&area)-14)/2;
  int top=area.y1+(lv_area_get_height(&area)-18)/2;
  bool playing=(g_home_music.state==MUSIC_PLAYING||g_home_music.state==MUSIC_LOADING)&&!g_home_music.paused;
  if(playing){
    lv_draw_rect_dsc_t draw;lv_draw_rect_dsc_init(&draw);
    draw.bg_color=ink;draw.radius=1;
    lv_area_t bar={left,top,left+3,top+17};
    lv_draw_rect(layer,&draw,&bar);
    bar.x1+=10;bar.x2+=10;lv_draw_rect(layer,&draw,&bar);
  }else{
    lv_draw_triangle_dsc_t draw;lv_draw_triangle_dsc_init(&draw);
    draw.bg_color=ink;
    /* Slightly right of the bounds center balances the triangle's mass. */
    draw.p[0]=(lv_point_precise_t){left+1,top};
    draw.p[1]=(lv_point_precise_t){left+1,top+18};
    draw.p[2]=(lv_point_precise_t){left+16,top+9};
    lv_draw_triangle(layer,&draw);
  }
}

static void home_music_refresh(void)
{
  if(!g_home_music.root)return;
  struct music_now_playing now;
  glass_music_now_playing(&now);
  /* Artwork can arrive independently of playback's revision/second tick. */
  if(!glass_page_covers_home()&&glass_ui_can_paint(g_home_music.root))
    music_cover_tick(&g_home_music.cover,&now);
  /* Only feather actual artwork. Alpha blending a solid fallback onto the
   * same RGB565 surface can otherwise leave a faint quantization rectangle. */
  if(g_home_music.cover.canvas)lv_obj_remove_flag(g_home_music.fade,LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(g_home_music.fade,LV_OBJ_FLAG_HIDDEN);
  unsigned seconds=now.position_ms/1000;
  unsigned duration=now.duration_ms/1000;
  bool paused=now.pause_requested||now.state==MUSIC_PAUSED;
  bool track_changed=!g_home_music.valid||strcmp(g_home_music.name,now.name);
  if(track_changed){
    snprintf(g_home_music.name,sizeof(g_home_music.name),"%s",now.name);
    lv_label_set_text(g_home_music.title,now.name[0]?now.name:glass_tr("听见喜欢的声音"));
  }
  if(track_changed||strcmp(g_home_music.singer,now.artist)){
    snprintf(g_home_music.singer,sizeof(g_home_music.singer),"%s",now.artist);
    lv_label_set_text(g_home_music.artist,now.artist[0]?now.artist:now.name[0]?glass_tr("未知歌手"):glass_tr("搜索好歌，收藏你的热爱"));
  }
  if(track_changed||strcmp(g_home_music.record,now.album)){
    snprintf(g_home_music.record,sizeof(g_home_music.record),"%s",now.album);
    lv_label_set_text(g_home_music.album,strcmp(now.album,now.name)?now.album:"");
  }
  if(now.can_skip){lv_obj_remove_state(g_home_music.next,LV_STATE_DISABLED);lv_obj_remove_state(g_home_music.previous,LV_STATE_DISABLED);}
  else {lv_obj_add_state(g_home_music.next,LV_STATE_DISABLED);lv_obj_add_state(g_home_music.previous,LV_STATE_DISABLED);}
  if(track_changed||g_home_music.seconds!=seconds||g_home_music.duration!=duration||
     g_home_music.state!=now.state||g_home_music.paused!=paused){
    const char *state=now.state==MUSIC_ERROR?glass_tr("播放失败"):now.state==MUSIC_LOADING?(paused?glass_tr("已暂停"):glass_tr("正在加载")):
      now.state==MUSIC_PLAYING?(paused?glass_tr("已暂停"):glass_tr("正在播放")):now.state==MUSIC_PAUSED?glass_tr("已暂停"):
      now.state==MUSIC_ENDED?glass_tr("播放结束"):now.state==MUSIC_STOPPED?glass_tr("已停止"):glass_tr("等待播放");
    lv_label_set_text(g_home_music.status,now.name[0]?state:glass_tr("等待播放"));
    lv_label_set_text_fmt(g_home_music.elapsed,"%02u:%02u",seconds/60,seconds%60);
    if(now.duration_ms)lv_label_set_text_fmt(g_home_music.duration_label,"%02u:%02u",duration/60,duration%60);
    else lv_label_set_text(g_home_music.duration_label,now.name[0]?"--:--":"00:00");
    lv_obj_set_style_bg_opa(g_home_music.dot,now.state==MUSIC_PLAYING&&!paused?LV_OPA_COVER:LV_OPA_40,0);
    lv_obj_invalidate(g_home_music.play);
  }
  unsigned progress=now.duration_ms?(uint64_t)now.position_ms*1000/now.duration_ms:0;
  if(progress>1000)progress=1000;
  if(!g_home_music.valid||g_home_music.progress_value!=progress){
    lv_bar_set_value(g_home_music.progress,progress,LV_ANIM_OFF);
  }
  g_home_music.seconds=seconds;g_home_music.duration=duration;
  g_home_music.state=now.state;g_home_music.paused=paused;
  g_home_music.progress_value=progress;g_home_music.valid=true;
}

static void home_music_toggle(lv_event_t *event)
{
  LV_UNUSED(event);
  struct music_now_playing now;glass_music_now_playing(&now);
  if(!now.name[0]){glass_music(NULL);return;}
  glass_music_toggle();home_music_refresh();
}

static void home_music_next(lv_event_t *event)
{
  LV_UNUSED(event);
  struct music_now_playing now;glass_music_now_playing(&now);
  if(!now.can_skip)return;
  if(glass_music_skip(1))show_toast(glass_tr("暂无可播放的下一首"));
  home_music_refresh();
}

static void home_music_previous(lv_event_t *event)
{
  LV_UNUSED(event);
  struct music_now_playing now;glass_music_now_playing(&now);
  if(!now.can_skip)return;
  if(glass_music_skip(-1))show_toast(glass_tr("暂无可播放的上一首"));
  home_music_refresh();
}

static void glass_rebuild(void)
{
  desktop_fixed_shell();
  glass_motion_set_reduced(g_glass.reduced_motion);
  const bool light = g_desktop.light_theme;
  const uint32_t ink = home_ink(), muted = light ? 0x595563 : 0xc4bfce;
  const uint32_t paper = home_paper(), accent = light ? 0x644e80 : 0xd1bce8;
  const uint32_t green = light ? 0x315b49 : 0xb7d9c8;
  const uint32_t blue = light ? 0x3d587b : 0xc2d2ed;
  const uint32_t background = light ? 0xf1f0f4 : 0x17181e;
  if (g_glass.home)
    {
      lv_obj_delete(g_glass.home);
    }
  g_glass_display.valid = false;
  lv_obj_t *root = glass_box(g_desktop.screen, 0, 0, 1024, GLASS_CONTENT_HEIGHT, background, 0);
  g_glass.home = root;
  lv_obj_move_to_index(root, 0);

  lv_obj_t *label;

  /* Persistent rail. All icons come from the same authored outline family. */
  const char *symbols[] = {GLASS_ICON_GRID, GLASS_ICON_CAMERA, LV_SYMBOL_HOME,
                          GLASS_ICON_SPARKLES, LV_SYMBOL_AUDIO, GLASS_ICON_ELLIPSIS};
  const char *names[] = {glass_tr("桌面"), glass_tr("相机"), glass_tr("家居"), "AI", glass_tr("影音"), glass_tr("应用")};
  lv_event_cb_t callbacks[] = {panel_hide, launch_builtin_camera_qapp,
    glass_homeassistant, glass_chat_open, glass_music, qpk_clicked};
  for (unsigned i = 0; i < 6; i++)
    {
      lv_obj_t *item = glass_box(root, 20, 12 + i * 80, 68, 72,
                                i ? background : paper, 17);
      if(i==0)lv_obj_add_state(item,LV_STATE_CHECKED);
      lv_obj_t *icon = home_icon(item, symbols[i], 0, 0, 20, i ? muted : accent);
      lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 12);
      label = home_text(item, names[i], 0, 47, 68, 12, i ? muted : accent);
      lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
      glass_clickable(item, callbacks[i], NULL);
      lv_obj_set_style_bg_color(item, lv_color_hex(paper), LV_STATE_PRESSED);
    }

  /* Current artwork is the feature, rather than unrelated demo photographs. */
  const uint32_t music_ink=light?0x26333d:0xeff3f5;
  const uint32_t music_muted=light?0x697680:0xa9b8c3;
  const uint32_t music_accent=light?0x38596c:0xa9c6d7;
  const uint32_t music_paper=light?0xf7f8fa:0x21272d;
  lv_obj_t *card = home_card(root, 0, 108, 12, 567, 280, music_paper);
  lv_obj_set_style_radius(card,24,0);
  g_home_music.root=card;
  lv_obj_add_event_cb(card,home_music_deleted,LV_EVENT_DELETE,NULL);
  /* The clear cover occupies about a third of the card. Extend the image
   * under a translucent edge so it melts into the information surface. */
  lv_obj_t *sleeve=glass_box(card,0,0,276,272,music_paper,0);
  lv_obj_remove_flag(sleeve,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *note=home_icon(sleeve,GLASS_ICON_HEADPHONES,0,0,32,light?0x8b7e9e:0xb1a6c3);
  lv_obj_set_pos(note,70,104);
  lv_obj_t *hint=home_text(sleeve,glass_tr("暂无封面"),26,156,120,12,light?0x8b7e9e:0xb1a6c3);
  lv_obj_set_style_text_align(hint,LV_TEXT_ALIGN_CENTER,0);
  g_home_music.cover.host=sleeve;g_home_music.cover.owner=GLASS_COVER_HOME;
  glass_clickable(sleeve,glass_music,NULL);
  /* Ease both ends of the feather with retained native two-stop strips.
   * This also works with the board's LV_GRADIENT_MAX_STOPS=2 configuration. */
  g_home_music.fade=glass_box(card,100,0,178,272,music_paper,0);
  lv_obj_set_style_bg_opa(g_home_music.fade,LV_OPA_TRANSP,0);
  lv_obj_remove_flag(g_home_music.fade,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
  static const uint8_t feather[]={0,20,64,128,192,235,255};
  for(unsigned i=0;i<6;i++){
    int left=176*i/6,right=176*(i+1)/6;
    lv_obj_t *strip=glass_box(g_home_music.fade,left,0,right-left,272,music_paper,0);
    lv_obj_set_style_bg_grad_color(strip,lv_color_hex(music_paper),0);
    lv_obj_set_style_bg_grad_dir(strip,LV_GRAD_DIR_HOR,0);
    lv_obj_set_style_bg_main_opa(strip,feather[i],0);
    lv_obj_set_style_bg_grad_opa(strip,feather[i+1],0);
    lv_obj_set_style_bg_main_stop(strip,0,0);
    /* Settle to the exact paper color before the image's physical edge. */
    lv_obj_set_style_bg_grad_stop(strip,i==5?224:255,0);
    lv_obj_remove_flag(strip,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
  }
  /* A short opaque tail lands exactly on the card color even in RGB565. */
  lv_obj_t *landing=glass_box(g_home_music.fade,171,0,7,272,music_paper,0);
  lv_obj_remove_flag(landing,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
  g_home_music.dot=glass_box(card,247,30,4,4,music_accent,2);
  lv_obj_remove_flag(g_home_music.dot,LV_OBJ_FLAG_CLICKABLE);
  g_home_music.status=home_text(card,"",260,23,185,14,music_accent);
  lv_obj_set_height(g_home_music.status,26);
  lv_obj_t *open=home_button(card,NULL,GLASS_ICON_ARROW_NE,499,8,44,
    0xffffff,music_muted,glass_music,NULL);
  lv_obj_set_style_bg_opa(open,LV_OPA_TRANSP,0);
  lv_obj_set_style_bg_opa(open,LV_OPA_10,LV_STATE_PRESSED);
  lv_obj_set_style_radius(open,22,0);
  label = home_heading(card, glass_tr("听见喜欢的声音"), 244, 64, 291, 34, music_ink);
  g_home_music.title=label;
  lv_obj_set_height(label,44);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_line_space(label, 2, 0);
  g_home_music.artist=home_text(card, "", 244, 111, 291, 16, music_muted);
  lv_obj_set_height(g_home_music.artist,22);
  g_home_music.album=home_text(card,"",244,138,291,12,music_muted);
  lv_obj_set_height(g_home_music.album,22);
  g_home_music.elapsed=home_text(card,"",244,174,72,14,music_muted);
  lv_obj_set_height(g_home_music.elapsed,26);
  g_home_music.duration_label=home_text(card,"",463,174,72,14,music_muted);
  lv_obj_set_height(g_home_music.duration_label,26);
  lv_obj_set_style_text_align(g_home_music.duration_label,LV_TEXT_ALIGN_RIGHT,0);
  g_home_music.progress=lv_bar_create(card);
  lv_obj_remove_style_all(g_home_music.progress);
  lv_obj_set_pos(g_home_music.progress,244,163);lv_obj_set_size(g_home_music.progress,291,3);
  lv_obj_set_style_radius(g_home_music.progress,2,0);
  lv_obj_set_style_radius(g_home_music.progress,2,LV_PART_INDICATOR);
  lv_bar_set_range(g_home_music.progress,0,1000);
  lv_obj_set_style_bg_color(g_home_music.progress,lv_color_hex(music_muted),0);
  lv_obj_set_style_bg_opa(g_home_music.progress,LV_OPA_20,0);
  lv_obj_set_style_bg_color(g_home_music.progress,lv_color_hex(music_accent),LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(g_home_music.progress,LV_OPA_COVER,LV_PART_INDICATOR);
  lv_obj_remove_flag(g_home_music.progress,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
  g_home_music.previous=home_button(card,NULL,LV_SYMBOL_PREV,296,211,44,
    0xffffff,music_ink,home_music_previous,NULL);
  lv_obj_set_style_bg_opa(g_home_music.previous,LV_OPA_TRANSP,0);
  lv_obj_set_style_bg_opa(g_home_music.previous,LV_OPA_10,LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(g_home_music.previous,LV_OPA_TRANSP,LV_STATE_DISABLED);
  lv_obj_set_style_opa(g_home_music.previous,LV_OPA_40,LV_STATE_DISABLED);
  lv_obj_set_style_text_font(lv_obj_get_child(g_home_music.previous,0),glass_icon_font(20),0);
  lv_obj_t *play=home_button(card,NULL,NULL,362,207,52,
    music_accent,light?0xffffff:0x17232d,home_music_toggle,NULL);
  lv_obj_set_height(play,52);lv_obj_set_style_radius(play,26,0);
  lv_obj_set_style_text_color(play,lv_color_hex(light?0xffffff:0x17232d),0);
  lv_obj_add_event_cb(play,home_music_play_draw,LV_EVENT_DRAW_MAIN,NULL);
  g_home_music.play=play;
  g_home_music.next=home_button(card,NULL,LV_SYMBOL_NEXT,436,211,44,
    0xffffff,music_ink,home_music_next,NULL);
  lv_obj_set_style_bg_opa(g_home_music.next,LV_OPA_TRANSP,0);
  lv_obj_set_style_bg_opa(g_home_music.next,LV_OPA_10,LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(g_home_music.next,LV_OPA_TRANSP,LV_STATE_DISABLED);
  lv_obj_set_style_opa(g_home_music.next,LV_OPA_40,LV_STATE_DISABLED);
  lv_obj_set_style_text_font(lv_obj_get_child(g_home_music.next,0),glass_icon_font(20),0);
  /* LONG_DOT measures against retained coordinates. Resolve the new card's
   * geometry before the first dynamic text, including an initially empty label. */
  lv_obj_update_layout(card);
  g_home_music.valid=false;home_music_refresh();

  /* City and current conditions sit on a retained native sky illustration. */
  card = home_card(root, 3, 693, 12, 307, 280, 0x244765);
  memset(&g_weather_home,0,sizeof(g_weather_home));
  g_weather_home.data=calloc(1,sizeof(*g_weather_home.data));
  lv_obj_add_event_cb(card,weather_home_deleted,LV_EVENT_DELETE,NULL);
  weather_scene_attach(&g_weather_home.scene,card,307,280,true);
  g_weather_home.city = home_heading(card,glass_tr("当地天气"),22,18,222,20,0xffffff);
  lv_obj_t *weather_more = home_button(card,NULL,GLASS_ICON_ARROW_NE,
    252,7,44,0xffffff,0xe8f3ff,glass_weather_open,NULL);
  lv_obj_set_style_bg_opa(weather_more,LV_OPA_TRANSP,0);
  g_glass.temperature = home_text(card,"--°",20,51,168,64,0xffffff);
  g_glass.weather_condition = home_text(card,glass_tr("等待联网"),23,126,255,16,0xffffff);
  g_weather_home.status = home_text(card,glass_tr("连接网络后更新"),23,155,264,12,0xe1edf7);
  g_glass.widget_title = home_text(card,glass_tr("空气质量"),22,197,130,12,0xd1e2ef);
  g_glass.widget_value = home_text(card,"--",22,217,93,28,0xffffff);
  g_glass.air = home_text(card,"AQI",110,231,40,12,0xd1e2ef);
  g_glass.widget_badge = glass_box(card,22,250,136,20,0x244765,0);
  lv_obj_set_style_bg_opa(g_glass.widget_badge, LV_OPA_TRANSP, 0);
  /* The AQI category shares the title row so all text fits the 272px card. */
  lv_obj_set_pos(g_glass.widget_badge,88,197);
  lv_obj_set_size(g_glass.widget_badge,68,20);
  g_glass.widget_category = home_text(g_glass.widget_badge,glass_tr("暂无数据"),0,0,68,12,0xc0edce);
  lv_obj_set_style_text_align(g_glass.widget_category,LV_TEXT_ALIGN_RIGHT,0);
  lv_obj_t *vertical = glass_box(card,171,201,1,51,0xffffff,0);
  lv_obj_set_style_bg_opa(vertical,35,0);
  lv_obj_remove_flag(vertical, LV_OBJ_FLAG_CLICKABLE);
  home_text(card,glass_i18n_language()==GLASS_LANGUAGE_EN?"RH":glass_tr("湿度"),191,197,42,12,0xd1e2ef);
  g_glass.humidity = home_text(card,"--%",191,217,100,28,0xffffff);
  g_glass.humidity_status = home_text(card,glass_tr("暂无数据"),233,197,56,12,0xd1e2ef);
  lv_obj_set_style_text_align(g_glass.humidity_status,LV_TEXT_ALIGN_RIGHT,0);
  /* The selectable widget retains its existing semantic updater. */
  g_glass.widget_hint = home_text(card, glass_tr("等待联网"), 0, 0, 260, 12, muted);
  lv_obj_add_flag(g_glass.widget_hint, LV_OBJ_FLAG_HIDDEN);
  glass_clickable(card, glass_widget_open, NULL);

  /* Bottom cards share geometry, typography and 44 px touch targets. */
  card = home_card(root, 1, 108, 310, 285, 192, light ? 0xe9e2f2 : 0x302a3e);
  lv_obj_set_style_text_color(card,lv_color_hex(ink),0);
  struct glass_grok *home_ball=glass_grok_create_pixel(card,285,!g_desktop.light_theme,zh_font);
  glass_grok_set_notice_text(home_ball,glass_tr("加载小球…"),glass_tr("小球素材未就绪"));
  glass_grok_set_ambient(home_ball,true);
  glass_grok_set_reduced_motion(home_ball,g_glass.reduced_motion);
  glass_grok_set_obscured_by(home_ball,g_desktop.panel);
  home_arrow(card, glass_chat_open);
  home_rule(card,21,99,243,accent);
  home_heading(card,glass_tr("AI 助手"),21,105,100,20,ink);
  label=home_text(card,glass_tr("文字 · 语音"),128,108,136,12,muted);
  lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_RIGHT,0);
  home_button(card, glass_tr("开始对话"), GLASS_ICON_CHAT, 21, 135, 143,
    light ? 0xf7f4fa : 0x3d354a, accent, home_chat_input, NULL);
  home_button(card, NULL, GLASS_ICON_MIC, 171, 135, 44,
    light ? 0xf7f4fa : 0x3d354a, accent, home_chat_input, (void *)1);
  glass_clickable(card, glass_chat_open, NULL);

  card = home_card(root, 2, 411, 310, 285, 192, light ? 0xe0ebe6 : 0x26372f);
  home_heading(card, glass_tr("智能家居"), 21, 18, 200, 20, ink);
  home_arrow(card, glass_homeassistant);
  for (unsigned i = 0; i < 2; i++)
    {
      lv_obj_t *row = glass_box(card, 21, 53 + i * 46, 243, 42,
                                light ? 0xf2f7f4 : 0x32473c, 10);
      lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
      glass_clickable(row, home_device_open, (void *)(uintptr_t)i);
      g_glass.device_rows[i] = row;
      g_glass.device_names[i] = home_text(row, "", 10, 3, 183, 14, ink);
      g_glass.device_states[i] = home_text(row, "", 10, 23, 220, 12, muted);
      g_glass.device_pins[i] = home_text(row, "", 203, 5, 32, 12, green);
    }
  g_glass.device_empty = home_text(card, glass_tr("进入米家 HA 同步设备"), 21, 83, 243, 14, muted);
  home_text(card, glass_tr("固定优先 · 常用补齐"), 21, 165, 243, 12, muted);
  /* Keep telemetry consumers separate from the shortcuts' visible labels. */
  g_glass.devices = home_text(card, "-- / --", 0, 0, 145, 14, ink);
  g_glass.sync = home_text(card, glass_tr("尚未同步"), 0, 0, 122, 12, muted);
  lv_obj_add_flag(g_glass.devices, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(g_glass.sync, LV_OBJ_FLAG_HIDDEN);
  g_home_devices_painted = ~g_home_devices.revision;
  glass_clickable(card, glass_homeassistant, NULL);

  card = home_card(root, 4, 714, 310, 286, 192, light ? 0xe3e7f0 : 0x29313f);
  home_icon(card, GLASS_ICON_FACE, 25, 20, 20, blue);
  home_arrow(card, launch_builtin_camera_qapp);
  home_heading(card, glass_tr("相机与识别"), 21, 60, 245, 20, ink);
  home_text(card, glass_tr("共用相机，切换不同任务"), 21, 90, 245, 12, muted);
  uint32_t chip = light ? 0xf5f6fa : 0x354154;
  home_button(card, glass_tr("拍摄"), NULL, 21, 135, 61, chip, blue, launch_builtin_camera_qapp, NULL);
  home_button(card, glass_tr("人脸跟随"), NULL, 88, 135, 103, chip, blue, home_vision_mode, (void *)1);
  home_button(card, glass_tr("分类"), NULL, 197, 135, 65, chip, blue, home_vision_mode, NULL);
  glass_clickable(card, launch_builtin_camera_qapp, NULL);

  home_text(root, "VelaDesk", 108, 528, 190, 12, muted);
  lv_obj_t *files = home_button(root, glass_tr("文件"), LV_SYMBOL_DIRECTORY,
    739, 514, 73, background, muted, glass_files_open, NULL);
  lv_obj_t *pet = home_button(root, glass_tr("桌宠"), GLASS_ICON_FISH,
    817, 514, 73, background, muted, launch_builtin_dafeiyu_qapp, NULL);
  lv_obj_t *portal = home_button(root, glass_tr("扫码配置"), GLASS_ICON_QR,
    895, 514, 105, background, muted, glass_portal_open, NULL);
  lv_obj_set_style_bg_opa(files, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_opa(pet, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_opa(portal, LV_OPA_TRANSP, 0);
  /* Explicit coordinates fill the 568px content area below the status bar.
   * Do not shift children after construction: that also moves the footer. */
  glass_update(NULL);
  glass_home_finish();
}
