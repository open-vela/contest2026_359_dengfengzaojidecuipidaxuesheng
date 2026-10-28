/* SPDX-License-Identifier: Apache-2.0
 * 32 px status area; the combined status island opens settings when tapped.
 * Updated by the desktop's existing one-second timer; compare owned strings
 * because LV_LABEL_LONG_DOT may change the label's internal text buffer.
 */
static struct {
  lv_obj_t *bar, *network, *music, *island, *bluetooth, *ble_count, *battery, *battery_text, *charging, *date;
  lv_obj_t *bars[4];
  uint32_t background;
  char time[16];
  unsigned network_state, play_state;
  unsigned signal_level;
  unsigned bluetooth_count;
  int battery_percent;
  bool valid, light, signal_bars, charge_state;
} g_status_ui;

/* Text shares a baseline; square icon cells share the status row's center. */
static void status_align_label(lv_obj_t *label)
{
  const lv_font_t *font=lv_obj_get_style_text_font(label,0);
  lv_obj_set_y(label,22-font->line_height+font->base_line);
}

static uint32_t status_ink(void)
{ return g_desktop.light_theme ? 0x55515f : 0xc9c4d2; }

static lv_obj_t *status_icon(lv_obj_t *parent, const char *symbol, int x, int size)
{
  /* The authored icon font adds four trailing line pixels outside its square
   * glyph cell. Centering its line box or using the text baseline raises it. */
  return home_icon(parent,symbol,x,(GLASS_STATUS_HEIGHT-size)/2,size,status_ink());
}

static lv_obj_t *status_text(lv_obj_t *parent, const char *text,
                             int x, int y, int w)
{
  lv_obj_t *label=home_text(parent,text,x,y,w,14,status_ink());
  status_align_label(label);return label;
}


static void glass_status_visibility(bool locked)
{
  if (!g_status_ui.bar) return;
  if (locked) lv_obj_add_flag(g_status_ui.bar, LV_OBJ_FLAG_HIDDEN);
  else { lv_obj_remove_flag(g_status_ui.bar, LV_OBJ_FLAG_HIDDEN); glass_status_tick(); }
}

static void glass_status_tick(void)
{
  if (!g_status_ui.bar || lv_obj_has_flag(g_status_ui.bar, LV_OBJ_FLAG_HIDDEN)) return;
  bool first = !g_status_ui.valid, light = g_desktop.light_theme;
  bool retheme=first||light!=g_status_ui.light;
  lv_obj_t *page = g_desktop.current_card;
  if (!page || lv_obj_has_flag(g_desktop.panel, LV_OBJ_FLAG_HIDDEN)) page = g_glass.home;
  uint32_t background = page ? lv_color_to_u32(lv_obj_get_style_bg_color(page, 0)) & 0xffffff :
                              (light ? 0xf1f0f4 : 0x17181e);
  if (first || background != g_status_ui.background) {
    lv_obj_set_style_bg_color(g_status_ui.bar, lv_color_hex(background), 0);
    g_status_ui.background = background;
  }
  if (retheme) {
    lv_obj_t *labels[] = {g_desktop.clock_label, g_status_ui.network, g_status_ui.music,
      g_status_ui.bluetooth,g_status_ui.ble_count,g_status_ui.battery,g_status_ui.battery_text,g_status_ui.charging,g_status_ui.date};
    for (unsigned i = 0; i < sizeof(labels)/sizeof(labels[0]); i++)
      lv_obj_set_style_text_color(labels[i], lv_color_hex(status_ink()), 0);
    lv_obj_set_style_bg_color(g_status_ui.island, lv_color_hex(status_ink()), 0);
    lv_obj_set_style_border_color(g_status_ui.island, lv_color_hex(status_ink()), LV_STATE_PRESSED);
    g_status_ui.light = light;
  }
  char text[16]; struct tm local; time_t now = time(NULL);
#ifndef CONFIG_LIBC_LOCALTIME
  now += GLASS_CLOCK_OFFSET_SECONDS;
#endif
  if (now > 1700000000 && localtime_r(&now, &local)) strftime(text, sizeof(text), "%H:%M", &local);
  else strcpy(text, "--:--");
  if (first || strcmp(text, g_status_ui.time)) {
    strcpy(g_status_ui.time, text); lv_label_set_text(g_desktop.clock_label, text);
  }
  char date[64];
  if(now>1700000000&&localtime_r(&now,&local))snprintf(date,sizeof(date),"%d/%d",local.tm_mon+1,local.tm_mday);
  else snprintf(date,sizeof(date),glass_tr("时钟待校准"));
  if(strcmp(lv_label_get_text(g_status_ui.date),date))lv_label_set_text(g_status_ui.date,date);
  struct glass_status_network net = {0};
#ifdef CONFIG_SYSTEM_C6_DESKTOP
  glass_status_network_get(&net);
#endif
  unsigned network = net.wired ? 3 : net.wifi ? 2 : net.joining ? 1 : 0;
  if (first || network != g_status_ui.network_state) {
    lv_label_set_text(g_status_ui.network, network == 3 ? LV_SYMBOL_USB : LV_SYMBOL_WIFI);
    lv_obj_set_style_opa(g_status_ui.network, network >= 2 ? LV_OPA_80 : LV_OPA_30, 0);
    g_status_ui.network_state = network;
  }
  unsigned level=net.wifi&&net.rssi_valid?(net.rssi_dbm>=-55?4:net.rssi_dbm>=-67?3:net.rssi_dbm>=-78?2:1):0;
  bool bars=network==2&&net.rssi_valid;
  if(retheme||bars!=g_status_ui.signal_bars||level!=g_status_ui.signal_level){
    if(bars)lv_obj_add_flag(g_status_ui.network,LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(g_status_ui.network,LV_OBJ_FLAG_HIDDEN);
    for(unsigned i=0;i<4;i++){
      if(bars)lv_obj_remove_flag(g_status_ui.bars[i],LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(g_status_ui.bars[i],LV_OBJ_FLAG_HIDDEN);
      if(retheme)lv_obj_set_style_bg_color(g_status_ui.bars[i],lv_color_hex(status_ink()),0);
      lv_obj_set_style_bg_opa(g_status_ui.bars[i],i<level?LV_OPA_COVER:LV_OPA_20,0);
    }
    g_status_ui.signal_bars=bars;g_status_ui.signal_level=level;
  }
  /* Each connected business contributes one peer; discoveries do not. */
  struct glass_ble_hid_state ble={0};glass_ble_hid_read(&ble);
  struct glass_ble_state central={0};glass_ble_read(&central);
  unsigned bluetooth_count=(unsigned)ble.connected+
    (central.session_count?central.connection_count:
     (unsigned)(central.connected&&central.peer_valid));
  if(first||bluetooth_count!=g_status_ui.bluetooth_count)
    lv_obj_set_style_opa(g_status_ui.bluetooth,bluetooth_count?LV_OPA_COVER:LV_OPA_40,0);
  g_status_ui.bluetooth_count=bluetooth_count;
  char count[8];snprintf(count,sizeof(count),"%u",g_status_ui.bluetooth_count);
  if(strcmp(lv_label_get_text(g_status_ui.ble_count),count))lv_label_set_text(g_status_ui.ble_count,count);
  int percent=-1;bool charging=false;glass_status_power_read(&percent,&charging);
  g_status_ui.battery_percent=percent;
  if(first||charging!=g_status_ui.charge_state){
    if(charging)lv_obj_remove_flag(g_status_ui.charging,LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(g_status_ui.charging,LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_x(g_status_ui.battery_text,charging?128:119);
    lv_obj_set_width(g_status_ui.battery_text,charging?44:51);
    g_status_ui.charge_state=charging;
  }
  const char *symbol=percent<0?LV_SYMBOL_BATTERY_EMPTY:
    percent>80?LV_SYMBOL_BATTERY_FULL:percent>55?LV_SYMBOL_BATTERY_3:percent>30?LV_SYMBOL_BATTERY_2:
    percent>10?LV_SYMBOL_BATTERY_1:LV_SYMBOL_BATTERY_EMPTY;
  if(strcmp(lv_label_get_text(g_status_ui.battery),symbol))lv_label_set_text(g_status_ui.battery,symbol);
  char capacity[12];if(percent>=0)snprintf(capacity,sizeof(capacity),"%d%%",percent);else strcpy(capacity,"--%");
  if(strcmp(lv_label_get_text(g_status_ui.battery_text),capacity))lv_label_set_text(g_status_ui.battery_text,capacity);
  struct music_now_playing music; glass_music_now_playing(&music);
  bool playing = music.state == MUSIC_PLAYING && !music.pause_requested;
  unsigned state = playing ? 1 : music.state == MUSIC_LOADING ? 2 : 0;
  if (first || state != g_status_ui.play_state) {
    lv_label_set_text(g_status_ui.music, state ? LV_SYMBOL_AUDIO : LV_SYMBOL_PAUSE);
    g_status_ui.play_state = state;
  }
  bool active = music.name[0] && (playing || music.state == MUSIC_PAUSED || music.state == MUSIC_LOADING || music.pause_requested);
  /* Removing HIDDEN invalidates even an already visible LVGL object. */
  bool music_hidden=lv_obj_has_flag(g_status_ui.music,LV_OBJ_FLAG_HIDDEN);
  if(active&&music_hidden)lv_obj_remove_flag(g_status_ui.music,LV_OBJ_FLAG_HIDDEN);
  else if(!active&&!music_hidden)lv_obj_add_flag(g_status_ui.music,LV_OBJ_FLAG_HIDDEN);
  g_status_ui.valid = true;
}

static void glass_status_create(void)
{
  lv_obj_t *bar = glass_box(lv_layer_top(), 0, 0, 1024, GLASS_STATUS_HEIGHT, detail_paper(), 0);
  g_status_ui.bar = g_desktop.statusbar = bar;
  lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
  g_desktop.clock_label = status_text(bar, "--:--", 24, 8, 48);
  g_status_ui.date=status_text(bar,"",84,8,190);
  g_status_ui.music = status_icon(bar, LV_SYMBOL_AUDIO, 818, 14);
  lv_obj_t *island=glass_box(bar,830,0,178,32,home_muted(),16);
  g_status_ui.island=island;
  glass_clickable(island,settings_clicked,NULL);
  lv_obj_set_style_bg_opa(island,LV_OPA_TRANSP,0);
  lv_obj_set_style_border_width(island,0,0);
  lv_obj_set_style_shadow_width(island,0,0);
  lv_obj_set_style_bg_opa(island,LV_OPA_20,LV_STATE_PRESSED);
  lv_obj_set_style_border_width(island,1,LV_STATE_PRESSED);
  lv_obj_set_style_border_color(island,lv_color_hex(status_ink()),LV_STATE_PRESSED);
  g_status_ui.network = status_icon(island, LV_SYMBOL_WIFI, 14, 14);
  for(unsigned i=0;i<4;i++){
    unsigned height=4+i*3;
    g_status_ui.bars[i]=glass_box(island,14+i*5,(GLASS_STATUS_HEIGHT+13)/2-height,3,height,home_muted(),1);
    lv_obj_remove_flag(g_status_ui.bars[i],LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
  }
  g_status_ui.bluetooth=status_icon(island,LV_SYMBOL_BLUETOOTH,48,14);
  g_status_ui.ble_count=status_text(island,"0",66,8,18);
  g_status_ui.battery=status_icon(island,LV_SYMBOL_BATTERY_EMPTY,92,16);
  g_status_ui.charging=status_icon(island,LV_SYMBOL_CHARGE,110,14);
  g_status_ui.battery_text=status_text(island,"--%",119,8,51);
  glass_status_tick();
}
