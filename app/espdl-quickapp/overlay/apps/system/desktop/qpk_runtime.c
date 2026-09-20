/****************************************************************************
 * apps/system/desktop/qpk_runtime.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <malloc.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/cache.h>
#include <nuttx/video/fb.h>
#include <nuttx/video/video.h>
#include <nuttx/video/uvc_camera.h>
#include "qpk_mjpeg.h"
#include "qpk_espdl.h"
#include <lvgl/lvgl.h>
#include <quickjs.h>

#include "qpk_runtime.h"
#include "qpk_security.h"
#include "qpk_error_text.h"
#include "glass_ime.h"
#include "qpk_storage.h"
#include "qpk_limits.h"
#include "glass_portal.h"
#include "qpk_homeassistant.h"
#ifdef CONFIG_SYSTEM_HASS
#include "hass_qjs.h"
#include "hass_service.h"
#include "hass_ui_auth.h"
#endif
#include "qpk_pet.h"
#include "glass_dashboard.h"

#define QPK_STACK_LIMIT   (16 * 1024)
#define QPK_EVAL_BUDGET   250
/* A touch, swipe or button callback runs while the finger is waiting, so it
 * has to stay short. A timer callback is deferred background work: the app has
 * already drawn whatever it wants the user to see, and a game or a solver
 * legitimately needs longer than an interactive response. Both are wall-clock
 * ceilings that stop a runaway script from freezing the UI thread.
 */
#define QPK_EVENT_BUDGET  80
#define QPK_TIMER_BUDGET  250
/* One frame is about 16 ms; a yield only has to let LVGL run once. */
#define QPK_YIELD_PERIOD_MS 1
#define QPK_DIR CONFIG_SYSTEM_DESKTOP_QPK_DIR
#define QPK_STORAGE_ROOT QPK_DIR "/.data"
#define QPK_CAMERA_DEVICE "/dev/video0"
#define QPK_CAMERA_RAW_WIDTH 1024
#define QPK_CAMERA_RAW_HEIGHT 600
#define QPK_CAMERA_BAR_TOP 500
#define QPK_CAMERA_VIEW_WIDTH 512
#define QPK_CAMERA_VIEW_HEIGHT 300
#define QPK_CAMERA_FRAME_SIZE \
  (QPK_CAMERA_RAW_WIDTH * QPK_CAMERA_RAW_HEIGHT * sizeof(uint16_t))
#define QPK_CAMERA_BUFFER_COUNT 3
#define QPK_CAMERA_DISPLAY_BUFFER_COUNT 2
#define QPK_CAMERA_FRAMEBUFFER_COUNT 3
#define QPK_CAMERA_PREVIEW_PERIOD_MS 20
#define QPK_CAMERA_PROFILE_FRAMES 60
#define QPK_CAMERA_HEALTH_FRAMES 300
#define QPK_CAMERA_MAX_DQ_ERRORS 8
#define QPK_CAMERA_MAX_PAN_ERRORS 3
#define QPK_CAMERA_CLEANUP_STACK 8192
#define QPK_CAMERA_CLEANUP_RETRIES 3
#define QPK_CAMERA_RETRY_MS 250

#ifdef CONFIG_ESPRESSIF_MIPI_DSI
FAR void *esp_mipi_dsi_noncache_addr(FAR void *addr);
#endif

enum qpk_widget_type_e
{
  QPK_WIDGET_LABEL = 0,
  QPK_WIDGET_NUMBER,
  QPK_WIDGET_PANEL,
  QPK_WIDGET_BUTTON,
  QPK_WIDGET_RECT,
  QPK_WIDGET_ARC,
  QPK_WIDGET_LINE,
};

struct qpk_event_s
{
  struct qpk_event_s *next;
  JSValue function;
  lv_obj_t *owner;
  bool used;
};

struct qpk_timer_s
{
  struct qpk_timer_s *next;
  JSValue function;      /* callback, or the promise resolver of a yield */
  lv_timer_t *timer;
  int id;
  bool used;
  bool repeat;           /* setInterval keeps its binding, timeout/yield frees it */
  bool yields;           /* resolves an awaited promise instead of calling back */
};

struct qpk_camera_s
{
  int fd;
  int fb_fd;
  bool usb;
  struct uvc_camera_start usb_mode;
  uint64_t fps_started_ms;
  uint64_t display_fps_started_ms;
  uint64_t profile_started_us;
  uint64_t profile_dq_us;
  uint64_t profile_invalidate_us;
  uint64_t profile_convert_us;
  uint64_t profile_clean_us;
  uint64_t profile_qbuf_us;
  uint32_t captured_frames;
  uint32_t dropped_frames;
  uint32_t frames;
  uint32_t profile_samples;
  uint32_t profile_converted;
  uint32_t dq_errors;
  uint32_t qbuf_errors;
  uint32_t pan_errors;
  bool stop_requested;
  bool thread_alive;
  int thread_error;
  bool thread_running;
  pthread_t thread;
  bool streaming;
  bool direct_preview;
  FAR uint8_t *mmap_buffers[QPK_CAMERA_BUFFER_COUNT];
  size_t mmap_lengths[QPK_CAMERA_BUFFER_COUNT];
  unsigned int mmap_count;
  uint32_t memory_type;
  struct fb_videoinfo_s fb_video;
  struct fb_planeinfo_s fb_plane;
  unsigned int fb_page;
};

/* Canvas memory belongs to the LVGL/JS lifetime, never to the I/O workers. */
struct qpk_camera_canvas_s
{
  lv_obj_t *canvas;
  lv_timer_t *timer;
  FAR uint16_t *rgb565[2];
  uint64_t display_fps_started_ms;
  uint32_t frames;
  int display_buffer;
};

enum qpk_camera_state_e
{
  QPK_CAMERA_IDLE = 0,
  QPK_CAMERA_ACTIVE,
  QPK_CAMERA_STOPPING,
  QPK_CAMERA_FAILED,
};

struct qpk_runtime_s
{
  JSRuntime *runtime;
  JSContext *context;
  lv_obj_t *root;
  lv_obj_t **widgets;
  uint8_t *widget_types;
  void **widget_extra;
  uint64_t *widget_generations;
  uint64_t widget_serial;
  int widget_capacity;
  int widget_hint;
  struct qpk_event_s *events;
  struct qpk_event_s swipe_event;
  JSValue touch_event;
  bool touch_used;
  struct qpk_timer_s *timers;
  lv_obj_t *input_shade;
  lv_obj_t *input_textarea;
  JSValue input_callback;
  qpk_font_cb_t font_cb;
  qpk_message_cb_t toast_cb;
  qpk_message_cb_t dialog_cb;
  char name[48];
  char package[48];
  bool ha_config_access;
#ifdef CONFIG_SYSTEM_HASS
  unsigned hass_grants;
#endif
  char version[24];
  char error[160];
  uint64_t deadline_ms;
  uint32_t primary_color;
  uint32_t secondary_color;
  uint32_t surface_color;
  uint32_t card_color;
  uint32_t accent_color;
  int next_timer_id;
  struct qpk_camera_canvas_s camera_canvas;
};

static struct qpk_runtime_s g_qpk;
static char g_qpk_last_error[256];

/* The hardware session outlives JS and retired pages. The permanent lock
 * protects lifecycle flags and the published frame count; it never spans I/O.
 * Only the capture worker touches frame data, then the cleanup worker owns it
 * after a successful join. No worker may access g_qpk or any LVGL object.
 */
static struct qpk_camera_s g_camera =
{
  .fd = -1,
  .fb_fd = -1,
};
static pthread_mutex_t g_camera_lock = PTHREAD_MUTEX_INITIALIZER;
static enum qpk_camera_state_e g_camera_state;
static uint64_t g_camera_retry_at;
static unsigned int g_camera_cleanup_attempts;
static bool g_camera_cleanup_blocked;
static bool g_camera_redraw;
static int g_camera_last_error;
static bool g_photo_pending;
static int g_photo_status;
static char g_photo_path[128];
static uint64_t g_photo_status_ms;
static uint64_t qpk_now_ms(void);
/* The shutter pulses the result and then returns to its idle colour; the
 * recorded result stays readable for the quick app through photoStatus(). */
#define QPK_CAMERA_PHOTO_NOTICE_MS 2000

/* Callers already hold g_camera_lock. */
static void qpk_camera_set_photo_status(int status)
{
  g_photo_status = status;
  g_photo_status_ms = qpk_now_ms();
}
static unsigned int g_photo_sequence;

#ifndef QPK_PHOTO_DIR
#define QPK_PHOTO_DIR "/data/photos"
#endif

/* Only the capture worker writes files, using the dequeued page before UI
 * composition. No snapshot pointer escapes the page's ownership interval. */
static void qpk_camera_save(const uint16_t *pixels)
{
  uint8_t header[66] = {0};
  uint8_t row[1024];
  char path[128];
  FILE *file = NULL;
  int error = 0;
  unsigned int x;
  unsigned int y;
  pthread_mutex_lock(&g_camera_lock);
  bool pending = g_photo_pending;
  g_photo_pending = false;
  pthread_mutex_unlock(&g_camera_lock);
  if (!pending) return;

  if (mkdir(QPK_PHOTO_DIR, 0755) < 0 && errno != EEXIST)
    error = errno;
  if (!error)
    {
      /* Exclusive creation never overwrites a photo from an earlier boot. */
      do
        {
          snprintf(path, sizeof(path), QPK_PHOTO_DIR "/camera-%08u.bmp",
                   ++g_photo_sequence);
          file = fopen(path, "wbx");
        }
      while (!file && errno == EEXIST && g_photo_sequence < 100000);
      if (!file) error = errno ? errno : EIO;
    }
  if (file)
    {
      const uint32_t values[] = {307266, 66, 40, 512, (uint32_t)-300,
                                 3, 307200, 0xf800, 0x07e0, 0x001f};
      const unsigned int offsets[] = {2, 10, 14, 18, 22, 30, 34, 54, 58, 62};
      header[0] = 'B'; header[1] = 'M'; header[26] = 1; header[28] = 16;
      for (x = 0; x < 10; x++)
        for (y = 0; y < 4; y++)
          header[offsets[x] + y] = values[x] >> (8 * y);
      if (fwrite(header, 1, sizeof(header), file) != sizeof(header)) error = EIO;
      for (y = 0; !error && y < 300; y++)
        {
          if (qpk_runtime_camera_busy())
            {
              pthread_mutex_lock(&g_camera_lock);
              bool cancelled = g_camera.stop_requested;
              pthread_mutex_unlock(&g_camera_lock);
              if (cancelled) { error = ECANCELED; break; }
            }
          for (x = 0; x < 512; x++)
            {
              uint16_t pixel = pixels[y * 2 * 1024 + x * 2];
              row[x * 2] = pixel;
              row[x * 2 + 1] = pixel >> 8;
            }
          if (fwrite(row, 1, sizeof(row), file) != sizeof(row)) error = EIO;
        }
      if (!error && (fflush(file) != 0 || fsync(fileno(file)) != 0)) error = EIO;
      if (fclose(file) != 0 && !error) error = EIO;
      if (error) unlink(path);
    }
  pthread_mutex_lock(&g_camera_lock);
  qpk_camera_set_photo_status(error ? -error : 2);
  if (!error) snprintf(g_photo_path, sizeof(g_photo_path), "%s", path);
  pthread_mutex_unlock(&g_camera_lock);
}

bool qpk_runtime_camera_capture(void)
{
  pthread_mutex_lock(&g_camera_lock);
  bool ready = g_camera_state == QPK_CAMERA_ACTIVE &&
               g_camera.thread_alive && !g_camera.stop_requested &&
               g_photo_status != 1;
  if (ready)
    {
      g_photo_pending = true;
      qpk_camera_set_photo_status(1);
      g_photo_path[0] = '\0';
    }
  pthread_mutex_unlock(&g_camera_lock);
  return ready;
}

/* Live preview paint. Every desktop page is a rounded card, so the preview
 * pads the video with the page colour and draws the same control shapes.
 * These helpers run on the capture worker, never on the LVGL thread. */
#define QPK_CAMERA_CORNER_RADIUS 20
#define QPK_CAMERA_SWITCH_X      28
#define QPK_CAMERA_CLOSE_X       912
#define QPK_CAMERA_PILL_Y        518
#define QPK_CAMERA_PILL_W        94
#define QPK_CAMERA_PILL_H        64
#define QPK_CAMERA_PILL_RADIUS   18

static uint16_t qpk_camera_rgb565(uint32_t rgb)
{
  return (uint16_t)(((rgb >> 19) & 0x1f) << 11 |
                    ((rgb >> 10) & 0x3f) << 5 | ((rgb >> 3) & 0x1f));
}

/* Blend with a weight of 0..8 for the second colour. */
static uint16_t qpk_camera_blend(uint16_t first, uint16_t second,
                                unsigned weight)
{
  unsigned fr = (first >> 11) & 0x1f, fg = (first >> 5) & 0x3f;
  unsigned fb = first & 0x1f;
  unsigned sr = (second >> 11) & 0x1f, sg = (second >> 5) & 0x3f;
  unsigned sb = second & 0x1f;
  return (uint16_t)(((fr * (8 - weight) + sr * weight) / 8) << 11 |
                    ((fg * (8 - weight) + sg * weight) / 8) << 5 |
                    (fb * (8 - weight) + sb * weight) / 8);
}

static void qpk_camera_pixel(uint16_t *pixels, int x, int y, uint16_t color)
{
  if (x < 0 || x >= 1024 || y < 0 || y >= QPK_CAMERA_RAW_HEIGHT) return;
  pixels[y * 1024 + x] = color;
}

static void qpk_camera_disc(uint16_t *pixels, int cx, int cy, int radius,
                            uint16_t color)
{
  for (int y = -radius; y <= radius; y++)
    for (int x = -radius; x <= radius; x++)
      if (x * x + y * y <= radius * radius)
        qpk_camera_pixel(pixels, cx + x, cy + y, color);
}

static void qpk_camera_line(uint16_t *pixels, int x0, int y0, int x1, int y1,
                            int width, uint16_t color)
{
  int steps = abs(x1 - x0) > abs(y1 - y0) ? abs(x1 - x0) : abs(y1 - y0);
  if (!steps) steps = 1;
  for (int step = 0; step <= steps; step++)
    qpk_camera_disc(pixels, x0 + (x1 - x0) * step / steps,
                    y0 + (y1 - y0) * step / steps, width / 2, color);
}

static void qpk_camera_pill(uint16_t *pixels, int x, int y, int width,
                            int height, int radius, uint16_t fill,
                            uint16_t border)
{
  int right = x + width - 1, bottom = y + height - 1;
  for (int py = y; py <= bottom; py++)
    for (int px = x; px <= right; px++)
      {
        int dx = px < x + radius ? x + radius - px :
                 px > right - radius ? px - (right - radius) : 0;
        int dy = py < y + radius ? y + radius - py :
                 py > bottom - radius ? py - (bottom - radius) : 0;
        int distance = dx * dx + dy * dy;
        if (distance <= radius * radius)
          qpk_camera_pixel(pixels, px, py,
            distance <= (radius - 1) * (radius - 1) ? fill : border);
      }
}

/* The video spans the page width, so only its corners need the page colour to
 * read as the same rounded card as the other desktop pages. */
static void qpk_camera_frame_padding(uint16_t *pixels, uint16_t page,
                                     int radius)
{
  for (int y = 0; y < radius; y++)
    for (int x = 0; x < radius; x++)
      {
        int dx = radius - x, dy = radius - y;
        if (dx * dx + dy * dy <= radius * radius) continue;
        qpk_camera_pixel(pixels, x, y, page);
        qpk_camera_pixel(pixels, 1023 - x, y, page);
        qpk_camera_pixel(pixels, x, QPK_CAMERA_BAR_TOP - 1 - y, page);
        qpk_camera_pixel(pixels, 1023 - x, QPK_CAMERA_BAR_TOP - 1 - y, page);
      }
}

static uint16_t qpk_camera_page_color(void)
{
  return qpk_camera_rgb565(g_qpk.card_color ? g_qpk.card_color : 0x202641);
}

/* A finished capture keeps its colour for a short moment and then the shutter
 * returns to the idle accent, so the button cannot stay green forever. */
static int qpk_camera_bar_status(void)
{
  int status;
  uint64_t changed;
  pthread_mutex_lock(&g_camera_lock);
  status = g_photo_status;
  changed = g_photo_status_ms;
  pthread_mutex_unlock(&g_camera_lock);
  if (status != 0 && status != 1 && changed &&
      qpk_now_ms() - changed > QPK_CAMERA_PHOTO_NOTICE_MS)
    return 0;
  return status;
}

/* Drawn while the frame for this shutter press is still being written. */
static void qpk_camera_capture_ring(uint16_t *pixels, uint16_t color)
{
  for (int y = 0; y < 4; y++)
    for (int x = 0; x < 1024; x++)
      {
        qpk_camera_pixel(pixels, x, y, color);
        qpk_camera_pixel(pixels, x, QPK_CAMERA_BAR_TOP - 1 - y, color);
      }
  for (int x = 0; x < 4; x++)
    for (int y = 0; y < QPK_CAMERA_BAR_TOP; y++)
      {
        qpk_camera_pixel(pixels, x, y, color);
        qpk_camera_pixel(pixels, 1023 - x, y, color);
      }
}

/* RGB565 controls are composed only into a dequeued, non-scanout page. */
static int qpk_camera_bar_blit(uint16_t *pixels)
{
  int status = qpk_camera_bar_status();
  uint16_t page = qpk_camera_page_color();
  uint16_t surface = qpk_camera_rgb565(g_qpk.surface_color);
  uint16_t primary = qpk_camera_rgb565(g_qpk.primary_color);
  uint16_t accent = qpk_camera_rgb565(g_qpk.accent_color);
  uint16_t border = qpk_camera_blend(surface, primary, 2);
  uint16_t shutter = status == 1 ? qpk_camera_blend(accent, 0xffe0, 6) :
                     status < 0 ? 0xe34f4f :
                     status == 2 ? 0x2fbf8f : accent;
  for (int y = QPK_CAMERA_BAR_TOP; y < QPK_CAMERA_RAW_HEIGHT; y++)
    for (int x = 0; x < 1024; x++)
      pixels[y * 1024 + x] = y == QPK_CAMERA_BAR_TOP ?
        qpk_camera_blend(page, primary, 1) : page;
  /* A chevron returns to the camera selection controls. */
  qpk_camera_pill(pixels, QPK_CAMERA_SWITCH_X, QPK_CAMERA_PILL_Y,
                  QPK_CAMERA_PILL_W, QPK_CAMERA_PILL_H, QPK_CAMERA_PILL_RADIUS,
                  surface, border);
  qpk_camera_line(pixels, 87, 534, 71, 550, 7, primary);
  qpk_camera_line(pixels, 71, 550, 87, 566, 7, primary);
  /* Shutter ring plus the colour the capture status uses. */
  qpk_camera_disc(pixels, 512, 550, 36, primary);
  qpk_camera_disc(pixels, 512, 550, 29, shutter);
  qpk_camera_pill(pixels, QPK_CAMERA_CLOSE_X, QPK_CAMERA_PILL_Y,
                  QPK_CAMERA_PILL_W, QPK_CAMERA_PILL_H, QPK_CAMERA_PILL_RADIUS,
                  surface, border);
  qpk_camera_line(pixels, 937, 536, 961, 564, 7, primary);
  qpk_camera_line(pixels, 961, 536, 937, 564, 7, primary);
  qpk_camera_disc(pixels, 169, 550, 8,
                  status == 1 ? qpk_camera_blend(page, 0xffe0, 7) :
                  status < 0 ? 0xe34f4f : status == 2 ? 0x2fbf8f :
                  qpk_camera_blend(page, primary, 2));
  if (status == 1) qpk_camera_capture_ring(pixels, qpk_camera_rgb565(0xf6f7ff));
  qpk_camera_frame_padding(pixels, page, QPK_CAMERA_CORNER_RADIUS);
  return status;
}

static uint64_t qpk_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint64_t qpk_now_us(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int qpk_interrupt(JSRuntime *runtime, void *opaque)
{
  struct qpk_runtime_s *qpk = opaque;

  (void)runtime;
  return qpk->deadline_ms != 0 && qpk_now_ms() > qpk->deadline_ms;
}

static void qpk_deadline_begin(unsigned int budget_ms)
{
  g_qpk.deadline_ms = qpk_now_ms() + budget_ms;
  JS_UpdateStackTop(g_qpk.runtime);
}

static void qpk_deadline_end(void)
{
  g_qpk.deadline_ms = 0;
}

static void qpk_show_error(const char *prefix)
{
  JSValue exception;
  const char *message;
  const char *trace;

  exception = JS_GetException(g_qpk.context);
  message = qpk_exception_text(g_qpk.context, exception, "message");
  snprintf(g_qpk.error, sizeof(g_qpk.error), "%s%s%s",
           prefix ? prefix : "JavaScript 错误",
           message ? ": " : "", message ? message : "未知异常");
  strlcpy(g_qpk_last_error, g_qpk.error, sizeof(g_qpk_last_error));
  trace = qpk_exception_text(g_qpk.context, exception, "stack");
  printf("[qpk] %.512s\n", trace ? trace : g_qpk.error);
  if (g_qpk.toast_cb != NULL)
    {
      g_qpk.toast_cb(g_qpk.error);
    }

  if (trace != NULL)
    {
      JS_FreeCString(g_qpk.context, trace);
    }

  if (message != NULL)
    {
      JS_FreeCString(g_qpk.context, message);
    }

  JS_FreeValue(g_qpk.context, exception);
  JS_FreeValue(g_qpk.context, JS_GetException(g_qpk.context));
}

static int qpk_run_jobs(void)
{
  JSContext *context;
  int ret;

  do
    {
      ret = JS_ExecutePendingJob(g_qpk.runtime, &context);
    }
  while (ret > 0);

  if (ret < 0)
    {
      qpk_show_error("异步任务错误");
      return -1;
    }

  return 0;
}

/* A callback that spends more than half of its budget is a warning sign for
 * the app author: the platform still allows it, but nothing else on the UI
 * thread ran meanwhile, so it is the place to start slicing with
 * await system.yield(). */
static void qpk_budget_report(uint64_t started_ms, unsigned int budget_ms)
{
  uint64_t used = qpk_now_ms() - started_ms;

  if (budget_ms != 0 && used * 2 >= budget_ms)
    {
      printf("[qpk] callback used %lu ms of its %u ms budget\n",
             (unsigned long)used, budget_ms);
    }
}

static JSValue qpk_call(JSValueConst function, unsigned int budget_ms)
{
  JSValue result;
  JSValue retained = JS_DupValue(g_qpk.context, function);
  uint64_t started = qpk_now_ms();

  qpk_deadline_begin(budget_ms);
  result = JS_Call(g_qpk.context, retained, JS_UNDEFINED, 0, NULL);
  qpk_deadline_end();
  if (JS_IsException(result))
    {
      qpk_budget_report(started, budget_ms);
      qpk_show_error("事件执行错误");
      JS_FreeValue(g_qpk.context, retained);
      return result;
    }

  qpk_deadline_begin(budget_ms);
  qpk_run_jobs();
  qpk_deadline_end();
  qpk_budget_report(started, budget_ms);
  JS_FreeValue(g_qpk.context, retained);
  return result;
}

static JSValue qpk_call_args(JSValueConst function, unsigned int budget_ms,
                             int argc, JSValueConst *argv)
{
  JSValue result;
  JSValue retained = JS_DupValue(g_qpk.context, function);
  uint64_t started = qpk_now_ms();

  qpk_deadline_begin(budget_ms);
  result = JS_Call(g_qpk.context, retained, JS_UNDEFINED, argc, argv);
  qpk_deadline_end();
  if (JS_IsException(result))
    {
      qpk_budget_report(started, budget_ms);
      qpk_show_error("事件执行错误");
      JS_FreeValue(g_qpk.context, retained);
      return result;
    }

  qpk_deadline_begin(budget_ms);
  qpk_run_jobs();
  qpk_deadline_end();
  qpk_budget_report(started, budget_ms);
  JS_FreeValue(g_qpk.context, retained);
  return result;
}

static void qpk_widget_deleted(lv_event_t *event)
{
  uintptr_t handle = (uintptr_t)lv_event_get_user_data(event);
  if (handle > 0 && handle <= g_qpk.widget_capacity &&
      g_qpk.widgets[handle - 1] == lv_event_get_target(event))
    {
      g_qpk.widgets[handle - 1] = NULL;
      g_qpk.widget_types[handle - 1] = 0;
      free(g_qpk.widget_extra[handle - 1]);
      g_qpk.widget_extra[handle - 1] = NULL;
      if ((int)handle - 1 < g_qpk.widget_hint) g_qpk.widget_hint = handle - 1;
    }
}

static int qpk_add_widget(lv_obj_t *object, enum qpk_widget_type_e type)
{
  if (g_qpk.widget_serial == UINT64_MAX) return 0;
  int i = g_qpk.widget_hint;
  while (i < g_qpk.widget_capacity && g_qpk.widgets[i]) i++;
  if (i == g_qpk.widget_capacity) {
    if (i > INT_MAX / 2 || (size_t)i > SIZE_MAX / (2 * sizeof(*g_qpk.widgets)) ||
        (size_t)i > SIZE_MAX / (2 * sizeof(*g_qpk.widget_generations))) return 0;
    int capacity = i ? i * 2 : 32;
    lv_obj_t **widgets = calloc(capacity, sizeof(*widgets));
    uint8_t *types = calloc(capacity, sizeof(*types));
    void **extra = calloc(capacity, sizeof(*extra));
    uint64_t *generations = calloc(capacity, sizeof(*generations));
    if (!widgets || !types || !extra || !generations) {
      free(widgets); free(types); free(extra); free(generations); return 0;
    }
    if (i) {
      memcpy(widgets, g_qpk.widgets, i * sizeof(*widgets));
      memcpy(types, g_qpk.widget_types, i * sizeof(*types));
      memcpy(extra, g_qpk.widget_extra, i * sizeof(*extra));
      memcpy(generations, g_qpk.widget_generations, i * sizeof(*generations));
    }
    free(g_qpk.widgets); free(g_qpk.widget_types); free(g_qpk.widget_extra);
    free(g_qpk.widget_generations);
    g_qpk.widgets = widgets; g_qpk.widget_types = types; g_qpk.widget_extra = extra;
    g_qpk.widget_capacity = capacity;
    g_qpk.widget_generations = generations;
  }
  if (!lv_obj_add_event_cb(object, qpk_widget_deleted, LV_EVENT_DELETE, (void *)(uintptr_t)(i + 1))) return 0;
  g_qpk.widgets[i] = object; g_qpk.widget_types[i] = type; g_qpk.widget_hint = i + 1;
  g_qpk.widget_generations[i] = ++g_qpk.widget_serial;
  return i + 1;
}

static int qpk_arg_int(JSContext *context, int argc,
                       JSValueConst *argv, int index, int fallback);
static uint32_t qpk_arg_color(JSContext *context, int argc,
                              JSValueConst *argv, int index,
                              uint32_t fallback);

static JSValue js_ui_arc(JSContext *context, JSValueConst this_value,
                         int argc, JSValueConst *argv)
{
  lv_obj_t *arc;
  int handle;
  int value = qpk_arg_int(context, argc, argv, 4, 0);
  uint32_t track = qpk_arg_color(context, argc, argv, 5, 0xdfe5eb);
  uint32_t indicator = qpk_arg_color(context, argc, argv, 6, 0x129ddd);

  (void)this_value;
  arc = lv_arc_create(g_qpk.root);
  if (arc == NULL) return JS_ThrowOutOfMemory(context);
  lv_obj_set_pos(arc, qpk_arg_int(context, argc, argv, 0, 0),
                 qpk_arg_int(context, argc, argv, 1, 0));
  lv_obj_set_size(arc, qpk_arg_int(context, argc, argv, 2, 100),
                  qpk_arg_int(context, argc, argv, 3, 100));
  lv_arc_set_range(arc, 0, 100);
  lv_arc_set_rotation(arc, qpk_arg_int(context, argc, argv, 7, 135));
  lv_arc_set_angles(arc, 0, qpk_arg_int(context, argc, argv, 8, 270));
  lv_arc_set_value(arc, value < 0 ? 0 : value > 100 ? 100 : value);
  lv_obj_set_style_arc_color(arc, lv_color_hex(track), LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, 5, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc, lv_color_hex(indicator), LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(arc, 5, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(arc, lv_color_hex(indicator), LV_PART_KNOB);
  lv_obj_set_style_bg_opa(arc, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_pad_all(arc, 0, LV_PART_KNOB);
  lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  handle = qpk_add_widget(arc, QPK_WIDGET_ARC);
  if (handle == 0) { lv_obj_delete(arc); return JS_ThrowOutOfMemory(context); }
  return JS_NewInt32(context, handle);
}

static JSValue js_ui_arc_set(JSContext *context, JSValueConst this_value,
                             int argc, JSValueConst *argv)
{
  int handle = qpk_arg_int(context, argc, argv, 0, 0);
  int value = qpk_arg_int(context, argc, argv, 1, 0);
  if (handle <= 0 || handle > g_qpk.widget_capacity ||
      !g_qpk.widgets[handle - 1] ||
      g_qpk.widget_types[handle - 1] != QPK_WIDGET_ARC)
    return JS_ThrowRangeError(context, "invalid arc handle");
  lv_arc_set_value(g_qpk.widgets[handle - 1], value < 0 ? 0 : value > 100 ? 100 : value);
  return JS_UNDEFINED;
}

static JSValue js_ui_line(JSContext *context, JSValueConst this_value,
                          int argc, JSValueConst *argv)
{
  lv_obj_t *line;
  lv_point_precise_t *points;
  uint32_t color;
  int32_t count;
  int i, handle;
  JSValue length;

  (void)this_value;
  if (argc < 1 || !JS_IsArray(context, argv[0]))
    return JS_ThrowTypeError(context, "line points must be an array");
  length = JS_GetPropertyStr(context, argv[0], "length");
  if (JS_IsException(length) || JS_ToInt32(context, &count, length) < 0)
    { JS_FreeValue(context, length); return JS_EXCEPTION; }
  JS_FreeValue(context, length);
  if (count < 2 || count > 64) return JS_ThrowRangeError(context, "invalid line points");
  points = calloc((size_t)count, sizeof(*points));
  if (!points) return JS_ThrowOutOfMemory(context);
  for (i = 0; i < count; i++)
    {
      JSValue pair = JS_GetPropertyUint32(context, argv[0], (uint32_t)i);
      JSValue x, y;
      int32_t px, py;
      if (JS_IsException(pair) || !JS_IsArray(context, pair))
        { JS_FreeValue(context, pair); free(points); return JS_ThrowTypeError(context, "invalid line point"); }
      x = JS_GetPropertyUint32(context, pair, 0);
      y = JS_GetPropertyUint32(context, pair, 1);
      if (JS_ToInt32(context, &px, x) < 0 || JS_ToInt32(context, &py, y) < 0)
        { JS_FreeValue(context, x); JS_FreeValue(context, y); JS_FreeValue(context, pair); free(points); return JS_EXCEPTION; }
      points[i].x = px; points[i].y = py;
      JS_FreeValue(context, x); JS_FreeValue(context, y); JS_FreeValue(context, pair);
    }
  color = qpk_arg_color(context, argc, argv, 6, 0x129ddd);
  line = lv_line_create(g_qpk.root);
  if (line == NULL) { free(points); return JS_ThrowOutOfMemory(context); }
  lv_obj_set_pos(line, qpk_arg_int(context, argc, argv, 1, 0),
                 qpk_arg_int(context, argc, argv, 2, 0));
  lv_obj_set_size(line, qpk_arg_int(context, argc, argv, 3, 100),
                  qpk_arg_int(context, argc, argv, 4, 60));
  lv_line_set_points(line, points, count);
  lv_obj_set_style_line_color(line, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_line_width(line, 3, LV_PART_MAIN);
  lv_obj_set_style_line_rounded(line, true, LV_PART_MAIN);
  lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  handle = qpk_add_widget(line, QPK_WIDGET_LINE);
  if (handle == 0) { lv_obj_delete(line); free(points); return JS_ThrowOutOfMemory(context); }
  g_qpk.widget_extra[handle - 1] = points;
  return JS_NewInt32(context, handle);
}

static JSValue js_ui_line_set(JSContext *context, JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
  int handle = qpk_arg_int(context, argc, argv, 0, 0);
  int32_t count;
  int i;
  JSValue length;
  lv_point_precise_t *points;
  if (handle <= 0 || handle > g_qpk.widget_capacity ||
      !g_qpk.widgets[handle - 1] ||
      g_qpk.widget_types[handle - 1] != QPK_WIDGET_LINE ||
      argc < 2 || !JS_IsArray(context, argv[1]))
    return JS_ThrowRangeError(context, "invalid line handle");
  length = JS_GetPropertyStr(context, argv[1], "length");
  if (JS_IsException(length) || JS_ToInt32(context, &count, length) < 0)
    { JS_FreeValue(context, length); return JS_EXCEPTION; }
  JS_FreeValue(context, length);
  if (count < 2 || count > 64) return JS_ThrowRangeError(context, "invalid line points");
  points = calloc((size_t)count, sizeof(*points));
  if (!points) return JS_ThrowOutOfMemory(context);
  for (i = 0; i < count; i++)
    {
      JSValue pair = JS_GetPropertyUint32(context, argv[1], (uint32_t)i);
      JSValue x, y; int32_t px, py;
      if (JS_IsException(pair) || !JS_IsArray(context, pair))
        { JS_FreeValue(context, pair); free(points); return JS_ThrowTypeError(context, "invalid line point"); }
      x = JS_GetPropertyUint32(context, pair, 0); y = JS_GetPropertyUint32(context, pair, 1);
      if (JS_ToInt32(context, &px, x) < 0 || JS_ToInt32(context, &py, y) < 0)
        { JS_FreeValue(context, x); JS_FreeValue(context, y); JS_FreeValue(context, pair); free(points); return JS_EXCEPTION; }
      points[i].x = px; points[i].y = py;
      JS_FreeValue(context, x); JS_FreeValue(context, y); JS_FreeValue(context, pair);
    }
  free(g_qpk.widget_extra[handle - 1]);
  g_qpk.widget_extra[handle - 1] = points;
  lv_line_set_points(g_qpk.widgets[handle - 1], points, count);
  return JS_UNDEFINED;
}

static bool qpk_widget_current(int handle, uint64_t generation)
{
  return handle > 0 && handle <= g_qpk.widget_capacity &&
         g_qpk.widgets[handle - 1] != NULL &&
         g_qpk.widget_generations[handle - 1] == generation;
}

static int qpk_arg_int(JSContext *context, int argc,
                       JSValueConst *argv, int index, int fallback)
{
  int32_t value;

  if (index >= argc || JS_ToInt32(context, &value, argv[index]) < 0)
    {
      return fallback;
    }

  return value;
}

static uint32_t qpk_arg_color(JSContext *context, int argc,
                              JSValueConst *argv, int index,
                              uint32_t fallback)
{
  uint32_t value;

  if (index >= argc || JS_ToUint32(context, &value, argv[index]) < 0)
    {
      return fallback;
    }

  return value;
}

static const char *qpk_arg_string(JSContext *context, int argc,
                                  JSValueConst *argv, int index)
{
  return index < argc ? JS_ToCString(context, argv[index]) : NULL;
}

static void qpk_event_release(JSContext *context, struct qpk_event_s *binding)
{
  if (!binding->used)
    {
      return;
    }

  JSValue function = binding->function;
  binding->used = false;
  binding->owner = NULL;
  binding->function = JS_UNDEFINED;
  JS_FreeValue(context, function);
}

static void qpk_event_deleted(lv_event_t *event)
{
  struct qpk_event_s *binding = lv_event_get_user_data(event);
  if (g_qpk.context != NULL && binding != NULL && binding->used &&
      binding->owner == lv_event_get_target(event))
    {
      qpk_event_release(g_qpk.context, binding);
    }
}

static void qpk_event_clicked(lv_event_t *event)
{
  struct qpk_event_s *binding = lv_event_get_user_data(event);
  JSValue result;

  if (g_qpk.context == NULL || binding == NULL || !binding->used ||
      binding->owner != lv_event_get_target(event))
    {
      return;
    }

  result = qpk_call(binding->function, QPK_EVENT_BUDGET);
  JS_FreeValue(g_qpk.context, result);
}

static void qpk_event_swiped(lv_event_t *event)
{
  lv_indev_t *indev = lv_event_get_indev(event);
  const char *direction;
  JSValue argument;
  JSValue result;

  if (g_qpk.context == NULL || !g_qpk.swipe_event.used || indev == NULL)
    {
      return;
    }

  switch (lv_indev_get_gesture_dir(indev))
    {
      case LV_DIR_LEFT:
        direction = "left";
        break;
      case LV_DIR_RIGHT:
        direction = "right";
        break;
      case LV_DIR_TOP:
        direction = "up";
        break;
      case LV_DIR_BOTTOM:
        direction = "down";
        break;
      default:
        return;
    }

  argument = JS_NewString(g_qpk.context, direction);
  result = qpk_call_args(g_qpk.swipe_event.function, QPK_EVENT_BUDGET,
                         1, &argument);
  JS_FreeValue(g_qpk.context, argument);
  JS_FreeValue(g_qpk.context, result);
}

/* Live pointer state for quick apps that react to a finger. Coordinates are
 * fractions of the app content, so no screen geometry leaks into Javascript. */
static void qpk_touch_call(const char *state, double nx, double ny)
{
  JSValue arguments[3];
  JSValue result;

  arguments[0] = JS_NewString(g_qpk.context, state);
  arguments[1] = JS_NewFloat64(g_qpk.context, nx);
  arguments[2] = JS_NewFloat64(g_qpk.context, ny);
  result = qpk_call_args(g_qpk.touch_event, QPK_EVENT_BUDGET, 3, arguments);
  JS_FreeValue(g_qpk.context, arguments[0]);
  JS_FreeValue(g_qpk.context, arguments[1]);
  JS_FreeValue(g_qpk.context, arguments[2]);
  JS_FreeValue(g_qpk.context, result);
}

/* Board diagnostics replay one pointer update without the touch panel. */
int qpk_runtime_touch_test(const char *state, double nx, double ny)
{
  if (g_qpk.context == NULL || !g_qpk.touch_used || state == NULL) return 0;
  qpk_touch_call(state, nx, ny);
  return 1;
}

static void qpk_event_touched(lv_event_t *event)
{
  if (g_qpk.context == NULL || !g_qpk.touch_used || g_qpk.root == NULL)
    {
      return;
    }

  lv_indev_t *indev = lv_event_get_indev(event);
  if (indev == NULL)
    {
      return;
    }

  lv_point_t point;
  lv_area_t area;

  lv_indev_get_point(indev, &point);
  lv_obj_get_coords(g_qpk.root, &area);
  int width = lv_area_get_width(&area);
  int height = lv_area_get_height(&area);
  if (width <= 0 || height <= 0)
    {
      return;
    }

  lv_event_code_t code = lv_event_get_code(event);
  const char *state = code == LV_EVENT_PRESSED ? "down" :
                      code == LV_EVENT_PRESSING ? "move" :
                      code == LV_EVENT_RELEASED ? "up" : "cancel";
  qpk_touch_call(state, (double)(point.x - area.x1) / (double)width,
                 (double)(point.y - area.y1) / (double)height);
}

static JSValue js_ui_on_touch(JSContext *context, JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
  (void)this_value;
  if (argc < 1 || !JS_IsFunction(context, argv[0]))
    {
      return JS_ThrowTypeError(context, "touch handler must be a function");
    }

  if (g_qpk.touch_used)
    {
      JS_FreeValue(context, g_qpk.touch_event);
    }

  g_qpk.touch_event = JS_DupValue(context, argv[0]);
  g_qpk.touch_used = true;
  return JS_UNDEFINED;
}

static JSValue js_ui_on_swipe(JSContext *context, JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
  (void)this_value;
  if (argc < 1 || !JS_IsFunction(context, argv[0]))
    {
      return JS_ThrowTypeError(context, "swipe handler must be a function");
    }

  if (g_qpk.swipe_event.used)
    {
      JS_FreeValue(context, g_qpk.swipe_event.function);
    }

  g_qpk.swipe_event.function = JS_DupValue(context, argv[0]);
  g_qpk.swipe_event.used = true;
  return JS_UNDEFINED;
}

static JSValue js_ui_text(JSContext *context, JSValueConst this_value,
                          int argc, JSValueConst *argv)
{
  const char *text;
  lv_obj_t *label;
  int size;
  int handle;

  (void)this_value;
  text = qpk_arg_string(context, argc, argv, 0);
  if (text == NULL)
    {
      return JS_EXCEPTION;
    }

  size = qpk_arg_int(context, argc, argv, 3, 20);
  label = lv_label_create(g_qpk.root);
  if (label == NULL)
    {
      JS_FreeCString(context, text);
      return JS_ThrowOutOfMemory(context);
    }

  lv_label_set_text(label, text);
  lv_obj_set_pos(label, qpk_arg_int(context, argc, argv, 1, 24),
                 qpk_arg_int(context, argc, argv, 2, 80));
  lv_obj_set_style_text_color(label,
      lv_color_hex(qpk_arg_color(context, argc, argv, 4, 0xffffff)), 0);
  if (g_qpk.font_cb != NULL)
    {
      lv_obj_set_style_text_font(label, g_qpk.font_cb(size), 0);
    }

  /* Optional icon mode uses the built-in symbol font, not the CJK font. */
  if (qpk_arg_int(context, argc, argv, 5, 0) == 1)
    lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);

  handle = qpk_add_widget(label, QPK_WIDGET_LABEL);
  JS_FreeCString(context, text);
  if (handle == 0)
    {
      lv_obj_delete(label);
      return JS_ThrowOutOfMemory(context);
    }

  return JS_NewInt32(context, handle);
}

static void qpk_number_style(lv_obj_t *container, const char *text)
{
  lv_obj_t *label = lv_obj_get_child(container, 0);

  if (label != NULL)
    {
      lv_label_set_text(label, text);
      lv_obj_center(label);
    }
}

static JSValue js_ui_number(JSContext *context, JSValueConst this_value,
                            int argc, JSValueConst *argv)
{
  const char *text;
  lv_obj_t *container;
  lv_obj_t *label;
  int handle;

  (void)this_value;
  text = qpk_arg_string(context, argc, argv, 0);
  if (text == NULL)
    {
      return JS_EXCEPTION;
    }

  container = lv_obj_create(g_qpk.root);
  if (container == NULL)
    {
      JS_FreeCString(context, text);
      return JS_ThrowInternalError(context, "cannot create number");
    }

  lv_obj_set_pos(container, qpk_arg_int(context, argc, argv, 1, 0),
                 qpk_arg_int(context, argc, argv, 2, 0));
  lv_obj_set_size(container, qpk_arg_int(context, argc, argv, 3, 64),
                  qpk_arg_int(context, argc, argv, 4, 64));
  lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(container, 0, 0);
  lv_obj_set_style_pad_all(container, 0, 0);
  lv_obj_remove_flag(container, LV_OBJ_FLAG_CLICKABLE |
                             LV_OBJ_FLAG_SCROLLABLE);

  label = lv_label_create(container);
  if (label == NULL)
    {
      lv_obj_delete(container);
      JS_FreeCString(context, text);
      return JS_ThrowOutOfMemory(context);
    }

  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label,
      lv_color_hex(qpk_arg_color(context, argc, argv, 5, 0xffffff)), 0);
  if (g_qpk.font_cb != NULL)
    {
      lv_obj_set_style_text_font(label, g_qpk.font_cb(32), 0);
    }

  qpk_number_style(container, text);
  handle = qpk_add_widget(container, QPK_WIDGET_NUMBER);
  JS_FreeCString(context, text);
  if (handle == 0)
    {
      lv_obj_delete(container);
      return JS_ThrowOutOfMemory(context);
    }

  return JS_NewInt32(context, handle);
}

static JSValue js_ui_set_text(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
  int handle;
  const char *text;

  (void)this_value;
  handle = qpk_arg_int(context, argc, argv, 0, 0);
  text = qpk_arg_string(context, argc, argv, 1);
  if (text == NULL)
    {
      return JS_EXCEPTION;
    }

  if (handle <= 0 || handle > g_qpk.widget_capacity ||
      g_qpk.widgets[handle - 1] == NULL)
    {
      JS_FreeCString(context, text);
      return JS_ThrowRangeError(context, "invalid widget handle");
    }

  if (g_qpk.widget_types[handle - 1] == QPK_WIDGET_BUTTON)
    {
      lv_obj_t *label = lv_obj_get_child(g_qpk.widgets[handle - 1], 0);

      if (label == NULL)
        {
          JS_FreeCString(context, text);
          return JS_ThrowInternalError(context, "button label is missing");
        }

      lv_label_set_text(label, text);
    }
  else if (g_qpk.widget_types[handle - 1] == QPK_WIDGET_NUMBER)
    {
      qpk_number_style(g_qpk.widgets[handle - 1], text);
    }
  else if (g_qpk.widget_types[handle - 1] == QPK_WIDGET_LABEL)
    {
      lv_label_set_text(g_qpk.widgets[handle - 1], text);
    }
  else
    {
      JS_FreeCString(context, text);
      return JS_ThrowTypeError(context, "widget does not contain text");
    }
  JS_FreeCString(context, text);
  return JS_UNDEFINED;
}

static JSValue js_ui_set_hidden(JSContext *context, JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
  int handle;
  int hidden;

  (void)this_value;
  handle = qpk_arg_int(context, argc, argv, 0, 0);
  hidden = qpk_arg_int(context, argc, argv, 1, 0);
  if (handle <= 0 || handle > g_qpk.widget_capacity ||
      g_qpk.widgets[handle - 1] == NULL)
    {
      return JS_ThrowRangeError(context, "invalid widget handle");
    }

  if (hidden)
    {
      lv_obj_add_flag(g_qpk.widgets[handle - 1], LV_OBJ_FLAG_HIDDEN);
    }
  else
    {
      lv_obj_remove_flag(g_qpk.widgets[handle - 1], LV_OBJ_FLAG_HIDDEN);
    }

  return JS_UNDEFINED;
}

static JSValue js_ui_background(JSContext *context,
                                JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
  (void)this_value;
  lv_obj_set_style_bg_color(g_qpk.root,
      lv_color_hex(qpk_arg_color(context, argc, argv, 0, 0xffffff)), 0);
  lv_obj_set_style_bg_opa(g_qpk.root, LV_OPA_COVER, 0);
  return JS_UNDEFINED;
}

static JSValue js_ui_get_size(JSContext *context, JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
  JSValue size;

  (void)this_value;
  (void)argc;
  (void)argv;
  lv_obj_update_layout(g_qpk.root);
  size = JS_NewObject(context);
  JS_SetPropertyStr(context, size, "width",
                    JS_NewInt32(context, lv_obj_get_width(g_qpk.root)));
  JS_SetPropertyStr(context, size, "height",
                    JS_NewInt32(context, lv_obj_get_height(g_qpk.root)));
  return size;
}

static JSValue js_ui_set_color(JSContext *context, JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
  int handle;
  uint32_t color;
  lv_obj_t *widget;

  (void)this_value;
  handle = qpk_arg_int(context, argc, argv, 0, 0);
  color = qpk_arg_color(context, argc, argv, 1, 0xffffff);
  if (handle <= 0 || handle > g_qpk.widget_capacity ||
      g_qpk.widgets[handle - 1] == NULL)
    {
      return JS_ThrowRangeError(context, "invalid widget handle");
    }

  widget = g_qpk.widgets[handle - 1];
  if (g_qpk.widget_types[handle - 1] == QPK_WIDGET_LABEL ||
      g_qpk.widget_types[handle - 1] == QPK_WIDGET_NUMBER)
    {
      lv_obj_t *text = g_qpk.widget_types[handle - 1] == QPK_WIDGET_NUMBER ?
                       lv_obj_get_child(widget, 0) : widget;
      if (text == NULL)
        {
          return JS_ThrowInternalError(context, "widget text is missing");
        }

      lv_obj_set_style_text_color(text, lv_color_hex(color), 0);
    }
  else
    {
      lv_obj_set_style_bg_color(widget, lv_color_hex(color), 0);
      if (g_qpk.widget_types[handle - 1] == QPK_WIDGET_BUTTON)
        {
          lv_obj_t *label = lv_obj_get_child(widget, 0);
          unsigned brightness = ((color >> 16) & 255) +
                                ((color >> 8) & 255) + (color & 255);
          if (label) lv_obj_set_style_text_color(label,
              lv_color_hex(brightness > 510 ? 0x172033 : 0xffffff), 0);
        }
    }

  return JS_UNDEFINED;
}

/* Optional per-widget styling keeps existing quick apps' defaults intact. */
static JSValue js_ui_set_style(JSContext *context, JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
  static const char *names[] = {"radius", "borderColor", "borderWidth",
    "borderOpacity", "textColor", "fontSize", "center", "ellipsis", "enabled"};
  static const int limits[] = {64, 0xffffff, 8, 255, 0xffffff, 48, 1, 1, 1};
  int32_t values[9];
  bool present[9];
  int handle = qpk_arg_int(context, argc, argv, 0, 0);
  if (handle <= 0 || handle > g_qpk.widget_capacity || !g_qpk.widgets[handle - 1])
    return JS_ThrowRangeError(context, "invalid widget handle");
  if (argc < 2 || !JS_IsObject(argv[1]))
    return JS_ThrowTypeError(context, "style must be an object");
  uint64_t generation = g_qpk.widget_generations[handle - 1];
  for (unsigned i = 0; i < 9; i++)
    {
      JSValue value = JS_GetPropertyStr(context, argv[1], names[i]);
      if (JS_IsException(value)) return JS_EXCEPTION;
      present[i] = !JS_IsUndefined(value);
      int ret = present[i] ? JS_ToInt32(context, &values[i], value) : 0;
      JS_FreeValue(context, value);
      if (ret < 0) return JS_EXCEPTION;
      if (present[i] && (values[i] < 0 || values[i] > limits[i]))
        return JS_ThrowRangeError(context, "invalid style %s", names[i]);
    }
  if (!qpk_widget_current(handle, generation))
    return JS_ThrowRangeError(context, "widget changed during style conversion");
  lv_obj_t *obj = g_qpk.widgets[handle - 1];
  lv_obj_t *label = g_qpk.widget_types[handle - 1] == QPK_WIDGET_LABEL ? obj :
    g_qpk.widget_types[handle - 1] == QPK_WIDGET_BUTTON ? lv_obj_get_child(obj, 0) : NULL;
  if (g_qpk.widget_types[handle - 1] == QPK_WIDGET_BUTTON)
    lv_obj_set_style_shadow_width(obj, 0, 0);
  if (present[0]) lv_obj_set_style_radius(obj, values[0], 0);
  if (present[1]) lv_obj_set_style_border_color(obj, lv_color_hex(values[1]), 0);
  if (present[2]) lv_obj_set_style_border_width(obj, values[2], 0);
  if (present[3]) lv_obj_set_style_border_opa(obj, values[3], 0);
  if (label)
    {
      if (present[4]) lv_obj_set_style_text_color(label, lv_color_hex(values[4]), 0);
      if (present[5] && g_qpk.font_cb)
        lv_obj_set_style_text_font(label, g_qpk.font_cb(values[5]), 0);
      if (present[6]) lv_obj_set_style_text_align(label,
        values[6] ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
      if (present[7]) lv_label_set_long_mode(label,
        values[7] ? LV_LABEL_LONG_DOT : LV_LABEL_LONG_WRAP);
    }
  if (present[8])
    {
      if (values[8]) lv_obj_remove_state(obj, LV_STATE_DISABLED);
      else lv_obj_add_state(obj, LV_STATE_DISABLED);
    }
  return JS_UNDEFINED;
}

static JSValue js_ui_panel(JSContext *context, JSValueConst this_value,
                           int argc, JSValueConst *argv)
{
  lv_obj_t *panel;
  int handle;
  int opacity;

  (void)this_value;
  panel = lv_obj_create(g_qpk.root);
  if (panel == NULL)
    {
      return JS_ThrowInternalError(context, "cannot create panel");
    }

  lv_obj_set_pos(panel, qpk_arg_int(context, argc, argv, 0, 0),
                 qpk_arg_int(context, argc, argv, 1, 0));
  lv_obj_set_size(panel, qpk_arg_int(context, argc, argv, 2, 100),
                  qpk_arg_int(context, argc, argv, 3, 100));
  lv_obj_set_style_bg_color(panel,
      lv_color_hex(qpk_arg_color(context, argc, argv, 4, 0xffffff)), 0);
  opacity = qpk_arg_int(context, argc, argv, 6, LV_OPA_COVER);
  lv_obj_set_style_bg_opa(panel, opacity, 0);
  lv_obj_set_style_border_width(panel, 1, 0);
  lv_obj_set_style_border_color(panel, lv_color_hex(0x344144), 0);
  lv_obj_set_style_radius(panel, 8, 0);
  lv_obj_set_style_pad_all(panel, 0, 0);
  lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  handle = qpk_add_widget(panel, QPK_WIDGET_PANEL);
  if (handle == 0)
    {
      lv_obj_delete(panel);
      return JS_ThrowOutOfMemory(context);
    }

  return JS_NewInt32(context, handle);
}

static JSValue js_ui_set_pos(JSContext *context, JSValueConst this_value,
                             int argc, JSValueConst *argv)
{
  int handle;

  (void)this_value;
  handle = qpk_arg_int(context, argc, argv, 0, 0);
  if (handle <= 0 || handle > g_qpk.widget_capacity ||
      g_qpk.widgets[handle - 1] == NULL)
    {
      return JS_ThrowRangeError(context, "invalid widget handle");
    }

  uint64_t generation = g_qpk.widget_generations[handle - 1];
  int32_t x = 0, y = 0;
  if ((argc > 1 && JS_ToInt32(context, &x, argv[1]) < 0) ||
      (argc > 2 && JS_ToInt32(context, &y, argv[2]) < 0)) return JS_EXCEPTION;
  if (!qpk_widget_current(handle, generation))
    return JS_ThrowRangeError(context, "widget changed during position conversion");
  lv_obj_set_pos(g_qpk.widgets[handle - 1], x, y);
  return JS_UNDEFINED;
}

static JSValue js_ui_set_size(JSContext *context, JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
  int handle;

  (void)this_value;
  handle = qpk_arg_int(context, argc, argv, 0, 0);
  if (handle <= 0 || handle > g_qpk.widget_capacity ||
      g_qpk.widgets[handle - 1] == NULL)
    {
      return JS_ThrowRangeError(context, "invalid widget handle");
    }

  uint64_t generation = g_qpk.widget_generations[handle - 1];
  int32_t width = 0, height = 0;
  if ((argc > 1 && JS_ToInt32(context, &width, argv[1]) < 0) ||
      (argc > 2 && JS_ToInt32(context, &height, argv[2]) < 0)) return JS_EXCEPTION;
  if (!qpk_widget_current(handle, generation))
    return JS_ThrowRangeError(context, "widget changed during size conversion");
  lv_obj_set_size(g_qpk.widgets[handle - 1], width, height);
  return JS_UNDEFINED;
}

static JSValue js_ui_set_opacity(JSContext *context,
                                 JSValueConst this_value,
                                 int argc, JSValueConst *argv)
{
  int handle;
  int opacity;

  (void)this_value;
  handle = qpk_arg_int(context, argc, argv, 0, 0);
  opacity = qpk_arg_int(context, argc, argv, 1, LV_OPA_COVER);
  if (handle <= 0 || handle > g_qpk.widget_capacity ||
      g_qpk.widgets[handle - 1] == NULL)
    {
      return JS_ThrowRangeError(context, "invalid widget handle");
    }

  if (opacity < LV_OPA_TRANSP)
    {
      opacity = LV_OPA_TRANSP;
    }
  else if (opacity > LV_OPA_COVER)
    {
      opacity = LV_OPA_COVER;
    }

  lv_obj_set_style_opa(g_qpk.widgets[handle - 1], (lv_opa_t)opacity, 0);
  return JS_UNDEFINED;
}

static JSValue js_ui_show(JSContext *context, JSValueConst this_value,
                          int argc, JSValueConst *argv)
{
  int handle;

  (void)this_value;
  handle = qpk_arg_int(context, argc, argv, 0, 0);
  if (handle <= 0 || handle > g_qpk.widget_capacity ||
      g_qpk.widgets[handle - 1] == NULL)
    {
      return JS_ThrowRangeError(context, "invalid widget handle");
    }

  lv_obj_remove_flag(g_qpk.widgets[handle - 1], LV_OBJ_FLAG_HIDDEN);
  return JS_UNDEFINED;
}

static JSValue js_ui_hide(JSContext *context, JSValueConst this_value,
                          int argc, JSValueConst *argv)
{
  int handle;

  (void)this_value;
  handle = qpk_arg_int(context, argc, argv, 0, 0);
  if (handle <= 0 || handle > g_qpk.widget_capacity ||
      g_qpk.widgets[handle - 1] == NULL)
    {
      return JS_ThrowRangeError(context, "invalid widget handle");
    }

  lv_obj_add_flag(g_qpk.widgets[handle - 1], LV_OBJ_FLAG_HIDDEN);
  return JS_UNDEFINED;
}

static JSValue js_ui_remove(JSContext *context, JSValueConst this_value,
                            int argc, JSValueConst *argv)
{
  int handle;

  (void)this_value;
  handle = qpk_arg_int(context, argc, argv, 0, 0);
  if (handle <= 0 || handle > g_qpk.widget_capacity ||
      g_qpk.widgets[handle - 1] == NULL)
    {
      return JS_ThrowRangeError(context, "invalid widget handle");
    }

  lv_obj_delete(g_qpk.widgets[handle - 1]);
  return JS_UNDEFINED;
}

static JSValue js_ui_rect(JSContext *context, JSValueConst this_value,
                          int argc, JSValueConst *argv)
{
  lv_obj_t *rect;
  int handle;

  (void)this_value;
  rect = lv_obj_create(g_qpk.root);
  if (rect == NULL)
    {
      return JS_ThrowInternalError(context, "cannot create rect");
    }

  lv_obj_remove_style_all(rect);
  lv_obj_set_pos(rect, qpk_arg_int(context, argc, argv, 0, 0),
                 qpk_arg_int(context, argc, argv, 1, 0));
  lv_obj_set_size(rect, qpk_arg_int(context, argc, argv, 2, 100),
                  qpk_arg_int(context, argc, argv, 3, 100));
  lv_obj_set_style_bg_color(rect,
      lv_color_hex(qpk_arg_color(context, argc, argv, 4, 0x1a1a2e)), 0);
  lv_obj_set_style_bg_opa(rect, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(rect, 0, 0);
  lv_obj_clear_flag(rect, LV_OBJ_FLAG_SCROLLABLE);
  handle = qpk_add_widget(rect, QPK_WIDGET_RECT);
  if (handle == 0)
    {
      lv_obj_delete(rect);
      return JS_ThrowOutOfMemory(context);
    }

  return JS_NewInt32(context, handle);
}

static void qpk_input_finish(bool submit)
{
  JSContext *context = g_qpk.context;
  JSValue callback;
  JSValue argument = JS_UNDEFINED;
  JSValue result;

  if (context == NULL || g_qpk.input_shade == NULL)
    {
      return;
    }

  callback = JS_DupValue(context, g_qpk.input_callback);
  if (submit)
    {
      argument = JS_NewString(context,
                              lv_textarea_get_text(g_qpk.input_textarea));
    }

  JS_FreeValue(context, g_qpk.input_callback);
  g_qpk.input_callback = JS_UNDEFINED;
  lv_obj_delete(g_qpk.input_shade);
  g_qpk.input_shade = NULL;
  g_qpk.input_textarea = NULL;

  if (submit)
    {
      result = qpk_call_args(callback, QPK_EVENT_BUDGET, 1, &argument);
      JS_FreeValue(context, result);
      JS_FreeValue(context, argument);
    }

  JS_FreeValue(context, callback);
}

static void qpk_input_cancel(lv_event_t *event)
{
  (void)event;
  qpk_input_finish(false);
}

static void qpk_input_submit(lv_event_t *event)
{
  (void)event;
  glass_ime_commit_tree(g_qpk.input_shade);
  qpk_input_finish(true);
}

static lv_obj_t *qpk_input_button(lv_obj_t *parent, const char *text,
                                  int x, uint32_t color,
                                  lv_event_cb_t callback)
{
  lv_obj_t *button;
  lv_obj_t *label;

  button = lv_button_create(parent);
  if (button == NULL)
    {
      return NULL;
    }

  lv_obj_set_pos(button, x, 16);
  lv_obj_set_size(button, 92, 44);
  lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
  lv_obj_set_style_radius(button, 8, 0);
  lv_obj_set_style_border_width(button, 1, 0);
  lv_obj_set_style_border_color(button, lv_color_hex(0x3b484b), 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_set_style_transform_width(button, 0, LV_STATE_PRESSED);
  lv_obj_set_style_transform_height(button, 0, LV_STATE_PRESSED);
  lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);
  label = lv_label_create(button);
  if (label == NULL)
    {
      lv_obj_delete(button);
      return NULL;
    }

  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, lv_color_hex(0xffffff), 0);
  if (g_qpk.font_cb != NULL)
    {
      lv_obj_set_style_text_font(label, g_qpk.font_cb(20), 0);
    }

  lv_obj_center(label);
  return button;
}

static JSValue js_prompt_input(JSContext *context,
                               JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
  JSValue title_value = JS_UNDEFINED;
  JSValue placeholder_value = JS_UNDEFINED;
  JSValue text_value = JS_UNDEFINED;
  JSValue max_value = JS_UNDEFINED;
  JSValue password_value = JS_UNDEFINED;
  const char *title = "输入";
  const char *placeholder = "请输入内容";
  const char *text = "";
  const char *converted_title = NULL;
  const char *converted_placeholder = NULL;
  const char *converted_text = NULL;
  lv_obj_t *box;
  lv_obj_t *label;
  lv_obj_t *keyboard;
  int32_t max_length = 64;
  bool password = false;
  int root_width;
  int root_height;

  (void)this_value;
  if (argc < 2 || !JS_IsObject(argv[0]) ||
      !JS_IsFunction(context, argv[1]))
    {
      return JS_ThrowTypeError(context,
                               "prompt.input requires options and callback");
    }

  if (g_qpk.input_shade != NULL)
    {
      return JS_ThrowInternalError(context, "an input dialog is already open");
    }

  title_value = JS_GetPropertyStr(context, argv[0], "title");
  placeholder_value = JS_GetPropertyStr(context, argv[0], "placeholder");
  text_value = JS_GetPropertyStr(context, argv[0], "value");
  max_value = JS_GetPropertyStr(context, argv[0], "maxLength");
  password_value = JS_GetPropertyStr(context, argv[0], "password");
  if (!JS_IsUndefined(title_value) && !JS_IsNull(title_value))
    {
      converted_title = JS_ToCString(context, title_value);
      if (converted_title != NULL)
        {
          title = converted_title;
        }
    }

  if (!JS_IsUndefined(placeholder_value) && !JS_IsNull(placeholder_value))
    {
      converted_placeholder = JS_ToCString(context, placeholder_value);
      if (converted_placeholder != NULL)
        {
          placeholder = converted_placeholder;
        }
    }

  if (!JS_IsUndefined(text_value) && !JS_IsNull(text_value))
    {
      converted_text = JS_ToCString(context, text_value);
      if (converted_text != NULL)
        {
          text = converted_text;
        }
    }

  if (!JS_IsUndefined(max_value))
    {
      (void)JS_ToInt32(context, &max_length, max_value);
    }

  password = JS_ToBool(context, password_value) > 0;

  if (max_length < 1)
    {
      max_length = 1;
    }
  else if (max_length > 2048)
    {
      max_length = 2048;
    }

  lv_obj_update_layout(g_qpk.root);
  root_width = lv_obj_get_width(g_qpk.root);
  root_height = lv_obj_get_height(g_qpk.root);
  g_qpk.input_shade = lv_obj_create(g_qpk.root);
  if (g_qpk.input_shade == NULL)
    {
      goto err_input;
    }

  lv_obj_set_size(g_qpk.input_shade, root_width, root_height);
  lv_obj_set_style_bg_color(g_qpk.input_shade, lv_color_hex(0x02090d), 0);
  lv_obj_set_style_bg_opa(g_qpk.input_shade, LV_OPA_80, 0);
  lv_obj_set_style_border_width(g_qpk.input_shade, 0, 0);
  lv_obj_set_style_pad_all(g_qpk.input_shade, 0, 0);
  lv_obj_remove_flag(g_qpk.input_shade, LV_OBJ_FLAG_SCROLLABLE);

  box = lv_obj_create(g_qpk.input_shade);
  if (box == NULL)
    {
      goto err_input;
    }

  lv_obj_set_size(box, root_width, root_height);
  lv_obj_set_style_bg_color(box, lv_color_hex(g_qpk.surface_color), 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(box, 0, 0);
  lv_obj_set_style_radius(box, 8, 0);
  lv_obj_set_style_pad_all(box, 0, 0);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

  label = lv_label_create(box);
  if (label == NULL)
    {
      goto err_input;
    }

  lv_label_set_text(label, title);
  lv_obj_set_width(label, root_width - 256);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(label, 24, 22);
  lv_obj_set_style_text_color(label, lv_color_hex(g_qpk.primary_color), 0);
  if (g_qpk.font_cb != NULL)
    {
      lv_obj_set_style_text_font(label, g_qpk.font_cb(28), 0);
    }

  if (qpk_input_button(box, "取消", root_width - 208, 0x40515a,
                       qpk_input_cancel) == NULL ||
      qpk_input_button(box, "确定", root_width - 108, 0x16758a,
                       qpk_input_submit) == NULL)
    {
      goto err_input;
    }

  g_qpk.input_textarea = lv_textarea_create(box);
  if (g_qpk.input_textarea == NULL)
    {
      goto err_input;
    }

  lv_obj_set_pos(g_qpk.input_textarea, 24, 76);
  lv_obj_set_size(g_qpk.input_textarea, root_width - 48, 52);
  lv_textarea_set_one_line(g_qpk.input_textarea, true);
  lv_obj_set_style_text_color(g_qpk.input_textarea, lv_color_hex(g_qpk.primary_color), 0);
  lv_obj_set_style_text_color(g_qpk.input_textarea, lv_color_hex(g_qpk.secondary_color), LV_PART_TEXTAREA_PLACEHOLDER);
  lv_obj_set_style_pad_hor(g_qpk.input_textarea, 14, 0);
  lv_obj_set_style_pad_ver(g_qpk.input_textarea, 10, 0);
  lv_obj_set_style_bg_color(g_qpk.input_textarea,
                            lv_color_hex(g_qpk.surface_color), 0);
  lv_obj_set_style_border_width(g_qpk.input_textarea, 1, 0);
  lv_obj_set_style_border_color(g_qpk.input_textarea,
                                lv_color_hex(0x54d3a4), 0);
  lv_obj_set_style_radius(g_qpk.input_textarea, 8, 0);
  lv_textarea_set_max_length(g_qpk.input_textarea, max_length);
  lv_textarea_set_placeholder_text(g_qpk.input_textarea, placeholder);
  lv_textarea_set_text(g_qpk.input_textarea, text);
  lv_textarea_set_password_mode(g_qpk.input_textarea, password);
  if (g_qpk.font_cb != NULL)
    {
      lv_obj_set_style_text_font(g_qpk.input_textarea,
                                 g_qpk.font_cb(20), 0);
      lv_obj_set_style_text_font(g_qpk.input_textarea,
                                 g_qpk.font_cb(20), LV_PART_TEXTAREA_PLACEHOLDER);
    }

  keyboard = lv_keyboard_create(g_qpk.input_shade);
  if (keyboard == NULL)
    {
      goto err_input;
    }

  lv_obj_set_size(keyboard, lv_pct(100), 255);
  lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(keyboard, lv_color_hex(g_qpk.surface_color), 0);
  lv_obj_set_style_pad_all(keyboard, 12, 0);
  lv_obj_set_style_pad_row(keyboard, 8, 0);
  lv_obj_set_style_pad_column(keyboard, 6, 0);
  lv_obj_set_style_bg_color(keyboard, lv_color_hex(g_qpk.surface_color), LV_PART_ITEMS);
  lv_obj_set_style_bg_color(keyboard, lv_color_hex(g_qpk.primary_color > 0x808080 ? 0xffffff : 0x414d66), LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_text_color(keyboard, lv_color_hex(g_qpk.primary_color), LV_PART_ITEMS);
  lv_obj_set_style_radius(keyboard, 8, LV_PART_ITEMS);
  lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_keyboard_set_textarea(keyboard, g_qpk.input_textarea);
  glass_ime_attach(keyboard, g_qpk.font_cb ? g_qpk.font_cb(20) : LV_FONT_DEFAULT, !password,
                   g_qpk.primary_color, g_qpk.surface_color, 0xd82f62);
  lv_obj_add_event_cb(keyboard, qpk_input_submit, LV_EVENT_READY, NULL);
  lv_obj_add_event_cb(keyboard, qpk_input_cancel, LV_EVENT_CANCEL, NULL);
  g_qpk.input_callback = JS_DupValue(context, argv[1]);
  lv_obj_move_foreground(g_qpk.input_shade);

  if (converted_title != NULL)
    {
      JS_FreeCString(context, converted_title);
    }
  if (converted_placeholder != NULL)
    {
      JS_FreeCString(context, converted_placeholder);
    }
  if (converted_text != NULL)
    {
      JS_FreeCString(context, converted_text);
    }
  JS_FreeValue(context, title_value);
  JS_FreeValue(context, placeholder_value);
  JS_FreeValue(context, text_value);
  JS_FreeValue(context, max_value);
  JS_FreeValue(context, password_value);
  return JS_UNDEFINED;

err_input:
  if (g_qpk.input_shade != NULL)
    {
      lv_obj_delete(g_qpk.input_shade);
      g_qpk.input_shade = NULL;
      g_qpk.input_textarea = NULL;
    }

  if (converted_title != NULL)
    {
      JS_FreeCString(context, converted_title);
    }

  if (converted_placeholder != NULL)
    {
      JS_FreeCString(context, converted_placeholder);
    }

  if (converted_text != NULL)
    {
      JS_FreeCString(context, converted_text);
    }

  JS_FreeValue(context, title_value);
  JS_FreeValue(context, placeholder_value);
  JS_FreeValue(context, text_value);
  JS_FreeValue(context, max_value);
  JS_FreeValue(context, password_value);
  return JS_ThrowOutOfMemory(context);
}

/* Keys remain ASCII identifiers. Reject embedded NUL bytes before the
 * filesystem API sees a truncated key. Values keep their explicit length.
 */
static const char *qpk_storage_key(JSContext *context, int argc,
                                  JSValueConst *argv)
{
  size_t length;
  const char *key;
  if (argc < 1)
    {
      JS_ThrowTypeError(context, "storage requires a key");
      return NULL;
    }
  key = JS_ToCStringLen(context, &length, argv[0]);
  if (key != NULL && memchr(key, '\0', length) != NULL)
    {
      JS_FreeCString(context, key);
      JS_ThrowRangeError(context, "invalid storage key");
      return NULL;
    }
  return key;
}

static JSValue js_storage_get(JSContext *context, JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
  const char *key = qpk_storage_key(context, argc, argv);
  char *buffer;
  size_t length;
  int ret;
  JSValue result;
  (void)this_value;
  if (key == NULL) return JS_EXCEPTION;
  buffer = malloc(QPK_STORAGE_VALUE_MAX + 1);
  if (buffer == NULL)
    {
      JS_FreeCString(context, key);
      return JS_ThrowOutOfMemory(context);
    }
  ret = qpk_storage_read(QPK_STORAGE_ROOT, g_qpk.package, key,
                         buffer, QPK_STORAGE_VALUE_MAX + 1, &length);
  if(g_qpk.ha_config_access &&
     (!strcmp(key,"ha_url")||!strcmp(key,"ha_token"))) {
    ret=portal_ha_value(key,buffer,QPK_STORAGE_VALUE_MAX+1);
    if(!ret) length=strlen(buffer);
  }
  JS_FreeCString(context, key);
  if (ret == 0) result = JS_NewStringLen(context, buffer, length);
  else if (ret == -ENOENT) result = JS_NULL;
  else if (ret == -EINVAL) result = JS_ThrowRangeError(context, "invalid storage key");
  else result = JS_ThrowInternalError(context, "storage read failed: %d", -ret);
  free(buffer);
  return result;
}

static JSValue js_storage_set(JSContext *context, JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
  const char *key;
  const char *value;
  size_t length;
  int ret;
  (void)this_value;
  if (argc < 2) return JS_ThrowTypeError(context, "storage.set requires key and value");
  key = qpk_storage_key(context, argc, argv);
  if (key == NULL) return JS_EXCEPTION;
  value = JS_ToCStringLen(context, &length, argv[1]);
  if (value == NULL)
    {
      JS_FreeCString(context, key);
      return JS_EXCEPTION;
    }
  ret = qpk_storage_write(QPK_STORAGE_ROOT, g_qpk.package, key, value, length);
  if(!ret&&g_qpk.ha_config_access &&
     (!strcmp(key,"ha_url")||!strcmp(key,"ha_token"))) ret=portal_ha_set(key,value);
  JS_FreeCString(context, key);
  JS_FreeCString(context, value);
  if (ret == -EINVAL) return JS_ThrowRangeError(context, "invalid storage key or value");
  return ret < 0 ? JS_ThrowInternalError(context, "storage write failed: %d", -ret) : JS_UNDEFINED;
}

static JSValue js_storage_delete(JSContext *context, JSValueConst this_value,
                                 int argc, JSValueConst *argv)
{
  const char *key = qpk_storage_key(context, argc, argv);
  int ret;
  (void)this_value;
  if (key == NULL) return JS_EXCEPTION;
  ret = qpk_storage_remove(QPK_STORAGE_ROOT, g_qpk.package, key);
  JS_FreeCString(context, key);
  if (ret == -EINVAL) return JS_ThrowRangeError(context, "invalid storage key");
  return ret < 0 ? JS_ThrowInternalError(context, "storage delete failed: %d", -ret) : JS_UNDEFINED;
}

static JSValue js_ui_button(JSContext *context, JSValueConst this_value,
                            int argc, JSValueConst *argv)
{
  const char *text;
  lv_obj_t *button;
  lv_obj_t *label;
  struct qpk_event_s *binding = NULL;
  int handle;
  uint32_t button_color;
  uint32_t text_color;
  unsigned int brightness;

  (void)this_value;
  text = qpk_arg_string(context, argc, argv, 0);
  if (text == NULL)
    {
      return JS_EXCEPTION;
    }

  if (argc < 6 || !JS_IsFunction(context, argv[5]))
    {
      JS_FreeCString(context, text);
      return JS_ThrowTypeError(context, "button handler must be a function");
    }

  /* Each callback owns a stable allocation: adding more buttons must not
   * invalidate LVGL's user_data pointers. Released entries are reused. */
  for (binding = g_qpk.events; binding && binding->used; binding = binding->next) {}
  if (!binding) {
    binding = calloc(1, sizeof(*binding));
    if (!binding) { JS_FreeCString(context, text); return JS_ThrowOutOfMemory(context); }
    binding->next = g_qpk.events; g_qpk.events = binding;
  }

  button = lv_button_create(g_qpk.root);
  if (button == NULL)
    {
      JS_FreeCString(context, text);
      return JS_ThrowOutOfMemory(context);
    }

  lv_obj_set_pos(button, qpk_arg_int(context, argc, argv, 1, 220),
                 qpk_arg_int(context, argc, argv, 2, 160));
  lv_obj_set_size(button, qpk_arg_int(context, argc, argv, 3, 300),
                  qpk_arg_int(context, argc, argv, 4, 54));
  button_color = qpk_arg_color(context, argc, argv, 6, 0x6677f5);
  brightness = ((button_color >> 16) & 0xff) +
               ((button_color >> 8) & 0xff) + (button_color & 0xff);
  text_color = brightness > 510 ? 0x172033 : 0xffffff;
  lv_obj_set_style_bg_color(button, lv_color_hex(button_color), 0);
  lv_obj_set_style_radius(button, 8, 0);
  lv_obj_set_style_transform_width(button, 0, LV_STATE_PRESSED);
  lv_obj_set_style_transform_height(button, 0, LV_STATE_PRESSED);
  label = lv_label_create(button);
  if (label == NULL)
    {
      lv_obj_delete(button);
      JS_FreeCString(context, text);
      return JS_ThrowOutOfMemory(context);
    }

  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, lv_color_hex(text_color), 0);
  if (g_qpk.font_cb != NULL)
    {
      lv_obj_set_style_text_font(label, g_qpk.font_cb(20), 0);
    }

  lv_obj_set_width(label, lv_pct(100));
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(label);
  binding->function = JS_DupValue(context, argv[5]);
  binding->owner = button;
  binding->used = true;
  if (lv_obj_add_event_cb(button, qpk_event_deleted, LV_EVENT_DELETE, binding) == NULL)
    {
      qpk_event_release(context, binding);
      lv_obj_delete(button);
      JS_FreeCString(context, text);
      return JS_ThrowOutOfMemory(context);
    }

  if (lv_obj_add_event_cb(button, qpk_event_clicked, LV_EVENT_CLICKED, binding) == NULL)
    {
      lv_obj_delete(button);
      JS_FreeCString(context, text);
      return JS_ThrowOutOfMemory(context);
    }

  handle = qpk_add_widget(button, QPK_WIDGET_BUTTON);
  JS_FreeCString(context, text);
  if (handle == 0)
    {
      lv_obj_delete(button);
      return JS_ThrowOutOfMemory(context);
    }

  return JS_NewInt32(context, handle);
}

static const char *qpk_message_arg(JSContext *context, int argc,
                                   JSValueConst *argv, JSValue *holder)
{
  if (argc < 1)
    {
      return NULL;
    }

  if (JS_IsObject(argv[0]))
    {
      *holder = JS_GetPropertyStr(context, argv[0], "message");
      return JS_ToCString(context, *holder);
    }

  *holder = JS_UNDEFINED;
  return JS_ToCString(context, argv[0]);
}

static JSValue js_prompt_toast(JSContext *context,
                               JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
  JSValue holder;
  const char *message;

  (void)this_value;
  message = qpk_message_arg(context, argc, argv, &holder);
  if (message == NULL)
    {
      return JS_EXCEPTION;
    }

  if (g_qpk.toast_cb != NULL)
    {
      g_qpk.toast_cb(message);
    }

  JS_FreeCString(context, message);
  JS_FreeValue(context, holder);
  return JS_UNDEFINED;
}

static JSValue js_prompt_dialog(JSContext *context,
                                JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
  JSValue title_value = JS_UNDEFINED;
  JSValue message_value = JS_UNDEFINED;
  const char *title = NULL;
  const char *message = NULL;
  char text[256];

  (void)this_value;
  if (argc > 0 && JS_IsObject(argv[0]))
    {
      title_value = JS_GetPropertyStr(context, argv[0], "title");
      message_value = JS_GetPropertyStr(context, argv[0], "message");
      title = JS_ToCString(context, title_value);
      message = JS_ToCString(context, message_value);
    }
  else if (argc > 0)
    {
      message = JS_ToCString(context, argv[0]);
    }

  snprintf(text, sizeof(text), "%s%s%s", title ? title : "快应用",
           message ? "\n" : "", message ? message : "");
  if (g_qpk.dialog_cb != NULL)
    {
      g_qpk.dialog_cb(text);
    }

  if (title != NULL)
    {
      JS_FreeCString(context, title);
    }

  if (message != NULL)
    {
      JS_FreeCString(context, message);
    }

  JS_FreeValue(context, title_value);
  JS_FreeValue(context, message_value);
  return JS_UNDEFINED;
}

static void qpk_timer_release(JSContext *context, struct qpk_timer_s *binding)
{
  if (!binding->used)
    {
      return;
    }

  lv_timer_t *timer = binding->timer;
  JSValue function = binding->function;
  binding->used = false;
  binding->timer = NULL;
  binding->function = JS_UNDEFINED;
  if (timer != NULL)
    {
      lv_timer_delete(timer);
    }

  JS_FreeValue(context, function);
}

static void qpk_timer_cb(lv_timer_t *timer)
{
  struct qpk_timer_s *binding = lv_timer_get_user_data(timer);
  JSValue result;

  if (g_qpk.context == NULL || binding == NULL || !binding->used ||
      binding->timer != timer)
    {
      return;
    }

  int id = binding->id;

  if (binding->yields)
    {
      /* Resolve the promise the app is awaiting, then run the continuation
       * here. This callback has already returned to LVGL, so the resumption
       * is charged to its own budget instead of the caller's - that is what
       * lets a long computation proceed in slices. The continuation may
       * register another yield, which reuses this slot, so nothing may be
       * touched after the release below.
       */

      result = JS_Call(g_qpk.context, binding->function, JS_UNDEFINED, 0, NULL);
      JS_FreeValue(g_qpk.context, result);
      if (binding->used && binding->timer == timer && binding->id == id)
        {
          qpk_timer_release(g_qpk.context, binding);
        }

      qpk_deadline_begin(QPK_TIMER_BUDGET);
      qpk_run_jobs();
      qpk_deadline_end();
      return;
    }

  result = qpk_call(binding->function, QPK_TIMER_BUDGET);
  /* The callback may cancel itself and reuse this slot for a new timer. A
   * timeout or a failure ends the binding; an interval keeps it. */
  if ((JS_IsException(result) || !binding->repeat) && binding->used &&
      binding->timer == timer && binding->id == id)
    {
      qpk_timer_release(g_qpk.context, binding);
    }

  JS_FreeValue(g_qpk.context, result);
}

static struct qpk_timer_s *qpk_timer_reserve(void)
{
  struct qpk_timer_s *binding;

  for (binding = g_qpk.timers; binding && binding->used; binding = binding->next) {}
  if (!binding)
    {
      binding = calloc(1, sizeof(*binding));
      if (!binding) return NULL;
      binding->next = g_qpk.timers; g_qpk.timers = binding;
    }

  return binding;
}

/* Shared by setInterval, setTimeout and system.yield(). LVGL has no timer
 * below one frame anyway, so a floor keeps a re-arming callback from running
 * flat out. */
static JSValue js_set_timer(JSContext *context, int argc, JSValueConst *argv,
                            bool repeat)
{
  struct qpk_timer_s *binding;
  int interval;

  if (argc < 1 || !JS_IsFunction(context, argv[0]))
    {
      return JS_ThrowTypeError(context, "callback must be a function");
    }

  interval = qpk_arg_int(context, argc, argv, 1, repeat ? 1000 : 0);
  if (interval < 20)
    {
      interval = 20;
    }

  binding = qpk_timer_reserve();
  if (binding == NULL)
    {
      return JS_ThrowOutOfMemory(context);
    }

  if (g_qpk.next_timer_id == INT_MAX)
    {
      return JS_ThrowInternalError(context, "timer identifier limit reached");
    }

  binding->id = ++g_qpk.next_timer_id;
  binding->function = JS_DupValue(context, argv[0]);
  binding->used = true;
  binding->repeat = repeat;
  binding->yields = false;
  binding->timer = lv_timer_create(qpk_timer_cb, interval, binding);
  if (binding->timer == NULL)
    {
      qpk_timer_release(context, binding);
      return JS_ThrowInternalError(context, "cannot create timer");
    }

  return JS_NewInt32(context, binding->id);
}

static JSValue js_set_interval(JSContext *context,
                               JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
  (void)this_value;
  return js_set_timer(context, argc, argv, true);
}

static JSValue js_set_timeout(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
  (void)this_value;
  return js_set_timer(context, argc, argv, false);
}

/* system.yield() hands the UI thread back to LVGL and resumes the calling
 * async function from a later timer callback, which is charged to the timer
 * budget rather than the caller's event budget:
 *
 *   for (var i = 0; i < points.length; i++) {
 *     score(points[i]);
 *     if ((i & 31) === 31) await system.yield();
 *   }
 *
 * A long computation therefore completes in slices instead of being cut off
 * by the interactive deadline.
 */
static JSValue js_system_yield(JSContext *context, JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
  JSValue resolving[2];
  JSValue promise;
  struct qpk_timer_s *binding;

  (void)this_value; (void)argc; (void)argv;
  promise = JS_NewPromiseCapability(context, resolving);
  if (JS_IsException(promise))
    {
      return promise;
    }

  binding = qpk_timer_reserve();
  if (binding == NULL || g_qpk.next_timer_id == INT_MAX)
    {
      JS_FreeValue(context, resolving[0]);
      JS_FreeValue(context, resolving[1]);
      JS_FreeValue(context, promise);
      return binding == NULL ? JS_ThrowOutOfMemory(context) :
             JS_ThrowInternalError(context, "timer identifier limit reached");
    }

  binding->id = ++g_qpk.next_timer_id;
  binding->function = resolving[0];
  binding->used = true;
  binding->repeat = false;
  binding->yields = true;
  binding->timer = lv_timer_create(qpk_timer_cb, QPK_YIELD_PERIOD_MS, binding);
  if (binding->timer == NULL)
    {
      qpk_timer_release(context, binding);
      JS_FreeValue(context, resolving[1]);
      JS_FreeValue(context, promise);
      return JS_ThrowInternalError(context, "cannot create timer");
    }

  JS_FreeValue(context, resolving[1]);
  return promise;
}

static JSValue js_clear_interval(JSContext *context,
                                 JSValueConst this_value,
                                 int argc, JSValueConst *argv)
{
  int id;

  (void)this_value;
  id = qpk_arg_int(context, argc, argv, 0, 0);
  for (struct qpk_timer_s *binding = g_qpk.timers; binding; binding = binding->next)
    {

      if (binding->used && binding->id == id)
        {
          qpk_timer_release(context, binding);
          break;
        }
    }

  return JS_UNDEFINED;
}

static bool qpk_camera_cancelled(void)
{
  bool cancelled;

  pthread_mutex_lock(&g_camera_lock);
  cancelled = g_camera.stop_requested;
  pthread_mutex_unlock(&g_camera_lock);
  return cancelled;
}

/* Called with g_camera_lock held, only after all device work has finished. */
static void qpk_camera_released(void)
{
  if (g_photo_status == 1) qpk_camera_set_photo_status(-ECANCELED);
  g_photo_pending = false;
  g_camera_redraw |= g_camera.direct_preview;
  memset(&g_camera, 0, sizeof(g_camera));
  g_camera.fd = -1;
  g_camera.fb_fd = -1;
  g_camera_retry_at = 0;
  g_camera_cleanup_attempts = 0;
  g_camera_cleanup_blocked = false;
  g_camera_state = QPK_CAMERA_IDLE;
}

static void qpk_camera_cleanup_failed(int error, bool retryable)
{
  printf("[qpk] camera cleanup failed: %d\n", error);
  pthread_mutex_lock(&g_camera_lock);
  g_camera_cleanup_blocked = !retryable ||
    g_camera_cleanup_attempts >= QPK_CAMERA_CLEANUP_RETRIES;
  g_camera_retry_at = qpk_now_ms() + QPK_CAMERA_RETRY_MS;
  g_camera_state = QPK_CAMERA_FAILED;
  pthread_mutex_unlock(&g_camera_lock);
  /* Nothing in the session may be accessed after publishing this state. */
}

static void *qpk_camera_cleanup(pthread_addr_t arg)
{
  struct qpk_camera_s *camera = &g_camera;
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  unsigned int i;
  int stop_error = OK;
  int fd;
  int ret;

  (void)arg;
  if (camera->streaming && !camera->usb)
    {
      ret = ioctl(camera->fd, VIDIOC_STREAMOFF, (uintptr_t)&type);
      if (ret < 0 && errno != EPERM)
        {
          stop_error = -errno;
        }
      else
        {
          /* This upper-half returns EPERM when already stopped. Closing the
           * video device below still has to drain deferred CSI stop work.
           */
          camera->streaming = false;
        }
    }

  if (camera->thread_running)
    {
      ret = pthread_join(camera->thread, NULL);
      if (ret != 0)
        {
          qpk_camera_cleanup_failed(-ret, false);
          return NULL;
        }

      pthread_mutex_lock(&g_camera_lock);
      camera->thread_running = false;
      pthread_mutex_unlock(&g_camera_lock);
    }

  if (stop_error != OK)
    {
      qpk_camera_cleanup_failed(stop_error, true);
      return NULL;
    }

  for (i = 0; camera->memory_type == V4L2_MEMORY_MMAP &&
              i < camera->mmap_count; i++)
    {
      if (camera->mmap_buffers[i] != NULL &&
          camera->mmap_buffers[i] != MAP_FAILED)
        {
          if (munmap(camera->mmap_buffers[i], camera->mmap_lengths[i]) < 0)
            {
              qpk_camera_cleanup_failed(-errno, false);
              return NULL;
            }

          camera->mmap_buffers[i] = NULL;
        }
    }

  /* Keep the framebuffer available until video close has drained LPWORK.
   * A failed close may have consumed its descriptor. Never retry that number
   * or release the display pause when completion cannot be established.
   */
  if (camera->fd >= 0)
    {
      fd = camera->fd;
      camera->fd = -1;
      if (close(fd) < 0)
        {
          qpk_camera_cleanup_failed(-errno, false);
          return NULL;
        }
    }

  if (camera->fb_fd >= 0)
    {
      fd = camera->fb_fd;
      camera->fb_fd = -1;
      if (close(fd) < 0)
        {
          qpk_camera_cleanup_failed(-errno, false);
          return NULL;
        }
    }

  pthread_mutex_lock(&g_camera_lock);
  qpk_camera_released();
  pthread_mutex_unlock(&g_camera_lock);
  return NULL;
}

static void qpk_camera_stop(void)
{
  struct qpk_camera_canvas_s *camera = &g_qpk.camera_canvas;
  pthread_attr_t attr;
  pthread_t thread;
  int ret;

  /* Only UI-owned memory is reclaimed here. Device operations, including
   * failed-start cleanup, must never make the LVGL thread wait for hardware.
   */
  if (camera->timer != NULL)
    {
      lv_timer_delete(camera->timer);
    }

  if (camera->canvas != NULL)
    {
      lv_obj_delete(camera->canvas);
    }

  free(camera->rgb565[0]);
  free(camera->rgb565[1]);
  memset(camera, 0, sizeof(*camera));

  pthread_mutex_lock(&g_camera_lock);
  if (g_camera_state == QPK_CAMERA_IDLE ||
      g_camera_state == QPK_CAMERA_STOPPING ||
      g_camera_cleanup_blocked ||
      (g_camera_state == QPK_CAMERA_FAILED &&
       qpk_now_ms() < g_camera_retry_at))
    {
      pthread_mutex_unlock(&g_camera_lock);
      return;
    }

  g_camera.stop_requested = true;
  if (g_camera.fd < 0 && g_camera.fb_fd < 0 &&
      !g_camera.thread_running && !g_camera.streaming &&
      g_camera.mmap_count == 0)
    {
      qpk_camera_released();
      pthread_mutex_unlock(&g_camera_lock);
      return;
    }

  g_camera_state = QPK_CAMERA_STOPPING;
  g_camera_cleanup_attempts++;
  pthread_mutex_unlock(&g_camera_lock);

  ret = pthread_attr_init(&attr);
  if (ret == 0)
    {
      ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
#ifdef __NuttX__
      if (ret == 0)
        {
          ret = pthread_attr_setstacksize(&attr, QPK_CAMERA_CLEANUP_STACK);
        }
#endif
      if (ret == 0)
        {
          ret = pthread_create(&thread, &attr, qpk_camera_cleanup, NULL);
        }

      pthread_attr_destroy(&attr);
    }

  if (ret != 0)
    {
      qpk_camera_cleanup_failed(-ret, true);
    }
}

static void qpk_camera_profile(FAR struct qpk_camera_s *camera)
{
  uint64_t elapsed;
  uint32_t fps_x10;
  uint32_t converted;
  uint32_t samples;

  if (camera->profile_samples < QPK_CAMERA_PROFILE_FRAMES)
    {
      return;
    }

  elapsed = qpk_now_us() - camera->profile_started_us;
  samples = camera->profile_samples;
  converted = camera->profile_converted > 0 ?
              camera->profile_converted : 1;
  fps_x10 = elapsed > 0 ?
            (uint32_t)((uint64_t)samples * 10000000 / elapsed) : 0;
  printf("[qpk] camera stages: capture=%lu.%lu fps "
         "dq=%lu us invalidate=%lu us copy=%lu us clean=%lu us "
         "qbuf=%lu us "
         "converted=%lu/%lu\n",
         (unsigned long)(fps_x10 / 10),
         (unsigned long)(fps_x10 % 10),
         (unsigned long)(camera->profile_dq_us / samples),
         (unsigned long)(camera->profile_invalidate_us / samples),
         (unsigned long)(camera->profile_convert_us / converted),
         (unsigned long)(camera->profile_clean_us / converted),
         (unsigned long)(camera->profile_qbuf_us / samples),
         (unsigned long)camera->profile_converted,
         (unsigned long)samples);

  camera->profile_started_us = qpk_now_us();
  camera->profile_dq_us = 0;
  camera->profile_invalidate_us = 0;
  camera->profile_convert_us = 0;
  camera->profile_clean_us = 0;
  camera->profile_qbuf_us = 0;
  camera->profile_samples = 0;
  camera->profile_converted = 0;
}

#include "qpk_camera_usb.inc"

static void *qpk_camera_thread(pthread_addr_t arg)
{
  struct qpk_camera_s *camera = (struct qpk_camera_s *)arg;
  struct v4l2_buffer buffer;
  struct mallinfo memory;
  FAR uint8_t *source;
  FAR uint8_t *target;
  FAR uint8_t *released;
  struct pollfd pfd;
  size_t page_size = 0;
  uint64_t started;
  uintptr_t target_offset;
  unsigned int dq_failures = 0;
  unsigned int next_page;
  unsigned int old_page = 0;
  unsigned int pan_failures = 0;
  int fatal_error = OK;
  int pan_error;
  int ret;

  pfd.fd = camera->fd;
  pfd.events = POLLIN;
  while (!qpk_camera_cancelled())
    {
      pfd.revents = 0;
      ret = poll(&pfd, 1, 100);
      if (qpk_camera_cancelled())
        {
          break;
        }

      if (ret < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          fatal_error = -errno;
          printf("[qpk] camera poll failed: %d\n", errno);
          break;
        }

      if (ret == 0)
        {
          continue;
        }

      if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
          fatal_error = -EIO;
          break;
        }

      if ((pfd.revents & POLLIN) == 0)
        {
          continue;
        }

      memset(&buffer, 0, sizeof(buffer));
      buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buffer.memory = camera->memory_type;
      started = qpk_now_us();
      ret = ioctl(camera->fd, VIDIOC_DQBUF, (uintptr_t)&buffer);
      if (ret < 0)
        {
          if (errno != EAGAIN && errno != EINTR)
            {
              camera->dq_errors++;
              dq_failures++;
              printf("[qpk] camera DQBUF failed: %d (%u/%u)\n",
                     errno, dq_failures, QPK_CAMERA_MAX_DQ_ERRORS);
              if (dq_failures >= QPK_CAMERA_MAX_DQ_ERRORS)
                {
                  fatal_error = -errno;
                  break;
                }
            }

          continue;
        }

      dq_failures = 0;
      camera->profile_dq_us += qpk_now_us() - started;
      camera->captured_frames++;
      camera->profile_samples++;
      if (camera->profile_started_us == 0)
        {
          camera->profile_started_us = started;
        }

      if (buffer.index >= camera->mmap_count)
        {
          printf("[qpk] camera returned invalid buffer %lu\n",
                 (unsigned long)buffer.index);
          fatal_error = -EIO;
          break;
        }

      if (camera->memory_type == V4L2_MEMORY_USERPTR)
        {
          if (buffer.m.userptr !=
              (uintptr_t)camera->mmap_buffers[buffer.index])
            {
              printf("[qpk] camera returned unexpected USERPTR %p\n",
                     (FAR void *)buffer.m.userptr);
              fatal_error = -EIO;
              break;
            }

          target = camera->mmap_buffers[buffer.index];
          page_size = QPK_CAMERA_RAW_HEIGHT * camera->fb_plane.stride;
          target_offset = (uintptr_t)target -
                          (uintptr_t)camera->fb_plane.fbmem;
          if (target_offset % page_size != 0 ||
              target_offset / page_size >= QPK_CAMERA_FRAMEBUFFER_COUNT)
            {
              printf("[qpk] camera USERPTR outside display pages: %p\n",
                     target);
              fatal_error = -EIO;
              break;
            }

          old_page = camera->fb_page;
          next_page = target_offset / page_size;
          if (next_page == old_page)
            {
              printf("[qpk] camera attempted capture into active page %u\n",
                     next_page);
              fatal_error = -EBUSY;
              break;
            }
        }
      else
        {
          source = camera->mmap_buffers[buffer.index];
          started = qpk_now_us();
          up_invalidate_dcache((uintptr_t)source,
                               (uintptr_t)source + QPK_CAMERA_FRAME_SIZE);
          camera->profile_invalidate_us += qpk_now_us() - started;

          next_page = 1 - camera->fb_page;
          target = (FAR uint8_t *)camera->fb_plane.fbmem +
                   next_page * QPK_CAMERA_RAW_HEIGHT *
                   camera->fb_plane.stride;
          started = qpk_now_us();
          memcpy(target, source, QPK_CAMERA_FRAME_SIZE);
          camera->profile_convert_us += qpk_now_us() - started;
        }

      started = qpk_now_us();
      if (camera->memory_type == V4L2_MEMORY_USERPTR)
        {
          up_invalidate_dcache((uintptr_t)target,
                               (uintptr_t)target + QPK_CAMERA_FRAME_SIZE);
        }
      else
        {
          up_clean_dcache((uintptr_t)target,
                          (uintptr_t)target + QPK_CAMERA_FRAME_SIZE);
        }

      camera->profile_clean_us += qpk_now_us() - started;
      camera->profile_converted++;

      qpk_camera_save((const uint16_t *)target);
      qpk_camera_bar_blit((uint16_t *)target);
      up_clean_dcache((uintptr_t)target + 500 * 1024 * 2,
                     (uintptr_t)target + QPK_CAMERA_FRAME_SIZE);

      camera->fb_plane.xoffset = 0;
      camera->fb_plane.yoffset =
        next_page * QPK_CAMERA_RAW_HEIGHT;
      ret = ioctl(camera->fb_fd, FBIOPAN_DISPLAY,
                  (uintptr_t)&camera->fb_plane);
      if (ret < 0)
        {
          pan_error = errno;
          camera->pan_errors++;
          camera->dropped_frames++;
          pan_failures++;
          printf("[qpk] framebuffer pan failed: %d (%u/%u)\n",
                 pan_error, pan_failures, QPK_CAMERA_MAX_PAN_ERRORS);
        }
      else
        {
          pan_failures = 0;
          camera->fb_page = next_page;
          pthread_mutex_lock(&g_camera_lock);
          camera->frames++;
          pthread_mutex_unlock(&g_camera_lock);
          if (camera->frames == 1)
            {
              camera->fps_started_ms = qpk_now_ms();
              camera->display_fps_started_ms = camera->fps_started_ms;
              printf("[qpk] camera first frame displayed: %lu bytes\n",
                     (unsigned long)buffer.bytesused);
            }
          else if (camera->frames == 31)
            {
              uint64_t elapsed =
                qpk_now_ms() - camera->display_fps_started_ms;
              uint32_t fps_x10 = elapsed > 0 ? 300000 / elapsed : 0;

              printf("[qpk] camera preview fps: %lu.%lu "
                     "captured=%lu dropped=%lu\n",
                     (unsigned long)(fps_x10 / 10),
                     (unsigned long)(fps_x10 % 10),
                     (unsigned long)camera->captured_frames,
                     (unsigned long)camera->dropped_frames);
            }
        }

      if (camera->memory_type == V4L2_MEMORY_USERPTR)
        {
          /* On a successful flip, only the page that just left scanout may
           * return to CSI.  If the flip failed, the newly captured page was
           * never displayed and remains the safe capture target.
           */

          released = ret == OK ?
            (FAR uint8_t *)camera->fb_plane.fbmem + old_page * page_size :
            target;
          camera->mmap_buffers[buffer.index] = released;
          buffer.m.userptr = (uintptr_t)released;
          buffer.length = QPK_CAMERA_FRAME_SIZE;
        }

      started = qpk_now_us();
      if (ioctl(camera->fd, VIDIOC_QBUF, (uintptr_t)&buffer) < 0)
        {
          camera->qbuf_errors++;
          fatal_error = -errno;
          printf("[qpk] camera QBUF failed: %d\n", errno);
          break;
        }

      camera->profile_qbuf_us += qpk_now_us() - started;
      qpk_camera_profile(camera);

      if (camera->captured_frames % QPK_CAMERA_HEALTH_FRAMES == 0)
        {
          memory = mallinfo();
          printf("[qpk] camera health: captured=%lu displayed=%lu "
                 "dropped=%lu heap_free=%lu heap_largest=%lu "
                 "dqerr=%lu qbuferr=%lu panerr=%lu\n",
                 (unsigned long)camera->captured_frames,
                 (unsigned long)camera->frames,
                 (unsigned long)camera->dropped_frames,
                 (unsigned long)memory.fordblks,
                 (unsigned long)memory.mxordblk,
                 (unsigned long)camera->dq_errors,
                 (unsigned long)camera->qbuf_errors,
                 (unsigned long)camera->pan_errors);
        }

      if (pan_failures >= QPK_CAMERA_MAX_PAN_ERRORS)
        {
          fatal_error = -pan_error;
          break;
        }
    }

  if (fatal_error < 0)
    {
      printf("[qpk] camera worker stopped at frame %lu: %d\n",
             (unsigned long)camera->frames, fatal_error);
    }

  pthread_mutex_lock(&g_camera_lock);
  camera->thread_error = fatal_error;
  camera->thread_alive = false;
  pthread_mutex_unlock(&g_camera_lock);
  return NULL;
}

static void qpk_camera_poll(lv_timer_t *timer)
{
  struct qpk_camera_canvas_s *camera = &g_qpk.camera_canvas;
  uint64_t elapsed;
  uint64_t now;
  uint32_t fps_x10;

  (void)timer;
  if (camera->canvas == NULL)
    {
      return;
    }

  camera->display_buffer = 1 - camera->display_buffer;
  lv_canvas_set_buffer(camera->canvas,
                       camera->rgb565[camera->display_buffer],
                       QPK_CAMERA_VIEW_WIDTH, QPK_CAMERA_VIEW_HEIGHT,
                       LV_COLOR_FORMAT_RGB565);
  lv_obj_invalidate(camera->canvas);
  camera->frames++;

  now = qpk_now_ms();
  if (camera->frames == 1)
    {
      camera->display_fps_started_ms = now;
      printf("[qpk] blank canvas baseline started\n");
    }
  else if ((camera->frames - 1) % 100 == 0)
    {
      elapsed = now - camera->display_fps_started_ms;
      fps_x10 = elapsed > 0 ? 1000000 / elapsed : 0;
      printf("[qpk] blank canvas fps: %lu.%lu frames=%lu\n",
             (unsigned long)(fps_x10 / 10),
             (unsigned long)(fps_x10 % 10),
             (unsigned long)camera->frames);
      camera->display_fps_started_ms = now;
    }
}

static int qpk_camera_blank_start(int x, int y)
{
  struct qpk_camera_canvas_s *camera = &g_qpk.camera_canvas;
  size_t frame_bytes = QPK_CAMERA_VIEW_WIDTH * QPK_CAMERA_VIEW_HEIGHT *
                       sizeof(uint16_t);
  size_t i;
  int ret;

  qpk_camera_stop();
  if (qpk_runtime_camera_busy())
    {
      return -EBUSY;
    }

  camera->rgb565[0] = memalign(64, frame_bytes);
  camera->rgb565[1] = memalign(64, frame_bytes);
  if (camera->rgb565[0] == NULL || camera->rgb565[1] == NULL)
    {
      ret = -ENOMEM;
      goto error;
    }

  for (i = 0; i < frame_bytes / sizeof(uint16_t); i++)
    {
      camera->rgb565[0][i] = 0x0841;
      camera->rgb565[1][i] = 0x1082;
    }

  camera->display_buffer = 0;
  camera->canvas = lv_canvas_create(g_qpk.root);
  if (camera->canvas == NULL)
    {
      ret = -ENOMEM;
      goto error;
    }

  lv_canvas_set_buffer(camera->canvas, camera->rgb565[0],
                       QPK_CAMERA_VIEW_WIDTH, QPK_CAMERA_VIEW_HEIGHT,
                       LV_COLOR_FORMAT_RGB565);
  lv_obj_set_pos(camera->canvas, x, y);
  lv_obj_remove_flag(camera->canvas,
                     LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(camera->canvas, 8, 0);
  camera->timer = lv_timer_create(qpk_camera_poll,
                                  QPK_CAMERA_PREVIEW_PERIOD_MS, NULL);
  if (camera->timer == NULL)
    {
      ret = -ENOMEM;
      goto error;
    }

  printf("[qpk] blank canvas ready: %dx%d period=%d ms\n",
         QPK_CAMERA_VIEW_WIDTH, QPK_CAMERA_VIEW_HEIGHT,
         QPK_CAMERA_PREVIEW_PERIOD_MS);
  return OK;

error:
  qpk_camera_stop();
  return ret;
}

static int qpk_camera_start_source(int x, int y, int usb_id, uint32_t generation,
                                    unsigned mode, uint32_t interval)
{
  struct qpk_camera_s *camera = &g_camera;
  struct v4l2_requestbuffers request;
  struct v4l2_format format;
  struct v4l2_streamparm parm;
  struct v4l2_buffer buffer;
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  unsigned int i;
  int ret;

  (void)x;
  (void)y;
  qpk_camera_stop();
  pthread_mutex_lock(&g_camera_lock);
  if (g_camera_state != QPK_CAMERA_IDLE)
    {
      pthread_mutex_unlock(&g_camera_lock);
      return -EBUSY;
    }

  g_camera_state = QPK_CAMERA_ACTIVE;
  pthread_mutex_unlock(&g_camera_lock);
  camera->usb = usb_id >= 0;
  camera->usb_mode.generation = generation;
  camera->usb_mode.mode = mode;
  camera->usb_mode.interval = interval;
  g_camera_last_error = 0;
  char device[24];
  if (camera->usb) snprintf(device, sizeof(device), "/dev/uvc%d", usb_id);
  else snprintf(device, sizeof(device), "%s", QPK_CAMERA_DEVICE);
  camera->fd = open(device, O_RDWR | O_NONBLOCK);
  if (camera->fd < 0)
    {
      ret = -errno;
      printf("[qpk] camera open %s failed: %d\n",
             QPK_CAMERA_DEVICE, -ret);
      goto error;
    }

  if (camera->usb) goto framebuffer;
  memset(&format, 0, sizeof(format));
  format.type = type;
  format.fmt.pix.width = QPK_CAMERA_RAW_WIDTH;
  format.fmt.pix.height = QPK_CAMERA_RAW_HEIGHT;
  format.fmt.pix.field = V4L2_FIELD_ANY;
  format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
  ret = ioctl(camera->fd, VIDIOC_S_FMT, (uintptr_t)&format);
  if (ret < 0)
    {
      ret = -errno;
      printf("[qpk] camera S_FMT failed: %d\n", -ret);
      goto error;
    }

  /* Do not rely on the driver's default interval.  The V4L2 upper-half can
   * otherwise retain its low-rate default even though SC2336 advertises
   * 30 fps. */
  memset(&parm, 0, sizeof(parm));
  parm.type = type;
  parm.parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
  parm.parm.capture.timeperframe.numerator = 1;
  parm.parm.capture.timeperframe.denominator = 30;
  ret = ioctl(camera->fd, VIDIOC_S_PARM, (uintptr_t)&parm);
  if (ret < 0)
    {
      ret = -errno;
      printf("[qpk] camera S_PARM failed: %d\n", -ret);
      goto error;
    }

framebuffer:
  camera->fb_fd = open("/dev/fb0", O_RDWR);
  if (camera->fb_fd < 0)
    {
      ret = -errno;
      printf("[qpk] camera framebuffer open failed: %d\n", -ret);
      goto error;
    }

  memset(&camera->fb_video, 0, sizeof(camera->fb_video));
  memset(&camera->fb_plane, 0, sizeof(camera->fb_plane));
  ret = ioctl(camera->fb_fd, FBIOGET_VIDEOINFO,
              (uintptr_t)&camera->fb_video);
  if (ret < 0)
    {
      ret = -errno;
      printf("[qpk] camera FB VIDEOINFO failed: %d\n", -ret);
      goto error;
    }

  camera->fb_plane.display = 0;
  ret = ioctl(camera->fb_fd, FBIOGET_PLANEINFO,
              (uintptr_t)&camera->fb_plane);
  if (ret < 0)
    {
      ret = -errno;
      printf("[qpk] camera FB PLANEINFO failed: %d\n", -ret);
      goto error;
    }

  if (camera->fb_video.fmt != FB_FMT_RGB16_565 ||
      camera->fb_video.xres != QPK_CAMERA_RAW_WIDTH ||
      camera->fb_video.yres != QPK_CAMERA_RAW_HEIGHT ||
      camera->fb_plane.fbmem == NULL ||
      camera->fb_plane.stride !=
        QPK_CAMERA_RAW_WIDTH * sizeof(uint16_t) ||
      camera->fb_plane.xoffset != 0 ||
      camera->fb_plane.yoffset % QPK_CAMERA_RAW_HEIGHT != 0 ||
      camera->fb_plane.yoffset / QPK_CAMERA_RAW_HEIGHT >=
        QPK_CAMERA_FRAMEBUFFER_COUNT ||
      camera->fb_plane.yres_virtual <
        QPK_CAMERA_RAW_HEIGHT * QPK_CAMERA_FRAMEBUFFER_COUNT ||
      camera->fb_plane.fblen <
        QPK_CAMERA_FRAME_SIZE * QPK_CAMERA_FRAMEBUFFER_COUNT)
    {
      ret = -ENOTSUP;
      printf("[qpk] camera framebuffer geometry unsupported\n");
      goto error;
    }

  camera->fb_page = camera->fb_plane.yoffset / QPK_CAMERA_RAW_HEIGHT;
  if (camera->usb)
    {
      pthread_mutex_lock(&g_camera_lock);
      camera->direct_preview = true;
      pthread_mutex_unlock(&g_camera_lock);
      goto launch_worker;
    }
  memset(&request, 0, sizeof(request));
  request.type = type;
  request.memory = V4L2_MEMORY_USERPTR;
  request.count = QPK_CAMERA_DISPLAY_BUFFER_COUNT;
  request.mode = V4L2_BUF_MODE_RING;
  ret = ioctl(camera->fd, VIDIOC_REQBUFS, (uintptr_t)&request);
  if (ret < 0 || request.count != QPK_CAMERA_DISPLAY_BUFFER_COUNT)
    {
      ret = ret < 0 ? -errno : -ENOMEM;
      printf("[qpk] camera USERPTR REQBUFS failed: %d count=%lu\n",
             -ret, (unsigned long)request.count);
      goto error;
    }

  camera->memory_type = V4L2_MEMORY_USERPTR;
  camera->mmap_count = request.count;
  camera->fb_page = camera->fb_plane.yoffset / QPK_CAMERA_RAW_HEIGHT;

  for (i = 0; i < camera->mmap_count; i++)
    {
      camera->mmap_lengths[i] = QPK_CAMERA_FRAME_SIZE;
      camera->mmap_buffers[i] =
        (FAR uint8_t *)camera->fb_plane.fbmem +
        ((camera->fb_page + 1 + i) % QPK_CAMERA_FRAMEBUFFER_COUNT) *
        QPK_CAMERA_RAW_HEIGHT *
        camera->fb_plane.stride;

      memset(&buffer, 0, sizeof(buffer));
      buffer.type = type;
      buffer.memory = camera->memory_type;
      buffer.index = i;
      buffer.length = camera->mmap_lengths[i];
      buffer.m.userptr = (uintptr_t)camera->mmap_buffers[i];
      ret = ioctl(camera->fd, VIDIOC_QBUF, (uintptr_t)&buffer);
      if (ret < 0)
        {
          ret = -errno;
          printf("[qpk] camera USERPTR QBUF %u failed: %d\n", i, -ret);
          goto error;
        }
    }

  /* Even a failed STREAMON can leave driver work to drain. Keep LVGL away
   * from these USERPTR pages until the asynchronous device close completes.
   */
  pthread_mutex_lock(&g_camera_lock);
  camera->direct_preview = true;
  camera->streaming = true;
  pthread_mutex_unlock(&g_camera_lock);
  ret = ioctl(camera->fd, VIDIOC_STREAMON, (uintptr_t)&type);
  if (ret < 0)
    {
      ret = -errno;
      printf("[qpk] camera STREAMON failed: %d\n", -ret);
      goto error;
    }

launch_worker:
  pthread_mutex_lock(&g_camera_lock);
  camera->thread_alive = true;
  camera->thread_running = true;
  camera->thread_error = OK;
  pthread_mutex_unlock(&g_camera_lock);
  printf("[qpk] camera %s worker starting\n", camera->usb ? "USB MJPEG" : "CSI USERPTR");

  pthread_attr_t capture_attr;
  ret = pthread_attr_init(&capture_attr);
  if (ret == 0)
    {
      ret = pthread_attr_setstacksize(&capture_attr, 16384);
      if (ret == 0)
        ret = pthread_create(&camera->thread, &capture_attr,
                             camera->usb ? qpk_camera_usb_thread : qpk_camera_thread, camera);
      pthread_attr_destroy(&capture_attr);
    }
  if (ret != 0)
    {
      pthread_mutex_lock(&g_camera_lock);
      camera->thread_alive = false;
      camera->thread_running = false;
      pthread_mutex_unlock(&g_camera_lock);
      ret = -ret;
      goto error;
    }

  printf("[qpk] camera direct preview ready\n");
  return OK;

error:
  g_camera_last_error = ret;
  qpk_camera_stop();
  return ret;
}

static int qpk_camera_start(int x, int y)
{ return qpk_camera_start_source(x, y, -1, 1, 0, 0); }

#include "qpk_camera_devices.inc"

static JSValue js_camera_capture(JSContext *context, JSValueConst self,
                                 int argc, JSValueConst *argv)
{
  return JS_NewBool(context, qpk_runtime_camera_capture());
}

static JSValue js_camera_active(JSContext *context, JSValueConst self,
                                int argc, JSValueConst *argv)
{
  return JS_NewBool(context, qpk_runtime_camera_active());
}

static JSValue js_camera_photo_status(JSContext *context, JSValueConst self,
                                      int argc, JSValueConst *argv)
{
  JSValue result = JS_NewObject(context);
  char path[sizeof(g_photo_path)];
  int status;
  pthread_mutex_lock(&g_camera_lock);
  status = g_photo_status;
  snprintf(path, sizeof(path), "%s", g_photo_path);
  pthread_mutex_unlock(&g_camera_lock);
  JS_SetPropertyStr(context, result, "status", JS_NewInt32(context, status));
  JS_SetPropertyStr(context, result, "path", JS_NewString(context, path));
  return result;
}

static JSValue js_camera_start(JSContext *context,
                               JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
  int x;
  int y;
  int ret;

  (void)this_value;
  x = qpk_arg_int(context, argc, argv, 0, 124);
  y = qpk_arg_int(context, argc, argv, 1, 12);
  ret = qpk_camera_start(x, y);
  if (ret < 0)
    {
      printf("[qpk] camera start failed: %d\n", ret);
      if (g_qpk.toast_cb != NULL)
        {
          g_qpk.toast_cb("摄像头启动失败");
        }
    }

  return JS_NewBool(context, ret >= 0);
}

static JSValue js_camera_stop(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
  (void)context;
  (void)this_value;
  (void)argc;
  (void)argv;
  qpk_camera_stop();
  return JS_UNDEFINED;
}

static JSValue js_camera_blank(JSContext *context,
                               JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
  int x;
  int y;
  int ret;

  (void)this_value;
  x = qpk_arg_int(context, argc, argv, 0, 124);
  y = qpk_arg_int(context, argc, argv, 1, 12);
  ret = qpk_camera_blank_start(x, y);
  if (ret < 0)
    {
      printf("[qpk] blank canvas start failed: %d\n", ret);
    }

  return JS_NewBool(context, ret >= 0);
}

static JSValue js_camera_frames(JSContext *context,
                                JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
  uint32_t frames;

  (void)this_value;
  (void)argc;
  (void)argv;
  pthread_mutex_lock(&g_camera_lock);
  frames = g_camera.frames;
  pthread_mutex_unlock(&g_camera_lock);
  if (g_qpk.camera_canvas.canvas != NULL)
    {
      frames = g_qpk.camera_canvas.frames;
    }
  return JS_NewUint32(context, frames);
}

/* Runs inside the existing LVGL-driven JS callback, without I/O or a worker. */
static JSValue js_desktop_publish(JSContext *context, JSValueConst this_value,
                                 int argc, JSValueConst *argv)
{
  int32_t values[5];
  double number;
  int i;

  (void)this_value;
  if (argc != 5)
    {
      return JS_ThrowTypeError(context, "desktop.publish needs five integers");
    }

  for (i = 0; i < 5; i++)
    {
      if (!JS_IsNumber(argv[i]) ||
          JS_ToFloat64(context, &number, argv[i]) < 0 ||
          !(number >= -999 && number <= 1000000) ||
          number != (int32_t)number)
        {
          return JS_ThrowTypeError(context, "invalid desktop snapshot");
        }

      values[i] = (int32_t)number;
    }

  if ((values[0] != -999 && (values[0] < -80 || values[0] > 80)) ||
      values[1] < -1 || values[1] > 100 || values[2] < -1 || values[2] > 2000 ||
      values[3] < 0 || values[4] < values[3] || values[4] > 100000)
    {
      return JS_ThrowRangeError(context, "desktop snapshot out of range");
    }

  glass_dashboard_publish(values[0], values[1], values[2],
                          values[3], values[4]);
  return JS_UNDEFINED;
}

static JSValue js_ha_get_state(JSContext *context, JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
  const char *url;
  const char *token;
  const char *entity;
  int ret = -EINVAL;

  (void)this_value;
  if (argc < 3)
    {
      return JS_NewBool(context, false);
    }

  url = JS_ToCString(context, argv[0]);
  token = JS_ToCString(context, argv[1]);
  entity = JS_ToCString(context, argv[2]);
  if (url != NULL && token != NULL && entity != NULL)
    {
      ret = qpk_ha_get_state(url, token, entity);
    }

  JS_FreeCString(context, url);
  JS_FreeCString(context, token);
  JS_FreeCString(context, entity);
  return JS_NewBool(context, ret == 0);
}

static JSValue js_ha_get(JSContext *context, JSValueConst this_value,
                         int argc, JSValueConst *argv)
{
  const char *url;
  const char *token;
  const char *resource;
  int ret = -EINVAL;

  (void)this_value;
  if (argc < 3)
    {
      return JS_NewBool(context, false);
    }

  url = JS_ToCString(context, argv[0]);
  token = JS_ToCString(context, argv[1]);
  resource = JS_ToCString(context, argv[2]);
  if (url != NULL && token != NULL && resource != NULL)
    {
      ret = qpk_ha_get(url, token, resource);
    }

  JS_FreeCString(context, url);
  JS_FreeCString(context, token);
  JS_FreeCString(context, resource);
  return JS_NewBool(context, ret == 0);
}

static JSValue js_ha_call_service(JSContext *context,
                                  JSValueConst this_value,
                                  int argc, JSValueConst *argv)
{
  const char *url;
  const char *token;
  const char *domain;
  const char *service;
  const char *data;
  int ret = -EINVAL;

  (void)this_value;
  if (argc < 5)
    {
      return JS_NewBool(context, false);
    }

  url = JS_ToCString(context, argv[0]);
  token = JS_ToCString(context, argv[1]);
  domain = JS_ToCString(context, argv[2]);
  service = JS_ToCString(context, argv[3]);
  data = JS_ToCString(context, argv[4]);
  if (url != NULL && token != NULL && domain != NULL && service != NULL &&
      data != NULL)
    {
      ret = qpk_ha_call_service(url, token, domain, service, data);
    }

  JS_FreeCString(context, url);
  JS_FreeCString(context, token);
  JS_FreeCString(context, domain);
  JS_FreeCString(context, service);
  JS_FreeCString(context, data);
  return JS_NewBool(context, ret == 0);
}

static JSValue js_ha_poll(JSContext *context, JSValueConst this_value,
                          int argc, JSValueConst *argv)
{
  struct qpk_ha_result_s result;
  JSValue object;

  (void)this_value;
  (void)argc;
  (void)argv;
  qpk_ha_poll(&result);
  object = JS_NewObject(context);
  JS_SetPropertyStr(context, object, "busy",
                    JS_NewBool(context, result.busy));
  JS_SetPropertyStr(context, object, "done",
                    JS_NewBool(context, result.done));
  JS_SetPropertyStr(context, object, "status",
                    JS_NewInt32(context, result.status));
  JS_SetPropertyStr(context, object, "error",
                    JS_NewInt32(context, result.error));
  JS_SetPropertyStr(context, object, "body",
                    JS_NewString(context,
                                 result.body == NULL ? "" : result.body));
  if (result.body != NULL)
    {
      memset(result.body, 0, strlen(result.body));
      free(result.body);
    }
  return object;
}

static JSValue js_app_get_info(JSContext *context,
                               JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
  JSValue info;

  (void)this_value;
  (void)argc;
  (void)argv;
  info = JS_NewObject(context);
  JS_SetPropertyStr(context, info, "name",
                    JS_NewString(context, g_qpk.name));
  JS_SetPropertyStr(context, info, "packageName",
                    JS_NewString(context, g_qpk.package));
  JS_SetPropertyStr(context, info, "versionName",
                    JS_NewString(context, g_qpk.version));
  return info;
}

static JSValue js_console_log(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
  int i;

  (void)this_value;
  printf("[qpk]");
  for (i = 0; i < argc; i++)
    {
      const char *text = JS_ToCString(context, argv[i]);
      if (text == NULL)
        {
          return JS_EXCEPTION;
        }

      printf(" %s", text);
      JS_FreeCString(context, text);
    }

  printf("\n");
  return JS_UNDEFINED;
}

#include "qpk_espdl_ui.inc"
#include "qpk_hardware_js.inc"

static void qpk_install_api(JSContext *context)
{
  JSValue global;
  JSValue object;

  global = JS_GetGlobalObject(context);
  object = JS_NewObject(context);
  JS_SetPropertyStr(context, object, "text",
                    JS_NewCFunction(context, js_ui_text, "text", 6));
  JS_SetPropertyStr(context, object, "number",
                    JS_NewCFunction(context, js_ui_number, "number", 6));
  JS_SetPropertyStr(context, object, "setText",
                    JS_NewCFunction(context, js_ui_set_text, "setText", 2));
  JS_SetPropertyStr(context, object, "setHidden",
                    JS_NewCFunction(context, js_ui_set_hidden,
                                    "setHidden", 2));
  JS_SetPropertyStr(context, object, "background",
                    JS_NewCFunction(context, js_ui_background,
                                    "background", 1));
  JS_SetPropertyStr(context, object, "getSize",
                    JS_NewCFunction(context, js_ui_get_size,
                                    "getSize", 0));
  JS_SetPropertyStr(context, object, "setColor",
                    JS_NewCFunction(context, js_ui_set_color,
                                    "setColor", 2));
  JS_SetPropertyStr(context, object, "setStyle",
                    JS_NewCFunction(context, js_ui_set_style, "setStyle", 2));
  JS_SetPropertyStr(context, object, "panel",
                    JS_NewCFunction(context, js_ui_panel, "panel", 7));
  JS_SetPropertyStr(context, object, "button",
                    JS_NewCFunction(context, js_ui_button, "button", 7));
  JS_SetPropertyStr(context, object, "onTouch",
                    JS_NewCFunction(context, js_ui_on_touch,
                                    "onTouch", 1));
  JS_SetPropertyStr(context, object, "onSwipe",
                    JS_NewCFunction(context, js_ui_on_swipe,
                                    "onSwipe", 1));
  JS_SetPropertyStr(context, object, "setPos",
                    JS_NewCFunction(context, js_ui_set_pos, "setPos", 3));
  JS_SetPropertyStr(context, object, "setSize",
                    JS_NewCFunction(context, js_ui_set_size, "setSize", 3));
  JS_SetPropertyStr(context, object, "setOpacity",
                    JS_NewCFunction(context, js_ui_set_opacity,
                                    "setOpacity", 2));
  JS_SetPropertyStr(context, object, "show",
                    JS_NewCFunction(context, js_ui_show, "show", 1));
  JS_SetPropertyStr(context, object, "hide",
                    JS_NewCFunction(context, js_ui_hide, "hide", 1));
  JS_SetPropertyStr(context, object, "remove",
                    JS_NewCFunction(context, js_ui_remove, "remove", 1));
  JS_SetPropertyStr(context, object, "rect",
                    JS_NewCFunction(context, js_ui_rect, "rect", 5));
  JS_SetPropertyStr(context, object, "arc",
                    JS_NewCFunction(context, js_ui_arc, "arc", 9));
  JS_SetPropertyStr(context, object, "arcSet",
                    JS_NewCFunction(context, js_ui_arc_set, "arcSet", 2));
  JS_SetPropertyStr(context, object, "line",
                    JS_NewCFunction(context, js_ui_line, "line", 7));
  JS_SetPropertyStr(context, object, "lineSet",
                    JS_NewCFunction(context, js_ui_line_set, "lineSet", 2));
  JS_SetPropertyStr(context, object, "primary",
                    JS_NewUint32(context, g_qpk.primary_color));
  JS_SetPropertyStr(context, object, "secondary",
                    JS_NewUint32(context, g_qpk.secondary_color));
  JS_SetPropertyStr(context, object, "surface",
                    JS_NewUint32(context, g_qpk.surface_color));
  JS_SetPropertyStr(context, object, "card",
                    JS_NewUint32(context, g_qpk.card_color));
  JS_SetPropertyStr(context, object, "accent",
                    JS_NewUint32(context, g_qpk.accent_color));
  JS_SetPropertyStr(context, global, "ui", object);

  object = JS_NewObject(context);
  JS_SetPropertyStr(context, object, "showToast",
                    JS_NewCFunction(context, js_prompt_toast,
                                    "showToast", 1));
  JS_SetPropertyStr(context, object, "dialog",
                    JS_NewCFunction(context, js_prompt_dialog,
                                    "dialog", 1));
  JS_SetPropertyStr(context, object, "input",
                    JS_NewCFunction(context, js_prompt_input,
                                    "input", 2));
  JS_SetPropertyStr(context, global, "prompt", object);

  object = JS_NewObject(context);
  JS_SetPropertyStr(context, object, "get",
                    JS_NewCFunction(context, js_storage_get, "get", 1));
  JS_SetPropertyStr(context, object, "set",
                    JS_NewCFunction(context, js_storage_set, "set", 2));
  JS_SetPropertyStr(context, object, "delete",
                    JS_NewCFunction(context, js_storage_delete,
                                    "delete", 1));
  {
    JSValue system = JS_NewObject(context);
    JSValue camera = JS_NewObject(context);
    JSValue homeassistant = JS_NewObject(context);
    JSValue desktop = JS_NewObject(context);

    qpk_dl_install(context, system);
    qpk_hw_install(context, system);

    JS_SetPropertyStr(context, desktop, "publish",
                      JS_NewCFunction(context, js_desktop_publish,
                                      "publish", 5));
    JS_SetPropertyStr(context, system, "desktop", desktop);

    JS_SetPropertyStr(context, camera, "devices", JS_NewCFunction(context, js_camera_devices, "devices", 0));
    JS_SetPropertyStr(context, camera, "modes", JS_NewCFunction(context, js_camera_modes, "modes", 2));
    JS_SetPropertyStr(context, camera, "startDevice", JS_NewCFunction(context, js_camera_start_device, "startDevice", 4));
    JS_SetPropertyStr(context, camera, "status", JS_NewCFunction(context, js_camera_status, "status", 0));
    JS_SetPropertyStr(context, camera, "start",
                      JS_NewCFunction(context, js_camera_start,
                                      "start", 2));
    JS_SetPropertyStr(context, camera, "capture",
                      JS_NewCFunction(context, js_camera_capture, "capture", 0));
    JS_SetPropertyStr(context, camera, "active",
                      JS_NewCFunction(context, js_camera_active, "active", 0));
    JS_SetPropertyStr(context, camera, "photoStatus",
                      JS_NewCFunction(context, js_camera_photo_status,
                                      "photoStatus", 0));
    JS_SetPropertyStr(context, camera, "stop",
                      JS_NewCFunction(context, js_camera_stop,
                                      "stop", 0));
    JS_SetPropertyStr(context, camera, "blank",
                      JS_NewCFunction(context, js_camera_blank,
                                      "blank", 2));
    JS_SetPropertyStr(context, camera, "frames",
                      JS_NewCFunction(context, js_camera_frames,
                                      "frames", 0));
    JS_SetPropertyStr(context, system, "storage", object);
    JS_SetPropertyStr(context, system, "camera", camera);
    JS_SetPropertyStr(context, system, "yield",
                      JS_NewCFunction(context, js_system_yield, "yield", 0));
    JS_SetPropertyStr(context, homeassistant, "getState",
                      JS_NewCFunction(context, js_ha_get_state,
                                      "getState", 3));
    JS_SetPropertyStr(context, homeassistant, "get",
                      JS_NewCFunction(context, js_ha_get, "get", 3));
    JS_SetPropertyStr(context, homeassistant, "callService",
                      JS_NewCFunction(context, js_ha_call_service,
                                      "callService", 5));
    JS_SetPropertyStr(context, homeassistant, "poll",
                      JS_NewCFunction(context, js_ha_poll, "poll", 0));
    JS_SetPropertyStr(context, system, "homeAssistant", homeassistant);
#ifdef CONFIG_SYSTEM_HASS
    if (g_qpk.hass_grants != 0 &&
        hass_qjs_install(context, system, g_qpk.hass_grants) < 0)
      {
        JSValue unavailable = JS_NewObject(context);
        JS_SetPropertyStr(context, unavailable, "apiVersion",
                          JS_NewInt32(context, 0));
        JS_SetPropertyStr(context, system, "homeAssistantService",
                          unavailable);
      }
#endif
    qpk_pet_bind(context, system);
    JS_SetPropertyStr(context, global, "system", system);
  }

  object = JS_NewObject(context);
  JS_SetPropertyStr(context, object, "getInfo",
                    JS_NewCFunction(context, js_app_get_info,
                                    "getInfo", 0));
  JS_SetPropertyStr(context, global, "app", object);

  object = JS_NewObject(context);
  JS_SetPropertyStr(context, object, "log",
                    JS_NewCFunction(context, js_console_log, "log", 1));
  JS_SetPropertyStr(context, global, "console", object);
  JS_SetPropertyStr(context, global, "setInterval",
                    JS_NewCFunction(context, js_set_interval,
                                    "setInterval", 2));
  JS_SetPropertyStr(context, global, "setTimeout",
                    JS_NewCFunction(context, js_set_timeout,
                                    "setTimeout", 2));
  JS_SetPropertyStr(context, global, "clearInterval",
                    JS_NewCFunction(context, js_clear_interval,
                                    "clearInterval", 1));
  JS_SetPropertyStr(context, global, "clearTimeout",
                    JS_NewCFunction(context, js_clear_interval,
                                    "clearTimeout", 1));
  JS_FreeValue(context, global);
}

int qpk_runtime_launch(lv_obj_t *root, const char *name,
                       const char *package, const char *version,
                       const char *filename, const char *source,
                       size_t source_len, qpk_font_cb_t font_cb,
                       qpk_message_cb_t toast_cb,
                       qpk_message_cb_t dialog_cb)
{
  JSValue result;
  uint32_t background;
  unsigned int brightness;

  if (root == NULL || source == NULL || source_len == 0)
    {
      return -EINVAL;
    }

  if (!qpk_launch_identity_valid(package, filename, sizeof(g_qpk.package)))
    return -EACCES;

  qpk_runtime_stop();
  g_qpk_last_error[0] = 0;
  memset(&g_qpk, 0, sizeof(g_qpk));
  g_qpk.root = root;
  g_qpk.font_cb = font_cb;
  g_qpk.toast_cb = toast_cb;
  g_qpk.dialog_cb = dialog_cb;
  g_qpk.next_timer_id = 100;
  g_qpk.input_callback = JS_UNDEFINED;
  background = lv_color_to_u32(lv_obj_get_style_bg_color(root,
                                                          LV_PART_MAIN));
  brightness = ((background >> 16) & 0xff) +
               ((background >> 8) & 0xff) + (background & 0xff);
  if (brightness > 384)
    {
      g_qpk.primary_color = 0x252d46;
      g_qpk.secondary_color = 0x616981;
      g_qpk.surface_color = 0xe9eafa;
      g_qpk.accent_color = 0x5860bf;
    }
  else
    {
      g_qpk.primary_color = 0xf4f2ff;
      g_qpk.secondary_color = 0xabb3cd;
      g_qpk.surface_color = 0x303752;
      g_qpk.accent_color = 0xb4a6ff;
    }
  g_qpk.card_color = background;
  strlcpy(g_qpk.name, name ? name : "Quick App", sizeof(g_qpk.name));
  strlcpy(g_qpk.package, package ? package : "", sizeof(g_qpk.package));
  g_qpk.ha_config_access = qpk_builtin_ha_origin(package, filename);
#ifdef CONFIG_SYSTEM_HASS
  g_qpk.hass_grants = hass_ui_grants(package, source, source_len);
#endif
  strlcpy(g_qpk.version, version ? version : "", sizeof(g_qpk.version));

  g_qpk.runtime = JS_NewRuntime();
  if (g_qpk.runtime == NULL)
    {
      return -ENOMEM;
    }

  JS_SetMemoryLimit(g_qpk.runtime, qpk_js_memory_budget());
  JS_SetMaxStackSize(g_qpk.runtime, QPK_STACK_LIMIT);
  JS_SetInterruptHandler(g_qpk.runtime, qpk_interrupt, &g_qpk);
  g_qpk.context = JS_NewContext(g_qpk.runtime);
  if (g_qpk.context == NULL)
    {
      qpk_runtime_stop();
      return -ENOMEM;
    }

  lv_obj_remove_flag(g_qpk.root, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(g_qpk.root, qpk_event_swiped,
                      LV_EVENT_GESTURE, NULL);
  /* Registered for every page; the handler returns unless the app asked for
   * touch updates through ui.onTouch(). */
  lv_obj_add_event_cb(g_qpk.root, qpk_event_touched, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(g_qpk.root, qpk_event_touched, LV_EVENT_PRESSING, NULL);
  lv_obj_add_event_cb(g_qpk.root, qpk_event_touched, LV_EVENT_RELEASED, NULL);
  lv_obj_add_event_cb(g_qpk.root, qpk_event_touched, LV_EVENT_PRESS_LOST, NULL);
  qpk_install_api(g_qpk.context);
  qpk_deadline_begin(QPK_EVAL_BUDGET);
  result = JS_Eval(g_qpk.context, source, source_len,
                   filename ? filename : "app.js", JS_EVAL_TYPE_GLOBAL);
  qpk_deadline_end();
  if (JS_IsException(result))
    {
      qpk_show_error("启动失败");
      JS_FreeValue(g_qpk.context, result);
      qpk_runtime_stop();
      return -ENOEXEC;
    }

  JS_FreeValue(g_qpk.context, result);
  qpk_deadline_begin(QPK_EVAL_BUDGET);
  qpk_run_jobs();
  qpk_deadline_end();
  printf("[qpk] started %s (%s %s) with QuickJS\n",
         g_qpk.name, g_qpk.package, g_qpk.version);
  return 0;
}

void qpk_runtime_stop(void)
{
  qpk_hw_release(g_qpk_hardware); g_qpk_hardware = NULL;
  int i;

  qpk_dl_ui_stop();
  qpk_camera_stop();
  qpk_ha_stop();

  if (g_qpk.root != NULL)
    {
      lv_obj_remove_event_cb(g_qpk.root, qpk_event_swiped);
      lv_obj_remove_event_cb(g_qpk.root, qpk_event_touched);
    }

  if (g_qpk.context != NULL)
    {
      /* Retired pages may outlive their JS runtime during a page handoff.
       * Remove their listeners before releasing callbacks or reusing slots.
       */
      for (i = 0; i < g_qpk.widget_capacity; i++)
        {
          if (g_qpk.widgets[i] != NULL)
            {
              lv_obj_remove_event_cb(g_qpk.widgets[i], qpk_widget_deleted);
            }
        }

      for (struct qpk_event_s *binding = g_qpk.events; binding; binding = binding->next) {
        if (binding->used && binding->owner) {
          lv_obj_remove_event_cb(binding->owner, qpk_event_clicked);
          lv_obj_remove_event_cb(binding->owner, qpk_event_deleted);
        }
      }

      if (g_qpk.input_shade != NULL)
        {
          lv_obj_delete(g_qpk.input_shade);
          g_qpk.input_shade = NULL;
          g_qpk.input_textarea = NULL;
        }

      if (!JS_IsUndefined(g_qpk.input_callback))
        {
          JS_FreeValue(g_qpk.context, g_qpk.input_callback);
          g_qpk.input_callback = JS_UNDEFINED;
        }

      if (g_qpk.touch_used)
        {
          JS_FreeValue(g_qpk.context, g_qpk.touch_event);
          g_qpk.touch_event = JS_UNDEFINED;
          g_qpk.touch_used = false;
        }

      for (struct qpk_timer_s *binding = g_qpk.timers; binding; binding = binding->next)
        if (binding->used) qpk_timer_release(g_qpk.context, binding);
      for (struct qpk_event_s *binding = g_qpk.events; binding; binding = binding->next)
        if (binding->used) qpk_event_release(g_qpk.context, binding);

      if (g_qpk.swipe_event.used)
        {
          JS_FreeValue(g_qpk.context, g_qpk.swipe_event.function);
        }

      JS_FreeContext(g_qpk.context);
    }

  if (g_qpk.runtime != NULL)
    {
      JS_FreeRuntime(g_qpk.runtime);
    }

  while (g_qpk.events) { struct qpk_event_s *next = g_qpk.events->next; free(g_qpk.events); g_qpk.events = next; }
  while (g_qpk.timers) { struct qpk_timer_s *next = g_qpk.timers->next; free(g_qpk.timers); g_qpk.timers = next; }
  for (i = 0; i < g_qpk.widget_capacity; i++) free(g_qpk.widget_extra[i]);
  free(g_qpk.widgets); free(g_qpk.widget_types); free(g_qpk.widget_extra);
  free(g_qpk.widget_generations);
  memset(&g_qpk, 0, sizeof(g_qpk));
}

const char *qpk_runtime_last_error(void) { return g_qpk_last_error; }

unsigned qpk_runtime_widget_count(void)
{
  unsigned count = 0;
  for (unsigned i = 0; i < g_qpk.widget_capacity; i++) if (g_qpk.widgets[i]) count++;
  return count;
}

bool qpk_runtime_camera_active(void)
{
  bool active;

  pthread_mutex_lock(&g_camera_lock);
  active = g_camera.direct_preview;
  pthread_mutex_unlock(&g_camera_lock);
  return active;
}

bool qpk_runtime_camera_busy(void)
{
  bool busy;

  pthread_mutex_lock(&g_camera_lock);
  busy = g_camera_state != QPK_CAMERA_IDLE;
  pthread_mutex_unlock(&g_camera_lock);
  return busy;
}

bool qpk_runtime_camera_poll(void)
{
  static bool armed;
  static int pressed;
  static lv_point_t point;
  bool failed;
  bool retry;
  bool redraw;
  int error;

  pthread_mutex_lock(&g_camera_lock);
  failed = g_camera_state == QPK_CAMERA_ACTIVE &&
           g_camera.thread_running && !g_camera.thread_alive &&
           !g_camera.stop_requested;
  error = g_camera.thread_error;
  retry = g_camera_state == QPK_CAMERA_FAILED &&
          !g_camera_cleanup_blocked && qpk_now_ms() >= g_camera_retry_at;
  redraw = g_camera_redraw;
  g_camera_redraw = false;
  pthread_mutex_unlock(&g_camera_lock);

  if (failed)
    {
      g_camera_last_error = error;
      printf("[qpk] camera failure awaiting cleanup: %d\n", error);
    }

  if (failed || retry)
    {
      qpk_camera_stop();
    }

  /* Read the pointer driver without dispatching LVGL events or rendering.
   * Require release after entry and cancel drags outside the original target. */
  if (!qpk_runtime_camera_active())
    {
      armed = false;
      pressed = 0;
    }
  else
    {
      lv_indev_t *indev = NULL;
      while ((indev = lv_indev_get_next(indev)) != NULL)
        {
          if (lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) continue;
          lv_indev_read_cb_t read_cb = lv_indev_get_read_cb(indev);
          if (read_cb == NULL) break;
          for (int i = 0; i < 16; i++)
            {
              lv_indev_data_t data = {0};
              data.point = point;
              read_cb(indev, &data);
              point = data.point;
              int hit = point.y >= 500 && point.y < 600 ?
                (point.x >= 462 && point.x < 562 ? 1 :
                 point.x >= 910 && point.x < 1010 ? 2 :
                 point.x >= 25 && point.x < 125 ? 3 : 0) : 0;
              if (!armed)
                {
                  if (data.state == LV_INDEV_STATE_RELEASED) armed = true;
                }
              else if (data.state == LV_INDEV_STATE_PRESSED)
                {
                  if (pressed == 0) pressed = hit ? hit : -1;
                  else if (pressed != hit) pressed = -1;
                }
              else
                {
                  int action = pressed == hit ? hit : 0;
                  pressed = 0;
                  if (action == 1) qpk_runtime_camera_capture();
                  if (action == 2 || action == 3)
                    {
                      qpk_camera_stop();
                      armed = false;
                      break;
                    }
                }
              if (!data.continue_reading) break;
            }
          break;
        }
    }

  return redraw;
}

bool qpk_runtime_running(void)
{
  return g_qpk.context != NULL;
}
