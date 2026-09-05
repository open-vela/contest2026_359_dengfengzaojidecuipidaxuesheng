/****************************************************************************
 * apps/system/desktop/pomodoro_engine.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#pragma once

#include <stdbool.h>
#include <stddef.h>

enum pomodoro_phase_e
{
  POMODORO_WORK = 0,
  POMODORO_SHORT,
  POMODORO_LONG
};

struct pomodoro_state_s
{
  enum pomodoro_phase_e phase;
  bool running;
  int remaining_ms;
  int work_min;
  int short_min;
  int long_min;
  int completed;
  unsigned event_seq;
  char clock[16];
  char label[16];
  char toast[64];
  char dialog[64];
};

void pomodoro_engine_init(void);
void pomodoro_engine_get(struct pomodoro_state_s *out);
void pomodoro_engine_tick(void);
void pomodoro_engine_start(void);
void pomodoro_engine_pause(void);
void pomodoro_engine_toggle(void);
void pomodoro_engine_skip(void);
void pomodoro_engine_reset(void);
void pomodoro_engine_add_work(int delta);
void pomodoro_engine_add_break(int delta);
void pomodoro_engine_scene_work(void);
void pomodoro_engine_nudge(int ms);
void pomodoro_engine_format_chip(char *buf, size_t size);
