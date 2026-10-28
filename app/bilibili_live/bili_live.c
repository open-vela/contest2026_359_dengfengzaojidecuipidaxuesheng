/* SPDX-License-Identifier: Apache-2.0 */
#include "bili_live.h"
#include "glass_input_guard.h"
#include "glass_icons.h"
#include "glass_app_controls.h"
#include "bili_http.h"
#include "bili_api.h"
#include "live_platform.h"
#include "bili_flv.h"
#include "bili_decoder.h"
#include <errno.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __NuttX__
#include <malloc.h>
#endif
#include "bili_pixels.h"
#include "bili_av.h"
#include "bili_ppa.h"
#include "bili_perf.h"
#include "glass_music.h"
#include "glass_music_pcm.h"
#define WIDTH 640
#define HEIGHT 360
#define PIXELS (WIDTH * HEIGHT)
#define CAPACITY_PIXELS (1024u*600u)
struct session {
  pthread_t thread;
  pthread_mutex_t lock;
  atomic_bool cancel, running, scale;
  atomic_uint displayed;
  unsigned pending_width,pending_height;
  struct bili_pixels_layout layout;
  struct bili_av *av;
  struct music_pcm pcm;
  unsigned packets;
  bool joined, dirty;
  unsigned frames;
  unsigned overwritten;
  struct bili_av_stats latest_stats;
  struct bili_perf_window perf;
  int platform;
  uint64_t started_us, decode_us, convert_us, read_us;
  char room[16], cookie[2048], status[160];
  uint16_t *pending, *shown, *back;
  struct bili_ppa ppa;
  struct bili_http *http;
  struct bili_streams streams;
};
static struct session *g_session;
static lv_obj_t *g_page, *g_room, *g_status, *g_image, *g_platform;
static lv_obj_t *g_scale_switch,*g_viewport,*g_fullscreen,*g_metrics;
static const lv_font_t *g_font;
static bool g_scale=true;
static uint32_t g_status_updated;
static int g_requested_platform;
static lv_timer_t *g_timer;
static lv_image_dsc_t g_dsc;
static char g_requested_room[16];
static void live_picture_layout(void)
{
  if(!g_image)return;
  lv_obj_set_size(g_image,g_dsc.header.w?g_dsc.header.w:WIDTH,g_dsc.header.h?g_dsc.header.h:HEIGHT);
  lv_image_set_scale(g_image,LV_SCALE_NONE);
  lv_obj_center(g_image);
}
static void live_fullscreen_leave(void)
{
  if(!g_fullscreen)return;
  lv_obj_t *old=g_fullscreen;g_fullscreen=NULL;
  if(g_image&&g_viewport){lv_obj_set_parent(g_image,g_viewport);live_picture_layout();}
  if(g_metrics&&g_page){lv_obj_set_parent(g_metrics,g_page);lv_obj_set_pos(g_metrics,10,84);lv_obj_set_style_text_color(g_metrics,lv_color_black(),0);lv_obj_set_style_bg_opa(g_metrics,LV_OPA_TRANSP,0);}
  lv_obj_delete(old);
}
static void live_mode_set(bool scale);
static void live_fullscreen_exit(lv_event_t *e){(void)e;live_mode_set(true);}
static void live_fullscreen_enter(void)
{
  if(g_fullscreen||!g_image)return;
  lv_obj_t *top=lv_layer_top();g_fullscreen=lv_obj_create(top);
  lv_obj_set_pos(g_fullscreen,0,0);lv_obj_set_size(g_fullscreen,1024,600);
  lv_obj_set_style_pad_all(g_fullscreen,0,0);lv_obj_set_style_border_width(g_fullscreen,0,0);
  lv_obj_set_style_radius(g_fullscreen,0,0);lv_obj_set_style_bg_color(g_fullscreen,lv_color_black(),0);
  lv_obj_set_style_bg_opa(g_fullscreen,LV_OPA_COVER,0);lv_obj_remove_flag(g_fullscreen,LV_OBJ_FLAG_SCROLLABLE);
  if(g_font)lv_obj_set_style_text_font(g_fullscreen,g_font,0);
  lv_obj_set_parent(g_image,g_fullscreen);lv_obj_update_layout(g_fullscreen);live_picture_layout();
  if(g_metrics){lv_obj_set_parent(g_metrics,g_fullscreen);lv_obj_align(g_metrics,LV_ALIGN_BOTTOM_LEFT,12,-12);lv_obj_set_style_text_color(g_metrics,lv_color_white(),0);lv_obj_set_style_bg_color(g_metrics,lv_color_black(),0);lv_obj_set_style_bg_opa(g_metrics,LV_OPA_70,0);}
  lv_obj_t *exit=lv_button_create(g_fullscreen);lv_obj_set_size(exit,120,40);lv_obj_align(exit,LV_ALIGN_TOP_RIGHT,-12,12);
  lv_obj_t *text=lv_label_create(exit);lv_label_set_text(text,"退出全屏");lv_obj_center(text);
  lv_obj_add_event_cb(exit,live_fullscreen_exit,LV_EVENT_CLICKED,NULL);
}
static void live_mode_set(bool scale)
{
  g_scale=scale;
  if(g_scale_switch){if(scale)lv_obj_add_state(g_scale_switch,LV_STATE_CHECKED);else lv_obj_remove_state(g_scale_switch,LV_STATE_CHECKED);}
  if(g_session)atomic_store(&g_session->scale,scale);
  if(scale)live_fullscreen_leave();else live_fullscreen_enter();
}
static void live_scale_changed(lv_event_t *e)
{live_mode_set(lv_obj_has_state(lv_event_get_target(e),LV_STATE_CHECKED));}

static void status(struct session *s, const char *text, int code)
{
  pthread_mutex_lock(&s->lock);
  if (code) snprintf(s->status, sizeof(s->status), "%s (%d)", text, code);
  else snprintf(s->status, sizeof(s->status), "%s", text);
  pthread_mutex_unlock(&s->lock);
  printf("bilibili: %s\n", s->status);
}
static uint64_t microseconds(void)
{
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
static int frame(void *ctx,const uint8_t *const p[3],const int stride[2],int w,int h)
{
  struct session *s=ctx;uint64_t begin=microseconds();unsigned outw,outh;
  int r=-ENOTSUP;
  if(atomic_load(&s->scale)&&!atomic_load(&s->cancel)){
    r=bili_ppa_frame(&s->ppa,s->back,p,stride,w,h,WIDTH,HEIGHT);
    if(!r){outw=WIDTH;outh=HEIGHT;}
  }
  if(r)r=bili_pixels_convert(s->back,p,stride,w,h,atomic_load(&s->scale),&s->layout,&s->cancel,&outw,&outh);
  if(r)return r;
  uint64_t converted=microseconds()-begin;
  /* Keep YUV conversion outside the UI lock. Publish only complete frames. */
  pthread_mutex_lock(&s->lock);
  s->convert_us+=converted;
  if(s->dirty)s->overwritten++;
  uint16_t *old = s->pending;
  s->pending = s->back; s->back = old;
  s->pending_width=outw;s->pending_height=outh;
  s->dirty = true; s->frames++;
  if (s->frames == 1 || s->frames % 30 == 0)
    printf("bilibili: decoded %u frames %dx%d\n", s->frames, w, h);
  struct bili_av_stats stats;bili_av_stats(s->av,&stats);
  s->latest_stats=stats;
  snprintf(s->status, sizeof(s->status), "%s %s · %dx%d → %ux%u · %s · %u 帧 · %s",
           live_platform_name(s->platform), s->room, w, h,outw,outh, s->streams.quality, s->frames,stats.audio_frames?"AAC 音频":"等待音频");
  pthread_mutex_unlock(&s->lock);
  return 0;
}
static int frame_timed(void *ctx,const uint8_t *const p[3],const int stride[2],int w,int h,uint64_t pts)
{
  struct session *s=ctx;(void)pts;
  if(atomic_load(&s->cancel))return -ECANCELED;
  return frame(ctx,p,stride,w,h);
}
static int audio_open(void *ctx,unsigned rate){return music_pcm_try_open(&((struct session*)ctx)->pcm,rate,40);}
static int audio_write(void *ctx,const int16_t *p,unsigned n,unsigned channels){
  int r=music_pcm_write(&((struct session*)ctx)->pcm,p,n,channels);
  if(r)printf("bilibili: PCM write error=%d\n",r);
  return r;
}
static unsigned audio_time(void *ctx){struct music_pcm *p=&((struct session*)ctx)->pcm;return p->rate?(uint64_t)p->played_samples*1000/p->rate:0;}
static void audio_close(void *ctx){music_pcm_close(&((struct session*)ctx)->pcm);}
static const struct bili_av_ops av_ops={frame_timed,audio_open,audio_write,audio_time,audio_close};
static int read_stream(void *ctx,uint8_t *p,size_t n)
{
  struct session *s=ctx;int error=bili_av_error(s->av);
  if(error){printf("bilibili: AV worker error=%d\n",error);return error;}
  uint64_t begin=microseconds();int r=bili_http_read(s->http,p,n);s->read_us+=microseconds()-begin;
  if(r<=0)printf("bilibili: HTTP body read=%d requested=%u\n",r,(unsigned)n);
  return r;
}
static int audio(void *ctx,const uint8_t *p,size_t n,uint32_t ts)
{return bili_av_audio(((struct session*)ctx)->av,p,n,ts);}
static int video(void *ctx,const uint8_t *p,size_t n,uint32_t ts)
{
  struct session *s=ctx;if(atomic_load(&s->cancel))return -ECANCELED;
  int r=bili_av_video(s->av,p,n,ts);if(!s->started_us)s->started_us=microseconds();
  struct bili_av_stats stats;bili_av_stats(s->av,&stats);
  pthread_mutex_lock(&s->lock);if(stats.decoded>=s->latest_stats.decoded)s->latest_stats=stats;pthread_mutex_unlock(&s->lock);
  if(++s->packets%30==0){
    uint64_t wall=microseconds()-s->started_us;
    pthread_mutex_lock(&s->lock);uint64_t convert=s->convert_us;pthread_mutex_unlock(&s->lock);
    printf("bilibili: perf frames=%u wall_ms=%lu fps_x100=%lu decode_ms=%lu convert_ms=%lu read_ms=%lu display=%u mode=%s decode_skip=%u render_skip=%u resync=%u lag=%u audio=%u audio_skip=%u threads=1 aq=%u aq_peak=%u aq_bytes_peak=%u aq_ms=%u audio_live_ms=%u video_live_ms=%u\n",
      stats.decoded,(unsigned long)(wall/1000),(unsigned long)(stats.decoded*100000000ULL/(wall?wall:1)),
      (unsigned long)((stats.decode_us>convert?stats.decode_us-convert:0)/1000),(unsigned long)(convert/1000),(unsigned long)(s->read_us/1000),
      atomic_load(&s->displayed),atomic_load(&s->scale)?"fit":"native",stats.decode_skip,stats.render_skip,stats.resync,stats.lag,stats.audio_frames,stats.audio_skip,stats.audio_depth,stats.audio_peak,stats.audio_bytes_peak,stats.audio_queue_ms,stats.audio_live_ms,stats.video_live_ms);
  }
  return r;
}
static void cookie_load(struct session *s)
{
  FILE *fp = fopen("/data/bili_cookie.txt", "r");
  size_t n;
  if (!fp) return;
  n = fread(s->cookie, 1, sizeof(s->cookie) - 1, fp);
  /* One line only; never print credentials or signed media URLs. */
  if (fgetc(fp) != EOF) s->cookie[0] = 0;
  else {
    s->cookie[n] = 0;
    s->cookie[strcspn(s->cookie, "\r\n")] = 0;
  }
  fclose(fp);
}
static void *worker(void *ctx)
{
  struct session *s = ctx;
  struct bili_flv f = {0};
  int r = 0, i;
  if(s->platform==LIVE_BILI) cookie_load(s);
  status(s, "正在获取最低码率 H.264 / FLV 线路…", 0);
  r=live_resolve(s->platform,s->room,s->cookie,&s->cancel,&s->streams);
  if(r){status(s,r==-ENODATA?"直播间未开播或房间不存在":r==-ENOTSUP?"当前房间没有可用 H.264 / FLV 线路":"直播线路请求失败",r);goto done;}
  r=glass_music_stop_wait(2000);if(r){status(s,"无法释放音乐音频设备",r);goto done;}
  f.read = read_stream; f.video = video; f.audio=audio; f.opaque = s;
  f.managed_buffers=true; /* Allocate only the compressed tag/AU actually received. */
  /* Bounded CDN failover. Stop/exit always wins; no automatic restart loop. */
  for (i = 0; i < s->streams.count && i < 3 && !atomic_load(&s->cancel); i++) {
    s->av=bili_av_create(&av_ops,s,&s->cancel);
    s->packets=0;s->started_us=s->read_us=0;
    atomic_store(&s->displayed,0);
    pthread_mutex_lock(&s->lock);s->convert_us=0;s->frames=s->overwritten=0;memset(&s->latest_stats,0,sizeof(s->latest_stats));pthread_mutex_unlock(&s->lock);
    if(!s->av){r=-ENOMEM;break;}
    status(s, "正在连接视频流…", 0);
    s->http = bili_http_open_site(s->streams.urls[i], s->cookie, s->streams.referer, &s->cancel, &r);
#ifdef __NuttX__
    struct mallinfo memory=mallinfo();
    printf("bilibili: RAM before stream free=%lu largest=%lu threads=1\n",(unsigned long)memory.fordblks,(unsigned long)memory.mxordblk);
#endif
    f.configured = 0;
    if (s->http) r = bili_flv_run(&f);
    bili_http_close(s->http); s->http = NULL;
    if(!r&&bili_av_error(s->av))r=bili_av_error(s->av);
    bili_av_destroy(s->av);s->av=NULL;
    if (!r || r == -ECANCELED || r == -EFBIG || r == -ENOMEM) break;
    status(s, "当前线路结束，尝试下一线路…", r);
  }
  if (atomic_load(&s->cancel)) status(s, "已停止", 0);
  else if (r == -EFBIG) status(s, "视频超过 1280×720 或码流缓冲上限", r);
  else status(s, r ? "视频播放失败" : "直播流已结束", r);
done:
  free(f.tag); free(f.annexb);
  memset(s->cookie, 0, sizeof(s->cookie));
  memset(&s->streams, 0, sizeof(s->streams));
  atomic_store(&s->running, false);
  return NULL;
}
static void reap(void)
{
  struct session *s = g_session;
  if (!s || atomic_load(&s->running)) return;
  if (!s->joined) { pthread_join(s->thread, NULL); s->joined = true; }
}
static void release(void)
{
  struct session *s = g_session;
  if (!s) return;
  /* LVGL must release its image pointer before the pixel buffer is freed. */
  if (g_image) lv_image_set_src(g_image, NULL);
  lv_image_cache_drop(&g_dsc);
  bili_ppa_close(&s->ppa);
  free(s->pending); free(s->shown); free(s->back);
  pthread_mutex_destroy(&s->lock); free(s); g_session = NULL;
}
static void tick(lv_timer_t *timer)
{
  struct session *s = g_session;
  if (s) {
    bool dirty;unsigned picture_w=0,picture_h=0;
    char text[160];struct bili_av_stats stats;unsigned submitted,overwritten;uint64_t convert;
    pthread_mutex_lock(&s->lock);
    dirty = s->dirty;
    if (dirty && g_image) {
      picture_w=s->pending_width;picture_h=s->pending_height;
      uint16_t *old = s->shown; s->shown = s->pending; s->pending = old;
    }
    s->dirty = false;
    snprintf(text, sizeof(text), "%s", s->status);
    stats=s->latest_stats;submitted=s->frames;overwritten=s->overwritten;convert=s->convert_us;
    pthread_mutex_unlock(&s->lock);
    if (g_requested_room[0])
      snprintf(text, sizeof(text), "正在切换到房间 %s…", g_requested_room);
    if(g_status&&(g_requested_room[0]||lv_tick_elaps(g_status_updated)>=250)){g_status_updated=lv_tick_get();if(strcmp(lv_label_get_text(g_status),text))lv_label_set_text(g_status,text);}
    if (dirty && g_image) {
      lv_image_cache_drop(&g_dsc);
      g_dsc.data = (const uint8_t *)s->shown;
      g_dsc.header.w=picture_w;g_dsc.header.h=picture_h;g_dsc.header.stride=picture_w*2;g_dsc.data_size=picture_w*picture_h*2;
      lv_image_set_src(g_image,&g_dsc);live_picture_layout();atomic_fetch_add(&s->displayed,1);
      lv_obj_invalidate(g_image);
    }
    if(g_metrics&&atomic_load(&s->running)&&bili_perf_update(&s->perf,microseconds()/1000,
        stats.decoded,submitted,atomic_load(&s->displayed),stats.decoder_us,convert)){
      struct bili_perf_window *p=&s->perf;char metric[256];
      snprintf(metric,sizeof(metric),"解码 %u.%02u / 画面 %u.%02u FPS · 耗时 %u+%u ms · 音画 %u ms · 缓存 %u ms · 视频 %u ms",
        p->decode_fps_x100/100,p->decode_fps_x100%100,p->display_fps_x100/100,p->display_fps_x100%100,
        p->decode_ms,p->convert_ms,stats.lag,stats.audio_queue_ms,stats.video_live_ms);
      lv_label_set_text(g_metrics,metric);
      printf("[video-fps] kind=live decode_x100=%u lvgl_x100=%u pure_decode_ms=%u convert_ms=%u decoded=%u submitted=%u lvgl=%u render_skip=%u overwritten=%u lag=%u\n",
        p->decode_fps_x100,p->display_fps_x100,p->decode_ms,p->convert_ms,stats.decoded,submitted,
        atomic_load(&s->displayed),stats.render_skip,overwritten,stats.lag);
    }
    reap();
    if (s->joined && g_page && g_requested_room[0]) {
      char next[sizeof(g_requested_room)];
      memcpy(next, g_requested_room, sizeof(next));
      g_requested_room[0] = 0;
      bili_live_play_platform(g_requested_platform,next);
      s = g_session;
    }
  }
  if (!g_page && (!s || s->joined)) {
    release(); lv_timer_delete(timer); g_timer = NULL;
  }
}
void bili_live_stop(void)
{
  g_requested_room[0] = 0;
  if (g_session) atomic_store(&g_session->cancel, true);
  live_fullscreen_leave();
  g_page = g_room = g_status = g_image = g_platform = g_scale_switch = g_viewport = NULL;
  g_metrics=NULL;
}
static void deleted(lv_event_t *e)
{
  if (lv_event_get_target(e) == g_page) bili_live_stop();
}
static void stop_clicked(lv_event_t *e)
{
  (void)e;
  g_requested_room[0] = 0;
  if (g_session) atomic_store(&g_session->cancel, true);
  if (g_status) lv_label_set_text(g_status, "正在停止…");
}
static void play_clicked(lv_event_t *e)
{
  struct session *s;
  pthread_attr_t attr;
  int r;
  (void)e;
  reap();
  if (!g_room || !g_status) return;
  if (!bili_valid_room(lv_textarea_get_text(g_room))) {
    lv_label_set_text(g_status, "请输入有效的数字直播间号"); return;
  }
  if (g_session && !g_session->joined) {
    snprintf(g_requested_room, sizeof(g_requested_room), "%s", lv_textarea_get_text(g_room));
    g_requested_platform=lv_dropdown_get_selected(g_platform);
    atomic_store(&g_session->cancel, true);
    lv_label_set_text(g_status, "正在停止旧直播并切换房间…");
    return;
  }
  g_requested_room[0] = 0;
  release();
  s = calloc(1, sizeof(*s));
  if (!s) goto oom;
  s->pcm.fd=-1;s->pcm.mq=(mqd_t)-1;
  s->pending = bili_ppa_pixels(CAPACITY_PIXELS); s->shown = bili_ppa_pixels(CAPACITY_PIXELS);
  s->back = bili_ppa_pixels(CAPACITY_PIXELS);
  if (!s->pending || !s->shown || !s->back) {
    free(s->pending); free(s->shown); free(s->back); free(s); goto oom;
  }
  pthread_mutex_init(&s->lock, NULL);
  atomic_init(&s->cancel, false); atomic_init(&s->running, true);atomic_init(&s->scale,g_scale);atomic_init(&s->displayed,0);
  s->platform=lv_dropdown_get_selected(g_platform);
  snprintf(s->room, sizeof(s->room), "%s", lv_textarea_get_text(g_room));
  snprintf(s->status, sizeof(s->status), "正在启动…");
  g_session = s;
  memset(&g_dsc, 0, sizeof(g_dsc));
  g_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  g_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
  g_dsc.header.w = WIDTH; g_dsc.header.h = HEIGHT;
  g_dsc.header.stride = WIDTH * 2;
  g_dsc.data_size = PIXELS * 2; g_dsc.data = (uint8_t *)s->shown;
  lv_image_set_src(g_image, &g_dsc);
  pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 65536);
  r = pthread_create(&s->thread, &attr, worker, s);
  pthread_attr_destroy(&attr);
  if (r) {
    atomic_store(&s->running, false); s->joined = true; release();
    lv_label_set_text(g_status, "无法创建播放任务");
  }
  return;
oom:
  lv_label_set_text(g_status, "内存不足，无法启动播放器");
}
void bili_live_play_room(const char *room)
{
  if (!g_room || !bili_valid_room(room)) return;
  bili_live_play_platform(LIVE_BILI,room);
}
void bili_live_play_platform(int platform,const char *room)
{
  if(!g_room || !g_platform || !bili_valid_room(room) || platform<0 || platform>=LIVE_PLATFORM_COUNT)return;
  lv_dropdown_set_selected(g_platform,platform);lv_textarea_set_text(g_room,room);play_clicked(NULL);
}
static void button(lv_obj_t *parent, const char *text, int x,
                    lv_event_cb_t cb)
{
  lv_obj_t *b = lv_button_create(parent), *label;
  lv_obj_set_pos(b, x, 12); lv_obj_set_size(b, 100, 44);
  struct glass_app_palette palette=glass_app_palette(lv_color_brightness(lv_obj_get_style_bg_color(parent,0))>128);
  bool primary=!strcmp(text,"播放");
  glass_app_control_style(b,primary?palette.accent:palette.soft,palette.line);
  label = lv_label_create(b); lv_label_set_text(label, text); lv_obj_center(label);
  lv_obj_set_style_text_color(label,lv_color_hex(primary?palette.on_accent:palette.ink),0);
  glass_input_guard_install(b);lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
}
void bili_live_show(lv_obj_t *parent, const lv_font_t *font)
{
  lv_obj_t *keyboard, *viewport;
  bili_live_stop(); reap();
  if (g_session && g_session->joined) release();
  g_page = parent;g_font=font;g_scale=true;
  struct glass_app_palette palette=glass_app_palette(lv_color_brightness(lv_obj_get_style_bg_color(parent,0))>128);
  lv_obj_set_style_bg_color(parent,lv_color_hex(palette.canvas),0);
  lv_obj_set_style_text_color(parent,lv_color_hex(palette.ink),0);
  lv_obj_add_event_cb(parent, deleted, LV_EVENT_DELETE, NULL);
  if (font) lv_obj_set_style_text_font(parent, font, 0);
  lv_obj_set_style_pad_all(parent, 0, 0);
  lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
  g_room = lv_textarea_create(parent);
  glass_app_input_style(g_room,palette);
  lv_obj_set_pos(g_room, 10, 12); lv_obj_set_size(g_room, 260, 44);
  lv_textarea_set_one_line(g_room, true);
  lv_textarea_set_max_length(g_room, 10);
  lv_textarea_set_accepted_chars(g_room, "0123456789");
  lv_textarea_set_placeholder_text(g_room, "数字直播间号");
  g_platform=lv_dropdown_create(parent);
  glass_app_input_style(g_platform,palette);
  lv_dropdown_set_options(g_platform,"B站\n虎牙\n网易CC");
  lv_dropdown_set_symbol(g_platform,"v");
  if(font) lv_obj_set_style_text_font(lv_dropdown_get_list(g_platform),font,0);
  glass_app_panel_style(lv_dropdown_get_list(g_platform),palette.paper,palette.line);
  lv_obj_set_style_text_color(lv_dropdown_get_list(g_platform),lv_color_hex(palette.ink),0);
  lv_obj_set_pos(g_platform,530,12);lv_obj_set_size(g_platform,160,44);
  button(parent, "播放", 290, play_clicked);
  button(parent, "停止", 410, stop_clicked);
  g_status = lv_label_create(parent);
  lv_obj_set_pos(g_status, 10, 62); lv_obj_set_width(g_status, 680);
  lv_label_set_text(g_status, "直接连接直播平台 · 自动请求最低码率 · 单线程追帧 · AAC 音频");
  g_metrics=lv_label_create(parent);lv_obj_set_pos(g_metrics,10,84);lv_obj_set_width(g_metrics,1000);
  lv_label_set_long_mode(g_metrics,LV_LABEL_LONG_DOT);lv_label_set_text(g_metrics,"解码 FPS / 画面 FPS · 每 2 秒采样，画面统计为 LVGL 提交");
  g_scale_switch=lv_checkbox_create(parent);lv_checkbox_set_text(g_scale_switch,"\u7f29\u653e\u753b\u9762");
  lv_obj_set_pos(g_scale_switch,710,22);lv_obj_add_state(g_scale_switch,LV_STATE_CHECKED);
  lv_obj_add_event_cb(g_scale_switch,live_scale_changed,LV_EVENT_VALUE_CHANGED,NULL);
  lv_obj_t *mode_note=lv_label_create(parent);lv_label_set_text(mode_note,"\u5173\u95ed\uff1a\u539f\u5c3a\u5bf8\u5168\u5c4f\uff0c\u8d85\u51fa\u5c4f\u5e55\u88c1\u5207");lv_obj_set_pos(mode_note,710,62);lv_obj_set_width(mode_note,300);
  viewport = lv_obj_create(parent);g_viewport=viewport;
  lv_obj_set_pos(viewport, 10, 116); lv_obj_set_size(viewport, WIDTH, HEIGHT);
  lv_obj_set_style_pad_all(viewport, 0, 0);
  lv_obj_set_style_border_width(viewport, 0, 0);
  lv_obj_set_style_bg_color(viewport, lv_color_black(), 0);
  lv_obj_remove_flag(viewport, LV_OBJ_FLAG_SCROLLABLE);
  g_image = lv_image_create(viewport);
  lv_obj_set_pos(g_image, 0, 0); lv_obj_set_size(g_image, WIDTH, HEIGHT);
  keyboard = lv_keyboard_create(parent);
  glass_app_panel_style(keyboard,palette.paper,palette.line);
  lv_obj_set_style_bg_color(keyboard,lv_color_hex(palette.soft),LV_PART_ITEMS);
  lv_obj_set_style_text_color(keyboard,lv_color_hex(palette.ink),LV_PART_ITEMS);
  lv_obj_set_style_radius(keyboard,GLASS_APP_CONTROL_RADIUS,LV_PART_ITEMS);
  lv_obj_set_style_bg_color(keyboard,lv_color_hex(palette.accent),LV_PART_ITEMS|LV_STATE_CHECKED);
  lv_obj_set_style_text_color(keyboard,lv_color_hex(palette.on_accent),LV_PART_ITEMS|LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(keyboard,lv_color_hex(palette.accent),LV_PART_ITEMS|LV_STATE_PRESSED);
  lv_obj_set_style_text_color(keyboard,lv_color_hex(palette.on_accent),LV_PART_ITEMS|LV_STATE_PRESSED);
  lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_NUMBER);
  lv_obj_set_style_text_font(keyboard, glass_icon_font(14), LV_PART_ITEMS);
  lv_obj_set_size(keyboard, 270, 270);
  lv_obj_align(keyboard, LV_ALIGN_TOP_LEFT, 670, 110);
  lv_keyboard_set_textarea(keyboard, g_room);
  if (!g_timer) g_timer = lv_timer_create(tick, 33, NULL);
}
