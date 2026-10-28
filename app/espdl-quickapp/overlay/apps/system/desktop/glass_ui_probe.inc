/* SPDX-License-Identifier: Apache-2.0
 * NSH diagnostics enqueue work; only the desktop thread calls LVGL.
 */
#include <stdarg.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <nuttx/video/fb.h>
#include "glass_device_controls.h"
#ifdef GLASS_VIDEO_BLIT
#include "glass_video_blit.h"
#endif

static pthread_mutex_t g_ui_probe_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_ui_probe_command[64];
static bool g_ui_probe_pending;
static bool g_ui_probe_done;
#define UI_PROBE_REPORT_BYTES 32768
static char *g_ui_probe_report;
static size_t g_ui_probe_length;
static unsigned g_ui_probe_id;
uint16_t *g_ui_probe_frame;
volatile size_t g_ui_probe_frame_bytes;

/* Lossless run-length capture makes USB JTAG readback practical. The UI
 * thread reads the current displayed page; GDB only reads the saved buffer.
 */
static int desktop_ui_probe_frame(void)
{
  struct fb_planeinfo_s plane;
  memset(&plane, 0, sizeof(plane));
  int fd = open("/dev/fb0", O_RDONLY);
  if (fd < 0) return -errno;
  int ret = ioctl(fd, FBIOGET_PLANEINFO, (unsigned long)&plane);
  int error = errno;
  close(fd);
  if (ret < 0) return -error;
  if (plane.bpp != 16 || plane.stride < 2048 ||
      (size_t)(plane.yoffset + 600) * plane.stride > plane.fblen) return -EINVAL;
  free(g_ui_probe_frame);
  g_ui_probe_frame_bytes = 0;
  g_ui_probe_frame = malloc(1024 * 600 * 4);
  if (!g_ui_probe_frame) return -ENOMEM;
  size_t pairs = 0;
  uint16_t value = 0;
  uint16_t count = 0;
  for (unsigned y = 0; y < 600; y++)
    {
      const uint16_t *row = (const uint16_t *)((const uint8_t *)plane.fbmem +
                            (size_t)(plane.yoffset + y) * plane.stride);
      for (unsigned x = 0; x < 1024; x++)
        {
          if (count && (row[x] != value || count == UINT16_MAX))
            {
              g_ui_probe_frame[pairs++] = count;
              g_ui_probe_frame[pairs++] = value;
              count = 0;
            }
          value = row[x];
          count++;
        }
    }
  if (count)
    { g_ui_probe_frame[pairs++] = count; g_ui_probe_frame[pairs++] = value; }
  g_ui_probe_frame_bytes = pairs * sizeof(uint16_t);
  return 0;
}

static void desktop_ui_probe_print(const char *format, ...)
{
  if (!g_ui_probe_report) return;
  size_t available = UI_PROBE_REPORT_BYTES - g_ui_probe_length;
  va_list args;
  va_start(args, format);
  int size = vsnprintf(g_ui_probe_report + g_ui_probe_length, available, format, args);
  va_end(args);
  if (size > 0) g_ui_probe_length += (size_t)size < available ? (size_t)size : available - 1;
}

static void desktop_ui_probe_string(const char *text)
{
  desktop_ui_probe_print("\"");
  for (size_t i = 0; text && text[i] && i < 192; i++)
    {
      unsigned char c = text[i];
      if (c == '"' || c == '\\') desktop_ui_probe_print("\\%c", c);
      else if (c < 32) desktop_ui_probe_print("\\u%04x", c);
      else desktop_ui_probe_print("%c", c);
    }
  desktop_ui_probe_print("\"");
}

static void desktop_ui_probe_tree(lv_obj_t *object, unsigned parent)
{
  if (!lv_obj_is_visible(object)) return;
  unsigned id = ++g_ui_probe_id;
  const char *type = "container";
  const char *text = "";
  bool input = lv_obj_check_type(object, &lv_textarea_class);
  if (lv_obj_has_class(object,&lv_label_class))
    { type = "label"; text = object == portal_code ? "[pairing code hidden]" : lv_label_get_text(object); }
  else if (input)
    { type = "input"; text = lv_textarea_get_placeholder_text(object); }
  else if (lv_obj_check_type(object, &lv_button_class)) type = "button";
  else if (lv_obj_check_type(object, &lv_switch_class)) type = "switch";
  else if (lv_obj_check_type(object, &lv_slider_class)) type = "slider";
  else if (lv_obj_check_type(object, &lv_dropdown_class)) type = "dropdown";
  else if (lv_obj_check_type(object, &lv_keyboard_class)) type = "keyboard";
  else if (lv_obj_check_type(object, &lv_image_class)) type = "image";
  lv_area_t area;
  lv_obj_get_coords(object, &area);
  desktop_ui_probe_print("%s{\"id\":%u,\"parent\":%u,\"type\":\"%s\",\"x\":%ld,\"y\":%ld,\"w\":%ld,\"h\":%ld,\"scroll\":%s,\"text\":",
      id > 1 ? ",\n" : "", id, parent, type, (long)area.x1, (long)area.y1,
      (long)lv_area_get_width(&area), (long)lv_area_get_height(&area),
      lv_obj_has_flag(object, LV_OBJ_FLAG_SCROLLABLE) ? "true" : "false");
  desktop_ui_probe_string(text);
  desktop_ui_probe_print("}");
  /* Never expose input values, including passwords, through diagnostics. */
  if (input) return;
  for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++)
    desktop_ui_probe_tree(lv_obj_get_child(object, i), id);
}

static bool desktop_ui_probe_click(lv_obj_t *object, const char *text)
{
  if (!object || !lv_obj_is_visible(object)) return false;
  if(lv_obj_check_type(object,&lv_checkbox_class)&&!lv_obj_has_state(object,LV_STATE_DISABLED)&&
     !strcmp(lv_checkbox_get_text(object),text)){
    if(lv_obj_has_state(object,LV_STATE_CHECKED))lv_obj_remove_state(object,LV_STATE_CHECKED);
    else lv_obj_add_state(object,LV_STATE_CHECKED);
    lv_obj_send_event(object,LV_EVENT_VALUE_CHANGED,NULL);return true;
  }
  if (lv_obj_has_flag(object, LV_OBJ_FLAG_CLICKABLE) && !lv_obj_has_state(object, LV_STATE_DISABLED))
    {
      for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++) {
        lv_obj_t *label = lv_obj_get_child(object, i);
        if (label && lv_obj_has_class(label,&lv_label_class) &&
          (text[0] == '*' ? strstr(lv_label_get_text(label), text + 1) != NULL :
                           !strcmp(lv_label_get_text(label), text)))
          { lv_obj_send_event(object, LV_EVENT_CLICKED, NULL); return true; }
      }
    }
  for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++)
    if (desktop_ui_probe_click(lv_obj_get_child(object, i), text)) return true;
  return false;
}

static bool desktop_ui_probe_click_at(lv_obj_t *object,int x,int y)
{
  if(!object||!lv_obj_is_visible(object))return false;
  for(uint32_t i=lv_obj_get_child_count(object);i>0;i--)
    if(desktop_ui_probe_click_at(lv_obj_get_child(object,i-1),x,y))return true;
  if(!lv_obj_check_type(object,&lv_button_class)||lv_obj_has_state(object,LV_STATE_DISABLED))return false;
  lv_area_t area;lv_obj_get_coords(object,&area);
  if(x<area.x1||x>area.x2||y<area.y1||y>area.y2)return false;
  lv_obj_send_event(object,LV_EVENT_CLICKED,NULL);return true;
}

static lv_point_t g_ui_probe_pointer_point;
static lv_indev_state_t g_ui_probe_pointer_state;
static void desktop_ui_probe_pointer_read(lv_indev_t *input,lv_indev_data_t *data)
{
  LV_UNUSED(input);
  data->point=g_ui_probe_pointer_point;data->state=g_ui_probe_pointer_state;
}
/* GUI thread only. Unlike click:, this runs native LVGL hit testing and
 * PRESSED/RELEASED/SHORT_CLICKED/CLICKED, including input guards and motion. */
static bool desktop_ui_probe_pointer_stroke(int x,int y,int end_x,int end_y)
{
  lv_display_t *display=lv_display_get_default();
  if(!display || x<0 || y<0 || x>=lv_display_get_horizontal_resolution(display) ||
     y>=lv_display_get_vertical_resolution(display) || end_x<0 || end_y<0 ||
     end_x>=lv_display_get_horizontal_resolution(display) || end_y>=lv_display_get_vertical_resolution(display))return false;
  lv_indev_t *input=lv_indev_create();if(!input)return false;
  lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);
  lv_indev_set_display(input,display);
  lv_indev_set_mode(input,LV_INDEV_MODE_EVENT);
  lv_indev_set_read_cb(input,desktop_ui_probe_pointer_read);
  g_ui_probe_pointer_point.x=x;g_ui_probe_pointer_point.y=y;
  g_ui_probe_pointer_state=LV_INDEV_STATE_PRESSED;lv_indev_read(input);
  g_ui_probe_pointer_point.x=end_x;g_ui_probe_pointer_point.y=end_y;
  g_ui_probe_pointer_state=LV_INDEV_STATE_RELEASED;lv_indev_read(input);
  lv_indev_delete(input);
  return true;
}

static bool desktop_ui_probe_pointer_tap(int x,int y)
{return desktop_ui_probe_pointer_stroke(x,y,x,y);}

#include "qpk_capacity_test.inc"
#include "qpk_yield_test.inc"

static lv_obj_t *desktop_ui_probe_form(lv_obj_t *object)
{
  if (!object || !lv_obj_is_visible(object)) return NULL;
  if (lv_obj_check_type(object, &lv_keyboard_class))
    return lv_obj_get_parent(object);
  for (uint32_t i = lv_obj_get_child_count(object); i > 0; i--)
    {
      lv_obj_t *form = desktop_ui_probe_form(lv_obj_get_child(object, i - 1));
      if (form) return form;
    }
  return NULL;
}

static void desktop_ui_probe_service(void)
{
  char command[sizeof(g_ui_probe_command)];
  pthread_mutex_lock(&g_ui_probe_lock);
  if (!g_ui_probe_pending || g_ui_probe_done)
    { pthread_mutex_unlock(&g_ui_probe_lock); return; }
  memcpy(command, g_ui_probe_command, sizeof(command));
  pthread_mutex_unlock(&g_ui_probe_lock);

  g_ui_probe_length = 0;
  g_ui_probe_report[0] = '\0';
  if (!strcmp(command, "frame"))
    {
      int ret = desktop_ui_probe_frame();
      desktop_ui_probe_print("desktop: frame result=%d bytes=%lu\n", ret,
                             (unsigned long)g_ui_probe_frame_bytes);
    }
  else if (!strcmp(command, "frame-free"))
    {
      free(g_ui_probe_frame);
      g_ui_probe_frame = NULL;
      g_ui_probe_frame_bytes = 0;
      desktop_ui_probe_print("desktop: frame released\n");
    }
#ifdef CONFIG_SYSTEM_BILIBILI_LIVE
  else if (!strcmp(command, "bilibili")) launch_bilibili(NULL);
  else if (!strcmp(command, "local-video")) launch_local_video(NULL);
  else if (!strncmp(command, "local-play:", 11)) {
    launch_local_video(NULL); local_album_open_path(command + 11);
  }
  else if (!strcmp(command, "local-stop")) panel_hide(NULL);
  else if (!strcmp(command, "local-scale-on")) local_video_set_upscale(true);
  else if (!strcmp(command, "local-scale-off")) local_video_set_upscale(false);
  else if (!strcmp(command,"video-layer-top")) local_video_set_screen_layer(false);
  else if (!strcmp(command,"video-layer-screen")) local_video_set_screen_layer(true);
  else if (!strcmp(command,"video-geometry")) local_video_draw_status(desktop_ui_probe_print);
  else if (!strcmp(command,"audio-status")) music_pcm_status();
  else if (!strncmp(command,"audio-volume:",13)) {
    unsigned value;char extra;
    if(sscanf(command+13,"%u%c",&value,&extra)==1 && value<=100){
      local_video_set_volume(value);glass_music_volume(value);
      int ret=glass_speaker_volume_set(value);
      desktop_ui_probe_print("AUDIO_VOLUME requested=%u master=%u result=%d\n",value,glass_speaker_volume(),ret);
    }else desktop_ui_probe_print("AUDIO_VOLUME invalid\n");
  }
#ifdef GLASS_VIDEO_BLIT
  else if (!strcmp(command, "video-blit-on")) glass_video_blit_set_enabled(true);
  else if (!strcmp(command, "video-blit-off")) glass_video_blit_set_enabled(false);
  else if (!strcmp(command, "video-profile-reset")) glass_video_profile_reset();
  else if (!strcmp(command, "video-profile")) glass_video_profile_report(desktop_ui_probe_print);
#endif
  else if (!strncmp(command, "bilibili-play:", 14)) {
    launch_bilibili(NULL);
    bili_live_play_room(command + 14);
  }
  else if (!strncmp(command,"huya-play:",10)) { launch_bilibili(NULL);bili_live_play_platform(1,command+10); }
  else if (!strncmp(command,"cc-play:",8)) { launch_bilibili(NULL);bili_live_play_platform(2,command+8); }
  else if (!strcmp(command, "bilibili-stop")) panel_hide(NULL);
#endif
  else if (!strcmp(command, "weather-status"))
    {
      struct glass_weather_snapshot weather;
      glass_weather_get(&weather);
      desktop_ui_probe_print("weather state=%d valid=%d error=%d revision=%u AQI=%d temp=%d humidity=%d city=%s\n",
        weather.state, weather.valid, weather.error, weather.revision,
        weather.data.aqi, weather.data.temperature, weather.data.humidity, weather.data.location);
    }
  else if (!strcmp(command, "weather-refresh")) glass_weather_refresh();
  else if (!strcmp(command, "pet-status"))
    desktop_ui_probe_print("pet source=%s bytes=%lu open=%d\n",
      pet_lvgl_uses_sd_sprites() ? "sd" : "fallback",
      (unsigned long)pet_lvgl_sprite_bytes(), pet_lvgl_is_open());
  else if (!strcmp(command,"music-pv-demo"))
    {
      struct music_song song={0};
      snprintf(song.name,sizeof(song.name),"慢下来的光");snprintf(song.artist,sizeof(song.artist),"原创演示");
      snprintf(song.url,sizeof(song.url),"/sdcard/music/jizura-demo.mp3");
      desktop_ui_probe_print("music-pv-demo accepted=%d\n",glass_music_play(&song)==0);
    }
  else if (!strcmp(command,"music-language-demo"))
    {
      struct music_song song={0};strcpy(song.name,"Language Demo");strcpy(song.artist,"Vela");
      strcpy(song.url,"/sdcard/music/jizura-demo.mp3");
      desktop_ui_probe_print("music-language-demo accepted=%d\n",glass_music_play(&song)==0);
    }
  else if (!strcmp(command,"music-lyrics-status"))
    {
      struct music_lyrics_window *window=malloc(sizeof(*window));
      unsigned translated=0,total=0;
      if(window){
        for(unsigned first=0;first<192;first+=MUSIC_LYRICS_WINDOW){
          glass_music_lyrics_read(first,window);total=window->total;
          for(unsigned i=0;i<window->count;i++) {
            if(window->first+i>=first&&window->translations[i][0])translated++;
          }
          if(first+MUSIC_LYRICS_WINDOW>=total)break;
        }
        free(window);
      }
      desktop_ui_probe_print("music-lyrics lines=%u translated=%u bilingual=%d\n",total,translated,!g_glass.music_original_only);
    }
  else if (!strcmp(command,"font-status"))
    {
      struct glass_font_cache_status cache;glass_lyric_font_cache_status(global_font_selected,&cache);
      desktop_ui_probe_print("FONT_CACHE face=%u glyphs=%u cached=%u preloaded=%u reads=%llu misses=%llu sizes=%u\n",
        cache.index,cache.glyph_capacity,cache.glyph_cached,cache.preloaded_bytes,
        (unsigned long long)cache.reads,(unsigned long long)cache.page_misses,cache.ready_sizes);

      int state,error;glass_sd_font_status(&state,&error);
      unsigned missing=0;
      static const uint32_t traditional[]={0x524e,0x537b,0x5f9e,0x611b,0x61f7,
        0x6703,0x68c4,0x6c92,0x7aa9,0x7e31,0x7f88,0x8207,0x88e1,0x898a,
        0x89ba,0x8ab0,0x8ad2,0x8b8a,0x8d95,0x8e64,0x9019,0x904e,0x9060,
        0x95ca,0x9727,0x98a8,0x98c4};
      for(unsigned i=0;i<sizeof(traditional)/sizeof(traditional[0]);i++) {
        lv_font_glyph_dsc_t glyph={0};
        if(!lv_font_get_glyph_dsc(zh_font(28),&glyph,traditional[i],0)||glyph.is_placeholder)missing++;
      }
      static const uint32_t languages[]={0x3042,0x3086,0x30a2,0x30f3,0x65e5,
        0xac00,0xd7a3,0xd55c,0xae00,0xc548,0xb155,0x1ebf,0x1ef9,0x03ac,0x0457};
      unsigned language_missing=0;
      for(unsigned i=0;i<sizeof(languages)/sizeof(languages[0]);i++) {
        lv_font_glyph_dsc_t glyph={0};
        if(!lv_font_get_glyph_dsc(zh_font(28),&glyph,languages[i],0)||glyph.is_placeholder)language_missing++;
      }
      desktop_ui_probe_print("font state=%d error=%d source=sd-stream unicode=%u sample=27 missing=%u language_sample=15 language_missing=%u lyric72=%d\n",
        state,error,SD_FONT_UNICODE,missing,language_missing,g_glass.music_font72!=NULL);
      static const uint32_t symbols[]={0x2705,0x274c,0x2611,0x2714,0x2715,0x274e,
        0x26a0,0xfe0f,0x1f4a1,0x1f50a,0x1f509,0x1f507,0x1f3b5,0x1f3b6,
        0x1f50d,0x1f50e,0x23f0,0x1f514,0x1f4f7,0x1f3a4,0x1f3a7,0x2728,0x1f4ac,0x3000};
      unsigned symbol_missing=0;
      const unsigned sizes[]={16,20,28};
      for(unsigned s=0;s<3;s++)for(unsigned i=0;i<sizeof(symbols)/sizeof(symbols[0]);i++){
        lv_font_glyph_dsc_t glyph={0};
        if(!lv_font_get_glyph_dsc(zh_font(sizes[s]),&glyph,symbols[i],0)||glyph.is_placeholder)symbol_missing++;
      }
      desktop_ui_probe_print("font ai_symbol_samples=72 ai_symbol_missing=%u\n",symbol_missing);
    }
  else if (!strcmp(command,"music-pv-status"))
    {
      struct music_pv_ui *p=g_music_ui.full?&g_music_ui.full->pv:NULL;
      desktop_ui_probe_print("music-pv active=%d native=1 style=%u line=%d position_ms=%u plans=%u plan_max_ms=%u updates=%u buffers=0\n",
        p&&p->visible&&p->valid,p?p->style_index:0,p?p->current:-1,p?p->last_position:0,
        p?p->plans:0,p?p->plan_max_ms:0,p?p->updates:0);
    }
  else if (!strcmp(command, "music-metadata-status"))
    {
      struct music_now_playing now;struct music_lyrics_window window;
      glass_music_now_playing(&now);glass_music_lyrics_read(0,&window);
      desktop_ui_probe_print("music-metadata lyrics=%d lines=%u artwork=%d cover_ready=%d full_cover_ready=%d\n",
        now.lyrics_state,window.total,now.cover_url[0]!=0,g_music_ui.cover.canvas!=NULL,
        g_music_ui.full&&g_music_ui.full->cover.canvas!=NULL);
    }
  else if (!strcmp(command, "music-status"))
    {
      struct music_snapshot *s = malloc(sizeof(*s));
      if (s) {
        glass_music_get(s);
        desktop_ui_probe_print("music state=%d error=%d search=%d search_error=%d result=%d favorites=%u storage=%d position_ms=%u volume=%u title=%s artist=%s\n",
          s->state,s->error,s->searching,s->search_error,s->has_result,s->favorite_count,s->storage_error,s->position_ms,s->volume,s->current.name,s->current.artist);
        free(s);
      }
    }
  else if (!strcmp(command, "music-play-result"))
    { if (g_music_ui.snapshot && g_music_ui.snapshot->has_result) glass_music_play(&g_music_ui.snapshot->result); }
  else if (!strcmp(command, "navigation-status"))
    desktop_ui_probe_print("navigation page=%s return=%s\n",
      lv_obj_has_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN)?"home":g_desktop.apps_page?"apps":g_desktop.chat_page?"chat":g_desktop.qapp_page?"qapp":"panel",
      (g_desktop.qapp_page && g_desktop.qapp_return_apps) || (g_desktop.chat_page && g_desktop.chat_return_apps)?"apps":"home");
  else if (!strcmp(command, "chat-status")) {
    struct chat_snapshot *s = calloc(1, sizeof(*s));
    if (s) {
      glass_chat_get(s);
      desktop_ui_probe_print("chat loaded=%d configured=%d phase=%d turns=%u storage=%d revision=%u last_state=%d last_error=%d editor=%d\n",
        s->loaded, s->configured, s->phase, s->count, s->storage_error, s->revision,
        s->count ? (int)s->turns[s->count - 1].state : -1,
        s->count ? (int)s->turns[s->count - 1].error : 0, g_chat_ui.editor != NULL);
      const struct chat_turn *t = s->count ? &s->turns[s->count - 1] : NULL;
      desktop_ui_probe_print("chat-stream text_bytes=%lu thinking_bytes=%lu thinking_tokens=%lu tokens_known=%d rounds=%u rebuilds=%u updates=%u refresh_max_ms=%lu draft_bytes=%u\n",
        (unsigned long)(t ? t->text_bytes : 0), (unsigned long)(t ? t->reasoning_bytes : 0),
        (unsigned long)(t ? t->reasoning_tokens : 0), t ? t->has_reasoning_tokens : 0, t ? t->stream_round : 0,
        g_chat_ui.rebuilds, g_chat_ui.live_updates, (unsigned long)g_chat_ui.refresh_max_ms, (unsigned)strlen(chat_draft));
      if(t)desktop_ui_probe_print("chat-camera turn=%lu source=%u session_only=%u sd_saved=%u sd_error=%d\n",
        (unsigned long)t->id,t->camera_samples,t->session_history,t->camera_observation[0]!=0,t->camera_save_error);
      free(s);
    }
  }
  else if (!strcmp(command, "qpk-draft-status")) {
    struct qpk_draft_info draft; glass_qpk_get(&draft);
    desktop_ui_probe_print("qpk-draft loaded=%d busy=%d revision=%lu previewed=%lu saved=%lu error=%d preview_active=%d name=%s package=%s\n",
      draft.loaded, draft.busy, (unsigned long)draft.revision, (unsigned long)draft.previewed_revision,
      (unsigned long)draft.saved_revision, draft.error, chat_qpk_preview_active, draft.name, draft.installed_package);
    desktop_ui_probe_print("qpk-save stage=%u elapsed_ms=%lu\n", draft.save_stage, (unsigned long)draft.save_elapsed_ms);
    extern volatile uint32_t g_flash_external_stack_ops;
    extern volatile uintptr_t g_flash_caller_sp, g_flash_worker_sp;
    desktop_ui_probe_print("flash-stack ops=%lu caller=%lx worker=%lx\n", (unsigned long)g_flash_external_stack_ops,
      (unsigned long)g_flash_caller_sp, (unsigned long)g_flash_worker_sp);
  }
  else if (!strcmp(command, "chat-scroll-up") || !strcmp(command, "chat-scroll-down")) {
    if (g_chat_ui.messages) {
      uint32_t began = lv_tick_get();
      lv_obj_scroll_by(g_chat_ui.messages, 0, !strcmp(command, "chat-scroll-up") ? 240 : -240, LV_ANIM_OFF);
      lv_refr_now(NULL);
      desktop_ui_probe_print("chat-scroll render_ms=%lu scroll_y=%ld\n", (unsigned long)(lv_tick_get() - began), (long)lv_obj_get_scroll_y(g_chat_ui.messages));
    }
  }
  else if (!strcmp(command, "app-back")) qapp_back(NULL);
  else if (!strcmp(command, "edge-back"))
    { g_edge_page=g_desktop.current_card; glass_edge_back_async(NULL); }
  else if (!strcmp(command, "music-toggle")) glass_music_toggle();
  else if (!strcmp(command, "music-stop")) glass_music_stop();
  else if (!strncmp(command, "music-search:", 13)) glass_music_search(command+13);
  else if (!strncmp(command, "music-key:", 10) || !strncmp(command, "chat-key:", 9))
    {
      bool chat_key = !strncmp(command, "chat-key:", 9);
      lv_obj_t *kb = chat_key ? g_chat_ui.keyboard : g_music_ui.keyboard;
      if (kb) {
        const char **map = lv_buttonmatrix_get_map(kb);
        unsigned id = 0;
        for (unsigned i=0; map[i] && map[i][0]; i++) {
          if (!strcmp(map[i],"\n")) continue;
          if (!strcmp(map[i],command+(chat_key?9:10))) {
            lv_buttonmatrix_set_selected_button(kb,id);
            lv_obj_send_event(kb,LV_EVENT_VALUE_CHANGED,NULL);
            break;
          }
          id++;
        }
      }
    }
  else if (!strncmp(command, "chat-type:", 10)) {
    if (g_chat_ui.input) lv_textarea_set_text(g_chat_ui.input, command + 10);
  }
  else if (!strcmp(command, "weather")) glass_weather_open(NULL);
  else if (strcmp(command, "audit") == 0)
    {
      lv_obj_update_layout(g_desktop.screen);
      g_ui_probe_id = 0;
      desktop_ui_probe_print("UI_LAYOUT_BEGIN\n[");
      lv_obj_t *root = g_desktop.toast ? g_desktop.toast :
                       g_desktop.dialog ? g_desktop.dialog :
                       lv_obj_has_flag(g_desktop.panel, LV_OBJ_FLAG_HIDDEN) ?
                       g_glass.home : g_desktop.current_card;
      /* Input shades cover the app below; audit the visible form itself. */
      lv_obj_t *form = desktop_ui_probe_form(root);
      if (form && root == g_desktop.current_card &&
          lv_obj_get_width(root) == 1024) root = form;
      if (root) desktop_ui_probe_tree(root, 0);
      desktop_ui_probe_print("\n]\nUI_LAYOUT_END\n");
    }
  else
    {
      if (!strcmp(command,"render-metrics-reset")) {
        glass_ui_metrics_reset();desktop_ui_probe_print("UI_RENDER_METRICS_RESET\n");
      }
      else if (!strcmp(command,"render-metrics")) {
        uint64_t now=glass_ui_metrics_now();
        desktop_ui_probe_print("UI_RENDER_METRICS elapsed_ms=%llu frames=%lu render_avg_us=%llu render_max_us=%lu refreshes=%lu refresh_avg_us=%llu refresh_max_us=%lu widgets=%u\n",
          (unsigned long long)(now>=g_ui_metrics.since?(now-g_ui_metrics.since)/1000:0),(unsigned long)g_ui_metrics.frames,
          (unsigned long long)(g_ui_metrics.frames?g_ui_metrics.render_us/g_ui_metrics.frames:0),(unsigned long)g_ui_metrics.render_max_us,
          (unsigned long)g_ui_metrics.refreshes,(unsigned long long)(g_ui_metrics.refreshes?g_ui_metrics.refresh_us/g_ui_metrics.refreshes:0),
          (unsigned long)g_ui_metrics.refresh_max_us,qpk_runtime_widget_count());
      }
      else if (!strcmp(command,"ha-status")) {
        char *status=malloc(16384);int ret=status?qpk_runtime_ha_status(status,16384):-ENOMEM;
        if(!ret)desktop_ui_probe_print("HA_STATUS %s\n",status);
        else desktop_ui_probe_print("HA_STATUS_ERROR %d\n",ret);
        free(status);
      }
      else if (!strcmp(command,"backlight-status")) {
        int result=glass_screen_brightness_status();
        desktop_ui_probe_print("BACKLIGHT_STATUS result=%d\n",result);
      }
      else if (!strncmp(command,"screen-brightness:",18)) {
        char *end;unsigned long value=strtoul(command+18,&end,10);
        int result=(*end || !command[18] || value<10 || value>100)?-EINVAL:
          glass_screen_brightness_set((unsigned)value);
        if(!result)g_screen_brightness=(int)value;
        desktop_ui_probe_print("BACKLIGHT_SET requested=%lu result=%d\n",value,result);
      }
      else if (!strncmp(command,"pointer-stroke:",15)) {
        int x,y,end_x,end_y;char extra;
        bool valid=sscanf(command+15,"%d:%d:%d:%d%c",&x,&y,&end_x,&end_y,&extra)==4;
        desktop_ui_probe_print("pointer-stroke -> %d\n",valid&&desktop_ui_probe_pointer_stroke(x,y,end_x,end_y));
      }
      else if (!strncmp(command,"pointer-tap:",12)) {
        int x,y;char extra;
        bool valid=sscanf(command+12,"%d:%d%c",&x,&y,&extra)==2;
        desktop_ui_probe_print("pointer-tap -> %d\n",valid&&desktop_ui_probe_pointer_tap(x,y));
      }
      else if (!strncmp(command,"click-at:",9)) {
        int x,y;bool hit=false;
        if(sscanf(command+9,"%d:%d",&x,&y)==2&&x>=0&&x<1024&&y>=0&&y<600)
          hit=desktop_ui_probe_click_at(g_desktop.current_card,x,y);
        desktop_ui_probe_print("click-at -> %d\n",hit);
      }
      else if (!strncmp(command, "click:", 6))
        {
          bool clicked=desktop_ui_probe_click(g_chat_ui.confirm ? g_chat_ui.confirm : g_desktop.current_card,command+6);
          if(!clicked)clicked=desktop_ui_probe_click(lv_layer_top(),command+6);
          desktop_ui_probe_print("click %s -> %d\n",command+6,clicked);
        }
      else if (!strncmp(command, "touch:", 6))
        {
          /* touch:<state>:<x>:<y> replays one pointer update with fractions
           * of the app content, which drives the ui.onTouch() path. */
          char text[sizeof(g_ui_probe_command)];
          strlcpy(text, command + 6, sizeof(text));
          char *state = text;
          char *x = strchr(text, ':');
          char *y = x ? strchr(x + 1, ':') : NULL;
          if (y == NULL)
            desktop_ui_probe_print("touch: expected state:x:y\n");
          else
            {
              *x++ = '\0';
              *y++ = '\0';
              desktop_ui_probe_print("touch %s %s %s -> %d\n", state, x, y,
                qpk_runtime_touch_test(state, strtod(x, NULL), strtod(y, NULL)));
            }
        }
      else if (!strcmp(command, "camera-start"))
        desktop_ui_probe_print("camera start clicked=%d\n",
          desktop_ui_probe_click(g_desktop.current_card, "开始预览"));
#ifdef GLASS_LOCAL_SPEECH
      else if(!strcmp(command,"speech"))glass_speech_open(NULL);
      else if(!strcmp(command,"speech-listen"))desktop_ui_probe_print("speech listen result=%d\n",glass_speech_listen());
      else if(!strcmp(command,"speech-start")){if(g_speech_ui.root)desktop_ui_probe_click(g_speech_ui.root,"开始联网语音");}
      else if(!strcmp(command,"speech-stop")){if(g_speech_ui.root)desktop_ui_probe_click(g_speech_ui.root,"结束输入");else glass_voice_stop();}
      else if(!strcmp(command,"speech-status"))desktop_ui_probe_print("VOICE_SERVICE page=%u owner=%lu recording=%u turn=%lu\n",
        g_speech_ui.root!=NULL,(unsigned long)g_speech_ui.owner,g_speech_ui.recording,(unsigned long)g_speech_ui.turn);
      else if(!strcmp(command,"speech-probe"))desktop_ui_probe_print("speech probe result=%d\n",glass_speech_probe());
      else if(!strcmp(command,"speech-nano-scalar"))desktop_ui_probe_print("speech nano result=%d\n",glass_speech_nano(0));
      else if(!strcmp(command,"speech-nano-pie"))desktop_ui_probe_print("speech nano result=%d\n",glass_speech_nano(1));
      else if(!strcmp(command,"speech-nano-play"))desktop_ui_probe_print("speech nano result=%d\n",glass_speech_nano(2));
      else if(!strcmp(command,"speech-esp-tts"))desktop_ui_probe_print("speech esp-tts result=%d\n",glass_speech_esp_tts(0));
      else if(!strcmp(command,"speech-esp-tts-play"))desktop_ui_probe_print("speech esp-tts result=%d\n",glass_speech_esp_tts(1));
      else if(!strcmp(command,"speech-esp-tts-export"))desktop_ui_probe_print("speech esp-tts export result=%d\n",glass_speech_esp_tts(2));
      else if(!strcmp(command,"speech-p4tts-scalar"))desktop_ui_probe_print("speech p4tts result=%d\n",glass_speech_p4tts(0));
      else if(!strcmp(command,"speech-p4tts-pie"))desktop_ui_probe_print("speech p4tts result=%d\n",glass_speech_p4tts(1));
      else if(!strcmp(command,"speech-p4tts-dual"))desktop_ui_probe_print("speech p4tts result=%d\n",glass_speech_p4tts(2));
      else if(!strcmp(command,"speech-zh-q8"))desktop_ui_probe_print("speech zh-q8 result=%d\n",glass_speech_zh_q8(0));
      else if(!strcmp(command,"speech-zh-q8-play"))desktop_ui_probe_print("speech zh-q8 result=%d\n",glass_speech_zh_q8(1));
      else if(!strcmp(command,"speech-zh-q8-pie"))desktop_ui_probe_print("speech zh-q8 result=%d\n",glass_speech_zh_q8(2));
      else if(!strcmp(command,"speech-zh-q8-dual"))desktop_ui_probe_print("speech zh-q8 result=%d\n",glass_speech_zh_q8(4));
      else if(!strncmp(command,"speech-say:",11))desktop_ui_probe_print("speech say result=%d\n",glass_speech_say(command+11));
#endif
      else if (!strcmp(command, "camera-mode"))
        desktop_ui_probe_print("camera mode clicked=%d\n",
          desktop_ui_probe_click(g_desktop.current_card, "*MJPEG"));
      else if (!strcmp(command, "camera-photo"))
        desktop_ui_probe_print("camera capture accepted=%d\n", qpk_runtime_camera_capture());
      else if (!strcmp(command, "espdl")) launch_builtin_espdl_qapp(NULL);
      else if (!strcmp(command, "espdl-start"))
        desktop_ui_probe_print("espdl start clicked=%d\n",desktop_ui_probe_click(g_desktop.current_card,"开始跟随"));
      else if (!strcmp(command, "espdl-stop"))
        desktop_ui_probe_print("espdl stop clicked=%d\n",desktop_ui_probe_click(g_desktop.current_card,"停止"));
      else if (!strcmp(command, "espdl-classify"))
        desktop_ui_probe_print("espdl mode clicked=%d\n",desktop_ui_probe_click(g_desktop.current_card,"物体分类"));
      else if (!strcmp(command, "espdl-photo"))
        desktop_ui_probe_print("espdl classify clicked=%d\n",desktop_ui_probe_click(g_desktop.current_card,"拍摄并分类"));
      else if (!strcmp(command, "home")) panel_hide(NULL);
      else if (!strcmp(command, "settings")) glass_settings(NULL);
      else if (!strcmp(command, "portal")) glass_portal_open(NULL);
      else if (!strcmp(command, "portal-status")) portal_debug_status();
      else if (!strcmp(command, "grok-status")) glass_grok_debug_status();
      else if (!strcmp(command,"wake-mini-status")){
        desktop_ui_probe_print("WAKE_MINI visible=%u event=%lu age_ms=%lu request=%lu capture=%u\n",
          g_wake_mini.visible,(unsigned long)g_wake_mini.event,
          (unsigned long)(g_wake_mini.visible?lv_tick_get()-g_wake_mini.shown_at:0),
          (unsigned long)g_wake_mini.request,g_wake_mini.voice_owned);
#ifdef GLASS_LOCAL_SPEECH
        struct speech_snapshot speech;glass_speech_get(&speech);
        struct glass_voice_snapshot voice;glass_voice_get(&voice);
        const char *monitor=speech.active&&speech.background_wake?"background":
          voice.active?voice.wake_observing?"shared-asr":"asr-start-or-finish":speech.active?
          speech.phase==SPEECH_WAKE||speech.phase==SPEECH_COMMAND?"local-voice":"foreground-speech":
          music_pcm_session_active()?"audio-busy":"arming";
        desktop_ui_probe_print("WAKE_MONITOR source=%s speech_request=%lu error=%d\n",monitor,(unsigned long)speech.request,speech.error);
        desktop_ui_probe_print("WAKE_EVENT retained=%lu stamp_ms=%lu now_ms=%lu local_page=%u local_accepting=%u\n",
          (unsigned long)speech.wake_event,(unsigned long)speech.wake_ms,(unsigned long)wake_mini_clock(),g_speech_ui.root!=NULL,g_speech_ui.recording);
        desktop_ui_probe_print("WAKE_PRESENTATION target=%s\n",chat_ball_is_present()?"large-ball":"mini-float");
        desktop_ui_probe_print("WAKE_IDLE elapsed_ms=%lu busy=%u timeout_ms=6000\n",
          (unsigned long)(g_wake_mini.visible?lv_tick_get()-g_wake_mini.idle_at:0),g_wake_mini.visible&&g_wake_mini.busy);
#endif
      }
      else if (!strcmp(command, "chat")) glass_chat_open(NULL);
      else if (!strcmp(command,"voice"))glass_chat_ball_open(NULL);
      else if (!strcmp(command,"voice-tap")){if(g_chat_ui.ball_mode)chat_ball_tap(NULL);}
      else if (!strcmp(command,"voice-end")){if(g_chat_ui.ball_mode)chat_ball_end(NULL);}
      else if (!strcmp(command,"voice-status")){
        desktop_ui_probe_print("VOICE_STATUS page=%u session=%u phase=%u from_wake=%u\n",
          g_chat_ui.ball_mode,g_chat_ui.voice_session,g_chat_ui.voice_phase,g_chat_ui.voice_from_wake);
        struct glass_voice_snapshot voice;glass_voice_get(&voice);
          desktop_ui_probe_print("VOICE_CAPTURE active=%u phase=%u error=%u level=%u wake_observer=%u request=%lu\n",voice.active,voice.phase,voice.error,voice.level,voice.wake_observing,(unsigned long)voice.request);
#ifdef GLASS_LOCAL_SPEECH
        struct speech_snapshot speech;glass_speech_get(&speech);
        desktop_ui_probe_print("VOICE_PLAY active=%u phase=%u error=%d backend=%s\n",speech.active,speech.phase,speech.error,glass_speech_backend());
#endif
        struct chat_snapshot *chat=calloc(1,sizeof(*chat));
        if(chat){glass_chat_get_mode(chat,g_chat_ui.mode);
          desktop_ui_probe_print("VOICE_CHAT configured=%u phase=%u turns=%u cursor=%lu\n",chat->configured,chat->phase,chat->count,(unsigned long)g_chat_ui.speech_cursor);free(chat);}
      }
      else if (!strcmp(command, "chat-input")) { if (g_chat_ui.root) chat_ui_editor_open(NULL); }
      else if (!strcmp(command, "chat-send")) { if (g_chat_ui.root) chat_ui_send(NULL); }
      else if (!strcmp(command, "chat-cancel")) glass_chat_cancel();
      else if (!strcmp(command, "chat-new")) { if (g_chat_ui.root) chat_ui_new(NULL); }
      else if (!strcmp(command, "chat-settings")) { if (g_chat_ui.root) chat_ui_config(NULL); }
      else if (!strcmp(command, "qpk-preview")) { if (g_chat_ui.root) chat_ui_preview_app(NULL); }
      else if (!strcmp(command, "qpk-save")) { if (g_chat_ui.root) chat_ui_save_app(NULL); }
      else if (!strcmp(command, "qpk-capacity")) qpk_capacity_test_start();
      else if (!strcmp(command, "qpk-yield")) qpk_yield_test_start();
      else if (!strcmp(command, "qpk-capacity-status")) desktop_ui_probe_print("qpk-resources widgets=%u error=%s\n", qpk_runtime_widget_count(), qpk_runtime_last_error());
      else if (!strcmp(command, "wifi")) glass_wifi_page(NULL);
#ifdef CONFIG_SYSTEM_C6_DESKTOP
      else if (!strcmp(command, "recovery")) glass_wifi_recovery_page(NULL);
      else if (!strcmp(command, "recovery-start") ||
               !strcmp(command, "recovery-stop"))
        {
          int ret = glass_network_recovery_init();
          if (!ret) ret = !strcmp(command, "recovery-start") ?
            glass_network_recovery_start() : glass_network_recovery_stop();
          desktop_ui_probe_print("NETWORK_RECOVERY accepted=%u result=%d\n",
                                 ret == 0, ret);
        }
      else if (!strcmp(command, "recovery-status"))
        {
          struct glass_recovery_snapshot_s state;
          int ret = glass_network_recovery_read(&state);
          if (ret < 0)
            desktop_ui_probe_print("NETWORK_RECOVERY result=%d\n", ret);
          else
            desktop_ui_probe_print("NETWORK_RECOVERY state=%u ap_running=%u dhcp_ready=%u joining=%u busy=%u associated=%u ipv4_ready=%u error=%d ap_error=%d cleanup_error=%d ap_ip=%u.%u.%u.%u sta_ip=%u.%u.%u.%u\n",
              (unsigned)state.state, state.ap_running, state.dhcp_ready,
              state.joining, state.busy, state.associated, state.ipv4_ready,
              state.error, state.ap_error, state.cleanup_error,
              state.ap_ipv4[0], state.ap_ipv4[1], state.ap_ipv4[2],
              state.ap_ipv4[3], state.ipv4[0], state.ipv4[1],
              state.ipv4[2], state.ipv4[3]);
        }
#endif
      else if (!strcmp(command, "bluetooth")) glass_bluetooth_page(NULL);
      else if (!strcmp(command, "ble-scan"))
        { glass_bluetooth_page(NULL); (void)glass_ble_scan(); }
      else if (!strcmp(command, "wifi-scan"))
        { glass_wifi_page(NULL); lv_obj_send_event(g_wifi_scan, LV_EVENT_CLICKED, NULL); }
      else if (!strcmp(command, "files")) glass_storage_page(NULL);
      else if (!strcmp(command, "resources")) glass_resources_page(NULL);
      else if (!strcmp(command, "music")) glass_music(NULL);
      else if (!strcmp(command, "apps")) glass_apps(NULL);
      else if (!strcmp(command, "about")) about_clicked(NULL);
      else if (!strcmp(command, "environment")) glass_environment(NULL);
      else if (!strcmp(command, "hello")) launch_builtin_qapp(NULL);
      else if (!strcmp(command, "toast"))
        { launch_builtin_qapp(NULL); show_toast("操作完成，中文提示清晰可读"); }
      else if (!strcmp(command, "dialog"))
        qpk_show_dialog("提示\n中文内容应清晰显示，正文与确认按钮保持独立间距。\n较长的内容可以在正文区域滚动查看。");
      else if (!strcmp(command, "2048")) launch_builtin_2048_qapp(NULL);
      else if (!strcmp(command, "ouo")) launch_builtin_ouo_qapp(NULL);
      else if (!strcmp(command, "dafeiyu") || !strcmp(command, "pet"))
        launch_builtin_dafeiyu_qapp(NULL);
      else if (!strcmp(command, "recorder")) launch_builtin_recorder_qapp(NULL);
      else if (!strcmp(command, "ha")) glass_homeassistant(NULL);
      else if (!strcmp(command, "ha-settings") || !strcmp(command, "ha-address"))
        {
          glass_portal_show(glass_homeassistant);
        }
      else if (!strcmp(command, "dropdown"))
        { glass_settings(NULL); lv_dropdown_open(g_glass.widget_dropdown); }
      else if (!strcmp(command, "files-new"))
        { glass_storage_page(NULL); g_files_operation = 1; glass_files_dialog_async(NULL); }
      else if (!strcmp(command, "light") || !strcmp(command, "dark"))
        { panel_hide(NULL); g_desktop.light_theme = !strcmp(command, "light"); apply_theme(); }
      desktop_ui_probe_print("desktop: ui %s ready\n", command);
    }
  pthread_mutex_lock(&g_ui_probe_lock);
  g_ui_probe_done = true;
  pthread_mutex_unlock(&g_ui_probe_lock);
}

static int desktop_ui_probe_request(const char *command)
{
  const char *commands[] = {"weather", "weather-status", "weather-refresh", "home", "settings", "wifi", "bluetooth", "files", "resources",
    "speech-esp-tts", "speech-esp-tts-play", "speech-esp-tts-export",
    "speech-p4tts-scalar", "speech-p4tts-pie", "speech-p4tts-dual",
    "speech-zh-q8", "speech-zh-q8-play", "speech-zh-q8-pie", "speech-zh-q8-dual",
    "audio-status", "music", "music-status", "music-metadata-status", "music-pv-demo", "music-pv-status", "font-status", "music-language-demo", "music-lyrics-status", "music-play-result", "music-toggle", "music-stop", "current", "apps", "about", "environment", "hello", "2048", "ouo", "ha", "dropdown", "grok-status",
    "files-new", "light", "dark", "audit", "frame", "frame-free", "wifi-scan", "ble-scan", "dialog",
    "recovery", "recovery-start", "recovery-stop", "recovery-status",
    "ha-settings", "ha-address", "ha-status", "render-metrics", "render-metrics-reset", "toast", "camera-start", "camera-photo", "camera-mode",
    "espdl", "espdl-start", "espdl-stop", "espdl-classify", "espdl-photo", "speech", "speech-listen", "speech-probe", "speech-nano-scalar", "speech-nano-pie", "speech-nano-play", "speech-start", "speech-stop", "speech-status",
    "voice", "voice-tap", "voice-end", "voice-status", "wake-mini-status",
    "navigation-status", "app-back", "edge-back", "portal", "portal-status",
    "chat", "chat-input", "chat-send", "chat-cancel", "chat-new", "chat-settings", "chat-status",
    "qpk-draft-status", "qpk-preview", "qpk-save", "qpk-capacity", "qpk-capacity-status", "chat-scroll-up", "chat-scroll-down", "recorder", "dafeiyu", "pet", "pet-status"};
  /* click:<label> presses one visible control by its label, which is how the
   * file manager's list rows are verified. */
  bool found = (!strcmp(command,"backlight-status")) ||
               (!strncmp(command,"screen-brightness:",18)) ||
#ifdef GLASS_LOCAL_SPEECH
               (!strncmp(command,"speech-say:",11)&&command[11]!='\0') ||
#endif
#ifdef CONFIG_SYSTEM_BILIBILI_LIVE
               (!strcmp(command, "local-video")) || (!strcmp(command, "local-stop")) ||
               (!strcmp(command, "local-scale-on")) || (!strcmp(command, "local-scale-off")) ||
               (!strcmp(command,"video-layer-top")) || (!strcmp(command,"video-layer-screen")) || (!strcmp(command,"video-geometry")) ||
               (!strncmp(command,"audio-volume:",13)) ||
               (!strcmp(command, "video-blit-on")) || (!strcmp(command, "video-blit-off")) ||
               (!strcmp(command, "video-profile-reset")) || (!strcmp(command, "video-profile")) ||
               (!strncmp(command, "local-play:", 11) && !strncmp(command + 11, "/sdcard/", 8)) ||
               (!strcmp(command, "bilibili")) || (!strcmp(command, "bilibili-stop")) ||
               (!strncmp(command, "bilibili-play:", 14) && bili_valid_room(command + 14)) ||
               (!strncmp(command,"huya-play:",10) && bili_valid_room(command+10)) ||
               (!strncmp(command,"cc-play:",8) && bili_valid_room(command+8)) ||
#endif
               (!strncmp(command, "click:", 6) && command[6] != '\0') ||
               (!strncmp(command,"click-at:",9) && command[9]!='\0') ||
               (!strncmp(command,"pointer-stroke:",15) && command[15]!='\0') ||
               (!strncmp(command,"pointer-tap:",12) && command[12]!='\0') ||
               (!strncmp(command, "music-key:", 10) && command[10] != '\0') ||
               (!strncmp(command, "chat-key:", 9) && command[9] != '\0') ||
               (!strncmp(command, "chat-type:", 10) && command[10] != '\0') ||
               (!strncmp(command, "music-search:", 13) && command[13] != '\0') ||
               (!strncmp(command, "touch:", 6) && command[6] != '\0');
  for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
    if (!strcmp(command, commands[i])) found = true;
  if (!found || !lv_is_initialized())
    { puts("desktop: unknown UI page or desktop not running"); return 1; }
  pthread_mutex_lock(&g_ui_probe_lock);
  if (g_ui_probe_pending)
    { pthread_mutex_unlock(&g_ui_probe_lock); puts("desktop: UI request busy"); return 1; }
  if (!g_ui_probe_report) g_ui_probe_report = malloc(UI_PROBE_REPORT_BYTES);
  if (!g_ui_probe_report)
    { pthread_mutex_unlock(&g_ui_probe_lock); puts("desktop: UI report allocation failed"); return 1; }
  strlcpy(g_ui_probe_command, command, sizeof(g_ui_probe_command));
  g_ui_probe_pending = true;
  g_ui_probe_done = false;
  pthread_mutex_unlock(&g_ui_probe_lock);
  for (unsigned i = 0; i < 250; i++)
    {
      usleep(20000);
      pthread_mutex_lock(&g_ui_probe_lock);
      if (g_ui_probe_done)
        {
          char *report = g_ui_probe_report;
          g_ui_probe_report = NULL;
          g_ui_probe_pending = false;
          pthread_mutex_unlock(&g_ui_probe_lock);
          if (!report) return 1;
          fputs(report, stdout);
          free(report);
          return 0;
        }
      pthread_mutex_unlock(&g_ui_probe_lock);
    }
  puts("desktop: UI request timed out");
  return 1;
}
