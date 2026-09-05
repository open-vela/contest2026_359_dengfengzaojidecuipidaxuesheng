/****************************************************************************
 * apps/system/desktop/pomodoro_engine.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include "pomodoro_engine.h"

#include <stdio.h>
#include <string.h>

#define WORK_MIN   25
#define SHORT_MIN  5
#define LONG_MIN   15
#define ROUNDS     4

struct pomodoro_engine_s
{
  enum pomodoro_phase_e phase;
  bool running;
  int remaining_ms;
  int work_min;
  int short_min;
  int long_min;
  int completed;
  unsigned event_seq;
  char toast[64];
  char dialog[64];
};

static struct pomodoro_engine_s g_pomodoro;

static int clamp_int(int value, int min, int max)
{
  if (value < min)
    {
      return min;
    }

  if (value > max)
    {
      return max;
    }

  return value;
}

static int phase_duration_ms(void)
{
  if (g_pomodoro.phase == POMODORO_SHORT)
    {
      return g_pomodoro.short_min * 60000;
    }

  if (g_pomodoro.phase == POMODORO_LONG)
    {
      return g_pomodoro.long_min * 60000;
    }

  return g_pomodoro.work_min * 60000;
}

static void emit(const char *toast, const char *dialog)
{
  g_pomodoro.event_seq++;
  snprintf(g_pomodoro.toast, sizeof(g_pomodoro.toast), "%s",
           toast != NULL ? toast : "");
  snprintf(g_pomodoro.dialog, sizeof(g_pomodoro.dialog), "%s",
           dialog != NULL ? dialog : "");
}

static void complete_phase(void)
{
  g_pomodoro.running = false;
  if (g_pomodoro.phase == POMODORO_WORK)
    {
      g_pomodoro.completed++;
      if (g_pomodoro.completed % ROUNDS == 0)
        {
          g_pomodoro.phase = POMODORO_LONG;
          g_pomodoro.remaining_ms = g_pomodoro.long_min * 60000;
          emit("四个番茄完成，进入长休息", "工作完成，开始长休息。");
        }
      else
        {
          g_pomodoro.phase = POMODORO_SHORT;
          g_pomodoro.remaining_ms = g_pomodoro.short_min * 60000;
          emit("工作完成，开始短休息", "本轮工作结束，休息一下。");
        }
    }
  else
    {
      g_pomodoro.phase = POMODORO_WORK;
      g_pomodoro.remaining_ms = g_pomodoro.work_min * 60000;
      emit("休息结束，开始工作", "休息结束，开始下一个番茄。");
    }
}

static void format_clock(int remaining_ms, char *buf, size_t size)
{
  int total;
  int hours;
  int minutes;
  int seconds;

  total = remaining_ms <= 0 ? 0 : (remaining_ms + 999) / 1000;
  hours = total / 3600;
  minutes = (total / 60) % 60;
  seconds = total % 60;
  if (hours > 0)
    {
      snprintf(buf, size, "%02d:%02d:%02d", hours, minutes, seconds);
    }
  else
    {
      snprintf(buf, size, "%02d:%02d", total / 60, seconds);
    }
}

void pomodoro_engine_init(void)
{
  memset(&g_pomodoro, 0, sizeof(g_pomodoro));
  g_pomodoro.work_min = WORK_MIN;
  g_pomodoro.short_min = SHORT_MIN;
  g_pomodoro.long_min = LONG_MIN;
  g_pomodoro.phase = POMODORO_WORK;
  g_pomodoro.remaining_ms = WORK_MIN * 60000;
}

void pomodoro_engine_get(struct pomodoro_state_s *out)
{
  if (out == NULL)
    {
      return;
    }

  memset(out, 0, sizeof(*out));
  out->phase = g_pomodoro.phase;
  out->running = g_pomodoro.running;
  out->remaining_ms = g_pomodoro.remaining_ms;
  out->work_min = g_pomodoro.work_min;
  out->short_min = g_pomodoro.short_min;
  out->long_min = g_pomodoro.long_min;
  out->completed = g_pomodoro.completed;
  out->event_seq = g_pomodoro.event_seq;
  format_clock(g_pomodoro.remaining_ms, out->clock, sizeof(out->clock));
  if (g_pomodoro.phase == POMODORO_SHORT)
    {
      snprintf(out->label, sizeof(out->label), "%s", "短休息");
    }
  else if (g_pomodoro.phase == POMODORO_LONG)
    {
      snprintf(out->label, sizeof(out->label), "%s", "长休息");
    }
  else
    {
      snprintf(out->label, sizeof(out->label), "%s", "工作");
    }

  snprintf(out->toast, sizeof(out->toast), "%s", g_pomodoro.toast);
  snprintf(out->dialog, sizeof(out->dialog), "%s", g_pomodoro.dialog);
}

void pomodoro_engine_tick(void)
{
  if (!g_pomodoro.running)
    {
      return;
    }

  g_pomodoro.remaining_ms -= 1000;
  if (g_pomodoro.remaining_ms <= 0)
    {
      g_pomodoro.remaining_ms = 0;
      complete_phase();
    }
}

void pomodoro_engine_start(void)
{
  g_pomodoro.running = true;
}

void pomodoro_engine_pause(void)
{
  g_pomodoro.running = false;
}

void pomodoro_engine_toggle(void)
{
  g_pomodoro.running = !g_pomodoro.running;
}

void pomodoro_engine_skip(void)
{
  g_pomodoro.remaining_ms = 0;
  complete_phase();
}

void pomodoro_engine_reset(void)
{
  g_pomodoro.running = false;
  g_pomodoro.remaining_ms = phase_duration_ms();
}

void pomodoro_engine_add_work(int delta)
{
  if (g_pomodoro.running)
    {
      emit("请先暂停再调整时长", "");
      return;
    }

  g_pomodoro.work_min = clamp_int(g_pomodoro.work_min + delta, 1, 60);
  if (g_pomodoro.phase == POMODORO_WORK)
    {
      g_pomodoro.remaining_ms = g_pomodoro.work_min * 60000;
    }
}

void pomodoro_engine_add_break(int delta)
{
  if (g_pomodoro.running)
    {
      emit("请先暂停再调整时长", "");
      return;
    }

  g_pomodoro.short_min = clamp_int(g_pomodoro.short_min + delta, 1, 30);
  g_pomodoro.long_min = clamp_int(g_pomodoro.short_min * 3, 3, 45);
  if (g_pomodoro.phase == POMODORO_SHORT)
    {
      g_pomodoro.remaining_ms = g_pomodoro.short_min * 60000;
    }

  if (g_pomodoro.phase == POMODORO_LONG)
    {
      g_pomodoro.remaining_ms = g_pomodoro.long_min * 60000;
    }
}

void pomodoro_engine_scene_work(void)
{
  g_pomodoro.phase = POMODORO_WORK;
  g_pomodoro.remaining_ms = g_pomodoro.work_min * 60000;
  g_pomodoro.running = true;
}

void pomodoro_engine_nudge(int ms)
{
  if (g_pomodoro.running)
    {
      return;
    }

  g_pomodoro.remaining_ms = clamp_int(g_pomodoro.remaining_ms + ms,
                                      1000, 99 * 3600000 + 59 * 60000 + 59000);
}

void pomodoro_engine_format_chip(char *buf, size_t size)
{
  struct pomodoro_state_s state;
  const char *prefix;

  pomodoro_engine_get(&state);
  if (!state.running)
    {
      prefix = "暂停";
    }
  else if (state.phase == POMODORO_SHORT)
    {
      prefix = "短休";
    }
  else if (state.phase == POMODORO_LONG)
    {
      prefix = "长休";
    }
  else
    {
      prefix = "专注";
    }

  snprintf(buf, size, "%s %s", prefix, state.clock);
}
