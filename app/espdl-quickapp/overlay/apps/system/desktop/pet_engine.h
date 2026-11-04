/****************************************************************************
 * apps/system/desktop/pet_engine.h
 *
 * Native desktop pet.  Quick Apps drive it through system.pet.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "glass_seagull_motion.h"

enum pet_mode_e
{
  PET_WANDER = 0,
  PET_FOLLOW,
  PET_STILL
};

enum pet_dir_e
{
  PET_DOWN = 0,
  PET_UP,
  PET_LEFT,
  PET_RIGHT
};

struct pet_state_s
{
  enum pet_mode_e mode;
  enum pet_dir_e dir;
  int facing;
  int x;
  int y;
  int w;
  int h;
  int screen_w;
  int screen_h;
  int mood;
  int hunger;
  bool visible;
  bool dragging;
  int jump_t;
  int action_t;
  unsigned motion;
  unsigned motion_ms;
  unsigned event_seq;
  char mode_id[12];
  char dir_id[8];
  char size_id[12];
  char bubble[64];
  bool bubble_inner;
  char last_event[16];
  char last_reply[64];
  char toast[64];
};

void pet_engine_init(int screen_w, int screen_h);
void pet_engine_set_rng(unsigned seed);
void pet_engine_set_bounds(int screen_w, int screen_h);
void pet_engine_tick(int dt_ms);
void pet_engine_get(struct pet_state_s *out);
void pet_engine_set_mode(enum pet_mode_e mode);
/* Bind the current nest centre in desktop content coordinates. */
void pet_engine_set_home(int x,int y);
void pet_engine_set_visible(bool on);
void pet_engine_toggle_visible(void);
void pet_engine_set_size(const char *size_id);
void pet_engine_poke(void);
/* A gesture never overrides dragging or starts an invisible pet. */
void pet_engine_motion(unsigned motion);
void pet_engine_feed(const char *food);
void pet_engine_say(const char *text, bool inner);
void pet_engine_chat(const char *user_msg);
void pet_engine_weather(void);
typedef int (*pet_weather_fn)(char *buf, size_t size);
void pet_engine_set_weather_cb(pet_weather_fn fn);
void pet_engine_set_follow(int x, int y);
void pet_engine_drag_begin(int x, int y);
void pet_engine_drag_move(int x, int y);
void pet_engine_drag_end(void);
enum pet_mode_e pet_engine_parse_mode(const char *mode);
