/****************************************************************************
 * apps/system/desktop/pomodoro_lvgl.c
 *
 * LVGL replica of camera-app/pomodoro/preview.html
 * LVGL 9.2.2 has no rotateX/rotateY; split-flap and hour fold are 2.5D
 * clip + height/width timers matching the CSS 0.32s / 0.5s timings.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include "pomodoro_lvgl.h"
#include "pomodoro_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIGIT_N       6
#define DIGIT_W       104
#define DIGIT_H       168
#define DIGIT_HALF    84
#define FOLD_W        18
#define COLON_W       24
#define PAIR_GAP      8
#define ROW_GAP       10
#define HOURS_INNER   (DIGIT_W * 2 + PAIR_GAP)
#define PACK_ON       (HOURS_INNER + PAIR_GAP + COLON_W)
#define PACK_OFF      FOLD_W
#define PAPER_BG      0xf3efe8
#define PAPER_CARD    0xfaf8f4
#define PAPER_INK     0x2a2724
#define PAPER_MUTED   0x8a847c
#define PAPER_HINT    0xb5aea4
#define PAPER_FOLD    0xe4dac8
#define PAPER_LIP     0xefe6d6
#define TICK_MS       16
#define FLIP_T1       20
#define FLIP_DELAY    10
#define FLIP_T2       32
#define FOLD_TICKS    31
#define HINT_MS       8000

struct flip_digit_s
{
  lv_obj_t *board;
  lv_obj_t *top_lab;
  lv_obj_t *bot_lab;
  lv_obj_t *drop;
  lv_obj_t *drop_lab;
  lv_obj_t *rise;
  lv_obj_t *rise_lab;
  lv_timer_t *anim;
  char curr;
  char pending;
  int tick;
  bool busy;
  bool up;
  int place_ms;
  int press_x;
  int press_y;
};

static struct
{
  lv_obj_t *root;
  lv_obj_t *phase;
  lv_obj_t *hint;
  lv_obj_t *pack;
  lv_obj_t *fold;
  lv_obj_t *hours_wrap;
  lv_obj_t *hour_colon;
  struct flip_digit_s digits[DIGIT_N];
  pomodoro_lvgl_close_cb_t close_cb;
  const lv_font_t *font_zh;
  lv_timer_t *fold_anim;
  lv_timer_t *hint_timer;
  int last_ms;
  int fold_tick;
  int pack_from;
  int pack_to;
  bool hours_on;
  bool hint_gone;
  bool open;
} g_ui;

static const int g_places[DIGIT_N] =
{
  10 * 3600000, 3600000,
  10 * 60000, 60000,
  10 * 1000, 1000
};

static void digit_set_text(lv_obj_t *lab, char ch)
{
  char buf[2];

  buf[0] = ch;
  buf[1] = '\0';
  lv_label_set_text(lab, buf);
}

static void style_clip(lv_obj_t *clip, int y, int h)
{
  lv_obj_set_pos(clip, 0, y);
  lv_obj_set_size(clip, DIGIT_W, h);
  lv_obj_set_style_bg_opa(clip, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(clip, 0, 0);
  lv_obj_set_style_pad_all(clip, 0, 0);
  lv_obj_set_style_radius(clip, 0, 0);
  lv_obj_remove_flag(clip, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(clip, LV_OBJ_FLAG_CLICKABLE);
}

static lv_obj_t *make_digit_label(lv_obj_t *parent, int y)
{
  lv_obj_t *lab = lv_label_create(parent);

  lv_obj_set_pos(lab, 0, y);
  lv_obj_set_size(lab, DIGIT_W, DIGIT_H);
  lv_obj_set_style_text_font(lab, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(lab, lv_color_hex(PAPER_INK), 0);
  lv_obj_set_style_text_align(lab, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_pad_top(lab, (DIGIT_H - 48) / 2, 0);
  lv_obj_remove_flag(lab, LV_OBJ_FLAG_CLICKABLE);
  lv_label_set_text(lab, "0");
  return lab;
}

static lv_obj_t *make_leaf(lv_obj_t *parent)
{
  lv_obj_t *leaf = lv_obj_create(parent);

  style_clip(leaf, 0, DIGIT_HALF);
  lv_obj_set_style_bg_color(leaf, lv_color_hex(PAPER_CARD), 0);
  lv_obj_set_style_bg_opa(leaf, LV_OPA_COVER, 0);
  lv_obj_add_flag(leaf, LV_OBJ_FLAG_HIDDEN);
  return leaf;
}

static void snap_digit(struct flip_digit_s *d, char ch)
{
  d->curr = ch;
  d->pending = ch;
  d->busy = false;
  d->tick = 0;
  digit_set_text(d->top_lab, ch);
  digit_set_text(d->bot_lab, ch);
  lv_obj_add_flag(d->drop, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(d->rise, LV_OBJ_FLAG_HIDDEN);
  if (d->anim != NULL)
    {
      lv_timer_pause(d->anim);
    }
}

static void hide_hint(void)
{
  if (g_ui.hint == NULL || g_ui.hint_gone)
    {
      return;
    }

  g_ui.hint_gone = true;
  lv_obj_set_style_opa(g_ui.hint, LV_OPA_TRANSP, 0);
  if (g_ui.hint_timer != NULL)
    {
      lv_timer_pause(g_ui.hint_timer);
    }
}

static void remaining_digits(int ms, bool hours_on, char out[DIGIT_N])
{
  int total;
  int h;
  int m;
  int s;

  total = ms <= 0 ? 0 : (ms + 999) / 1000;
  h = total / 3600;
  m = hours_on ? (total / 60) % 60 : total / 60;
  s = total % 60;
  out[0] = (char)('0' + (h / 10) % 10);
  out[1] = (char)('0' + h % 10);
  out[2] = (char)('0' + (m / 10) % 10);
  out[3] = (char)('0' + m % 10);
  out[4] = (char)('0' + (s / 10) % 10);
  out[5] = (char)('0' + s % 10);
}

static int flap_h(int start, int end, int tick)
{
  if (tick <= start)
    {
      return 0;
    }

  if (tick >= end)
    {
      return DIGIT_HALF;
    }

  return DIGIT_HALF * (tick - start) / (end - start);
}

static void apply_flap(struct flip_digit_s *d, int drop_h, int rise_h)
{
  lv_obj_set_pos(d->drop, 0, DIGIT_HALF - drop_h);
  lv_obj_set_size(d->drop, DIGIT_W, drop_h);
  lv_obj_set_y(d->drop_lab, -(DIGIT_HALF - drop_h));
  lv_obj_set_pos(d->rise, 0, DIGIT_HALF);
  lv_obj_set_size(d->rise, DIGIT_W, rise_h);
  lv_obj_set_y(d->rise_lab, -DIGIT_HALF);
}

static void flip_timer_cb(lv_timer_t *timer)
{
  struct flip_digit_s *d = lv_timer_get_user_data(timer);
  int drop_h;
  int rise_h;

  d->tick++;
  if (d->up)
    {
      drop_h = flap_h(FLIP_DELAY, FLIP_T2, d->tick);
      rise_h = DIGIT_HALF - flap_h(0, FLIP_T1, d->tick);
    }
  else
    {
      drop_h = DIGIT_HALF - flap_h(0, FLIP_T1, d->tick);
      rise_h = flap_h(FLIP_DELAY, FLIP_T2, d->tick);
    }

  apply_flap(d, drop_h, rise_h);
  if (d->tick < FLIP_T2)
    {
      return;
    }

  snap_digit(d, d->pending);
}

static void flip_to(struct flip_digit_s *d, char next, bool up)
{
  if (d->curr == next)
    {
      return;
    }

  if (d->busy || d->curr == '\0')
    {
      snap_digit(d, next);
      return;
    }

  d->pending = next;
  d->up = up;
  d->busy = true;
  d->tick = 0;
  if (up)
    {
      digit_set_text(d->drop_lab, next);
      digit_set_text(d->rise_lab, d->curr);
      digit_set_text(d->top_lab, d->curr);
      digit_set_text(d->bot_lab, next);
      apply_flap(d, 0, DIGIT_HALF);
    }
  else
    {
      digit_set_text(d->drop_lab, d->curr);
      digit_set_text(d->rise_lab, next);
      digit_set_text(d->top_lab, next);
      digit_set_text(d->bot_lab, d->curr);
      apply_flap(d, DIGIT_HALF, 0);
    }

  lv_obj_remove_flag(d->drop, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(d->rise, LV_OBJ_FLAG_HIDDEN);
  lv_timer_resume(d->anim);
}

static void phase_text(char *buf, size_t n)
{
  struct pomodoro_state_s state;

  pomodoro_engine_get(&state);
  if (state.running && state.phase == POMODORO_WORK)
    {
      snprintf(buf, n, "专注");
    }
  else if (state.running && state.phase == POMODORO_SHORT)
    {
      snprintf(buf, n, "休息");
    }
  else if (state.running && state.phase == POMODORO_LONG)
    {
      snprintf(buf, n, "长休");
    }
  else
    {
      snprintf(buf, n, "准备");
    }
}

static int ease(int from, int to, int t, int n)
{
  int p2;
  int num;
  int den;

  if (t <= 0)
    {
      return from;
    }

  if (t >= n)
    {
      return to;
    }

  p2 = t * t;
  num = 3 * p2 * n - 2 * p2 * t;
  den = n * n * n;
  return from + (int)((long)(to - from) * num / den);
}

static void set_hour_clickable(bool on)
{
  int i;

  for (i = 0; i < 2; i++)
    {
      if (g_ui.digits[i].board == NULL)
        {
          continue;
        }

      if (on)
        {
          lv_obj_add_flag(g_ui.digits[i].board, LV_OBJ_FLAG_CLICKABLE);
        }
      else
        {
          lv_obj_remove_flag(g_ui.digits[i].board, LV_OBJ_FLAG_CLICKABLE);
        }
    }
}

static void fold_apply(int pack_w, int fold_w)
{
  int hours_x = fold_w > 0 ? fold_w + PAIR_GAP : 0;

  lv_obj_set_width(g_ui.pack, pack_w);
  lv_obj_set_width(g_ui.fold, fold_w);
  lv_obj_set_pos(g_ui.fold, 0, 0);
  lv_obj_set_pos(g_ui.hours_wrap, hours_x, 0);
  lv_obj_set_pos(g_ui.hour_colon, hours_x + HOURS_INNER + PAIR_GAP, 0);
  if (fold_w <= 0)
    {
      lv_obj_add_flag(g_ui.fold, LV_OBJ_FLAG_HIDDEN);
    }
  else
    {
      lv_obj_remove_flag(g_ui.fold, LV_OBJ_FLAG_HIDDEN);
    }
}

static void fold_timer_cb(lv_timer_t *timer)
{
  int fold_w;

  LV_UNUSED(timer);
  g_ui.fold_tick++;
  if (g_ui.hours_on)
    {
      fold_w = ease(FOLD_W, 0, g_ui.fold_tick, FOLD_TICKS);
    }
  else
    {
      fold_w = ease(0, FOLD_W, g_ui.fold_tick, FOLD_TICKS);
    }

  fold_apply(ease(g_ui.pack_from, g_ui.pack_to, g_ui.fold_tick, FOLD_TICKS),
             fold_w);
  if (g_ui.fold_tick >= FOLD_TICKS)
    {
      fold_apply(g_ui.pack_to, g_ui.hours_on ? 0 : FOLD_W);
      lv_timer_pause(g_ui.fold_anim);
    }
}

static void set_hours_on(bool on);

static void ui_refresh(bool instant)
{
  struct pomodoro_state_s state;
  char digits[DIGIT_N];
  char phase[12];
  int i;
  bool up;

  if (!g_ui.open)
    {
      return;
    }

  pomodoro_engine_get(&state);
  if (state.remaining_ms >= 3600000)
    {
      set_hours_on(true);
    }

  remaining_digits(state.remaining_ms, g_ui.hours_on, digits);
  up = !instant && state.remaining_ms > g_ui.last_ms;
  g_ui.last_ms = state.remaining_ms;
  for (i = 0; i < DIGIT_N; i++)
    {
      if (instant)
        {
          snap_digit(&g_ui.digits[i], digits[i]);
        }
      else
        {
          flip_to(&g_ui.digits[i], digits[i], up);
        }
    }

  phase_text(phase, sizeof(phase));
  lv_label_set_text(g_ui.phase, phase);
}

static void set_hours_on(bool on)
{
  if (g_ui.hours_on == on || g_ui.pack == NULL)
    {
      return;
    }

  g_ui.hours_on = on;
  g_ui.pack_from = lv_obj_get_width(g_ui.pack);
  g_ui.pack_to = on ? PACK_ON : PACK_OFF;
  g_ui.fold_tick = 0;
  set_hour_clickable(on);
  if (on)
    {
      lv_obj_remove_flag(g_ui.hour_colon, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(g_ui.hint, "上下滑动翻页调节时间");
      hide_hint();
    }
  else
    {
      lv_obj_add_flag(g_ui.hour_colon, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(g_ui.fold, LV_OBJ_FLAG_HIDDEN);
      if (!g_ui.hint_gone)
        {
          lv_label_set_text(g_ui.hint, "右滑添加小时  ·  上下翻页调时");
        }
    }

  if (g_ui.fold_anim != NULL)
    {
      lv_timer_resume(g_ui.fold_anim);
    }
  else
    {
      fold_apply(g_ui.pack_to, on ? 0 : FOLD_W);
    }

  ui_refresh(true);
}

static void phase_clicked(lv_event_t *e)
{
  LV_UNUSED(e);
  pomodoro_engine_toggle();
  ui_refresh(false);
}

static void fold_clicked(lv_event_t *e)
{
  struct pomodoro_state_s state;

  LV_UNUSED(e);
  pomodoro_engine_get(&state);
  if (!state.running)
    {
      set_hours_on(true);
    }
}

static void handle_gesture(struct flip_digit_s *d, int dx, int dy)
{
  struct pomodoro_state_s state;

  pomodoro_engine_get(&state);
  if (state.running)
    {
      pomodoro_engine_pause();
      ui_refresh(false);
      return;
    }

  if (dx >= 28 && abs(dx) >= abs(dy))
    {
      set_hours_on(true);
      return;
    }

  if (dx <= -28 && abs(dx) >= abs(dy))
    {
      if (state.remaining_ms < 3600000)
        {
          set_hours_on(false);
        }

      return;
    }

  if (dy <= -24)
    {
      pomodoro_engine_nudge(d->place_ms);
    }
  else if (dy >= 24)
    {
      pomodoro_engine_nudge(-d->place_ms);
    }
  else
    {
      pomodoro_engine_nudge(d->place_ms);
    }

  hide_hint();
  ui_refresh(false);
}

static void digit_pressed(lv_event_t *e)
{
  struct flip_digit_s *d = lv_event_get_user_data(e);
  lv_indev_t *indev = lv_event_get_indev(e);
  lv_point_t p;

  if (d == NULL || indev == NULL)
    {
      return;
    }

  lv_indev_get_point(indev, &p);
  d->press_x = p.x;
  d->press_y = p.y;
}

static void digit_released(lv_event_t *e)
{
  struct flip_digit_s *d = lv_event_get_user_data(e);
  lv_indev_t *indev = lv_event_get_indev(e);
  lv_point_t p;

  if (d == NULL || indev == NULL)
    {
      return;
    }

  lv_indev_get_point(indev, &p);
  handle_gesture(d, p.x - d->press_x, p.y - d->press_y);
}

static void back_clicked(lv_event_t *e)
{
  pomodoro_lvgl_close_cb_t cb = g_ui.close_cb;

  LV_UNUSED(e);
  pomodoro_lvgl_close();
  if (cb != NULL)
    {
      cb();
    }
}

static void hint_timeout_cb(lv_timer_t *timer)
{
  LV_UNUSED(timer);
  hide_hint();
  if (g_ui.hint_timer != NULL)
    {
      lv_timer_pause(g_ui.hint_timer);
    }
}

static lv_obj_t *make_pair(lv_obj_t *parent)
{
  lv_obj_t *pair = lv_obj_create(parent);

  lv_obj_set_size(pair, LV_SIZE_CONTENT, DIGIT_H);
  lv_obj_set_style_bg_opa(pair, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(pair, 0, 0);
  lv_obj_set_style_pad_all(pair, 0, 0);
  lv_obj_set_style_pad_column(pair, PAIR_GAP, 0);
  lv_obj_set_flex_flow(pair, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(pair, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_remove_flag(pair, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(pair, LV_OBJ_FLAG_CLICKABLE);
  return pair;
}

static lv_obj_t *create_board(lv_obj_t *parent, struct flip_digit_s *d,
                              int place)
{
  lv_obj_t *top_clip;
  lv_obj_t *bot_clip;
  lv_obj_t *hinge;

  memset(d, 0, sizeof(*d));
  d->place_ms = place;
  d->curr = '0';
  d->board = lv_obj_create(parent);
  lv_obj_set_size(d->board, DIGIT_W, DIGIT_H);
  lv_obj_set_style_bg_color(d->board, lv_color_hex(PAPER_CARD), 0);
  lv_obj_set_style_border_width(d->board, 0, 0);
  lv_obj_set_style_radius(d->board, 6, 0);
  lv_obj_set_style_pad_all(d->board, 0, 0);
  lv_obj_set_style_shadow_width(d->board, 40, 0);
  lv_obj_set_style_shadow_color(d->board, lv_color_hex(0x3c3020), 0);
  lv_obj_set_style_shadow_opa(d->board, LV_OPA_10, 0);
  lv_obj_set_style_shadow_offset_y(d->board, 18, 0);
  lv_obj_set_style_clip_corner(d->board, true, 0);
  lv_obj_remove_flag(d->board, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(d->board, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(d->board, digit_pressed, LV_EVENT_PRESSED, d);
  lv_obj_add_event_cb(d->board, digit_released, LV_EVENT_RELEASED, d);

  top_clip = lv_obj_create(d->board);
  style_clip(top_clip, 0, DIGIT_HALF);
  d->top_lab = make_digit_label(top_clip, 0);

  bot_clip = lv_obj_create(d->board);
  style_clip(bot_clip, DIGIT_HALF, DIGIT_HALF);
  d->bot_lab = make_digit_label(bot_clip, -DIGIT_HALF);

  d->drop = make_leaf(d->board);
  d->drop_lab = make_digit_label(d->drop, 0);
  d->rise = make_leaf(d->board);
  d->rise_lab = make_digit_label(d->rise, -DIGIT_HALF);

  hinge = lv_obj_create(d->board);
  lv_obj_set_size(hinge, DIGIT_W, 1);
  lv_obj_set_pos(hinge, 0, DIGIT_HALF);
  lv_obj_set_style_bg_color(hinge, lv_color_hex(0x2a2724), 0);
  lv_obj_set_style_bg_opa(hinge, LV_OPA_20, 0);
  lv_obj_set_style_border_width(hinge, 0, 0);
  lv_obj_set_style_radius(hinge, 0, 0);
  lv_obj_remove_flag(hinge, LV_OBJ_FLAG_CLICKABLE);

  d->anim = lv_timer_create(flip_timer_cb, TICK_MS, d);
  lv_timer_pause(d->anim);
  return d->board;
}

static lv_obj_t *make_colon(lv_obj_t *parent)
{
  lv_obj_t *colon = lv_label_create(parent);

  lv_label_set_text(colon, ":");
  lv_obj_set_width(colon, COLON_W);
  lv_obj_set_style_text_font(colon, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(colon, lv_color_hex(PAPER_INK), 0);
  lv_obj_set_style_text_align(colon, LV_TEXT_ALIGN_CENTER, 0);
  return colon;
}

static lv_obj_t *make_fold(lv_obj_t *parent)
{
  lv_obj_t *fold;
  lv_obj_t *crease;
  lv_obj_t *lip;

  fold = lv_obj_create(parent);
  lv_obj_set_size(fold, FOLD_W, DIGIT_H);
  lv_obj_set_style_bg_color(fold, lv_color_hex(PAPER_FOLD), 0);
  lv_obj_set_style_bg_grad_color(fold, lv_color_hex(0xd8ccb8), 0);
  lv_obj_set_style_bg_grad_dir(fold, LV_GRAD_DIR_HOR, 0);
  lv_obj_set_style_border_width(fold, 0, 0);
  lv_obj_set_style_radius(fold, 6, 0);
  lv_obj_set_style_pad_all(fold, 0, 0);
  lv_obj_set_style_shadow_width(fold, 16, 0);
  lv_obj_set_style_shadow_opa(fold, LV_OPA_10, 0);
  lv_obj_remove_flag(fold, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(fold, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(fold, fold_clicked, LV_EVENT_CLICKED, NULL);

  crease = lv_obj_create(fold);
  lv_obj_set_size(crease, 1, DIGIT_H - 20);
  lv_obj_align(crease, LV_ALIGN_RIGHT_MID, -5, 0);
  lv_obj_set_style_bg_color(crease, lv_color_hex(0x5a4830), 0);
  lv_obj_set_style_bg_opa(crease, LV_OPA_20, 0);
  lv_obj_set_style_border_width(crease, 0, 0);
  lv_obj_set_style_radius(crease, 0, 0);
  lv_obj_remove_flag(crease, LV_OBJ_FLAG_CLICKABLE);

  lip = lv_obj_create(fold);
  lv_obj_set_size(lip, 6, DIGIT_H);
  lv_obj_align(lip, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_set_style_bg_color(lip, lv_color_hex(PAPER_LIP), 0);
  lv_obj_set_style_border_width(lip, 0, 0);
  lv_obj_set_style_radius(lip, 0, 0);
  lv_obj_remove_flag(lip, LV_OBJ_FLAG_CLICKABLE);
  return fold;
}

int pomodoro_lvgl_create(lv_obj_t *parent, const lv_font_t *font_zh,
                         pomodoro_lvgl_close_cb_t close_cb)
{
  lv_obj_t *root;
  lv_obj_t *row;
  lv_obj_t *mins;
  lv_obj_t *secs;
  lv_obj_t *back;
  lv_obj_t *back_lab;

  pomodoro_lvgl_close();
  root = lv_obj_create(parent);
  if (root == NULL)
    {
      return -1;
    }

  memset(&g_ui, 0, sizeof(g_ui));
  g_ui.root = root;
  g_ui.close_cb = close_cb;
  g_ui.font_zh = font_zh;
  lv_obj_set_size(root, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(root, lv_color_hex(PAPER_BG), 0);
  lv_obj_set_style_border_width(root, 0, 0);
  lv_obj_set_style_pad_all(root, 0, 0);
  lv_obj_set_style_radius(root, 0, 0);
  lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

  back = lv_button_create(root);
  lv_obj_set_size(back, 52, 42);
  lv_obj_set_pos(back, 16, 16);
  lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(back, 0, 0);
  lv_obj_set_style_shadow_width(back, 0, 0);
  lv_obj_add_event_cb(back, back_clicked, LV_EVENT_CLICKED, NULL);
  back_lab = lv_label_create(back);
  lv_label_set_text(back_lab, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_color(back_lab, lv_color_hex(PAPER_INK), 0);
  lv_obj_set_style_text_font(back_lab, &lv_font_montserrat_24, 0);
  lv_obj_center(back_lab);

  g_ui.phase = lv_label_create(root);
  lv_label_set_text(g_ui.phase, "准备");
  lv_obj_set_style_text_color(g_ui.phase, lv_color_hex(PAPER_MUTED), 0);
  lv_obj_set_style_text_letter_space(g_ui.phase, 6, 0);
  if (font_zh != NULL)
    {
      lv_obj_set_style_text_font(g_ui.phase, font_zh, 0);
    }

  lv_obj_align(g_ui.phase, LV_ALIGN_TOP_MID, 0, 72);
  lv_obj_add_flag(g_ui.phase, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(g_ui.phase, phase_clicked, LV_EVENT_CLICKED, NULL);

  row = lv_obj_create(root);
  lv_obj_set_size(row, LV_SIZE_CONTENT, DIGIT_H);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 0, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, ROW_GAP, 0);
  lv_obj_align(row, LV_ALIGN_CENTER, 0, 20);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

  g_ui.pack = lv_obj_create(row);
  lv_obj_set_size(g_ui.pack, PACK_OFF, DIGIT_H);
  lv_obj_set_style_bg_opa(g_ui.pack, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(g_ui.pack, 0, 0);
  lv_obj_set_style_pad_all(g_ui.pack, 0, 0);
  lv_obj_set_style_radius(g_ui.pack, 0, 0);
  lv_obj_set_style_clip_corner(g_ui.pack, true, 0);
  lv_obj_remove_flag(g_ui.pack, LV_OBJ_FLAG_SCROLLABLE);

  g_ui.fold = make_fold(g_ui.pack);
  g_ui.hours_wrap = make_pair(g_ui.pack);
  lv_obj_set_size(g_ui.hours_wrap, HOURS_INNER, DIGIT_H);
  create_board(g_ui.hours_wrap, &g_ui.digits[0], g_places[0]);
  create_board(g_ui.hours_wrap, &g_ui.digits[1], g_places[1]);
  set_hour_clickable(false);

  g_ui.hour_colon = make_colon(g_ui.pack);
  lv_obj_add_flag(g_ui.hour_colon, LV_OBJ_FLAG_HIDDEN);
  fold_apply(PACK_OFF, FOLD_W);

  mins = make_pair(row);
  create_board(mins, &g_ui.digits[2], g_places[2]);
  create_board(mins, &g_ui.digits[3], g_places[3]);
  make_colon(row);
  secs = make_pair(row);
  create_board(secs, &g_ui.digits[4], g_places[4]);
  create_board(secs, &g_ui.digits[5], g_places[5]);

  g_ui.hint = lv_label_create(root);
  lv_label_set_text(g_ui.hint, "右滑添加小时  ·  上下翻页调时");
  lv_obj_set_style_text_color(g_ui.hint, lv_color_hex(PAPER_HINT), 0);
  lv_obj_set_style_text_letter_space(g_ui.hint, 3, 0);
  if (font_zh != NULL)
    {
      lv_obj_set_style_text_font(g_ui.hint, font_zh, 0);
    }

  lv_obj_align(g_ui.hint, LV_ALIGN_BOTTOM_MID, 0, -48);

  g_ui.fold_anim = lv_timer_create(fold_timer_cb, TICK_MS, NULL);
  lv_timer_pause(g_ui.fold_anim);
  g_ui.hint_timer = lv_timer_create(hint_timeout_cb, HINT_MS, NULL);

  g_ui.open = true;
  ui_refresh(true);
  return 0;
}

void pomodoro_lvgl_sync(void)
{
  ui_refresh(false);
}

void pomodoro_lvgl_close(void)
{
  int i;

  if (!g_ui.open)
    {
      return;
    }

  for (i = 0; i < DIGIT_N; i++)
    {
      if (g_ui.digits[i].anim != NULL)
        {
          lv_timer_delete(g_ui.digits[i].anim);
          g_ui.digits[i].anim = NULL;
        }
    }

  if (g_ui.fold_anim != NULL)
    {
      lv_timer_delete(g_ui.fold_anim);
      g_ui.fold_anim = NULL;
    }

  if (g_ui.hint_timer != NULL)
    {
      lv_timer_delete(g_ui.hint_timer);
      g_ui.hint_timer = NULL;
    }

  if (g_ui.root != NULL)
    {
      lv_obj_delete(g_ui.root);
    }

  memset(&g_ui, 0, sizeof(g_ui));
}

bool pomodoro_lvgl_is_open(void)
{
  return g_ui.open;
}
