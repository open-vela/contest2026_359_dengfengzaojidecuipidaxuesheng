#include "../overlay/apps/system/desktop/pet_engine.h"

#include <stdio.h>
#include <string.h>
#include <limits.h>

static int g_failed;

static void expect(int ok, const char *name)
{
  if (!ok)
    {
      fprintf(stderr, "FAIL %s\n", name);
      g_failed++;
    }
}

static int fake_weather(char *buf, size_t size)
{
  if (buf == NULL || size == 0)
    {
      return -1;
    }

  snprintf(buf, size, "汕头今天26°，多云");
  return 0;
}

int main(void)
{
  struct pet_state_s state;
  int start_x;
  int start_y;
  int i;

  pet_engine_init(1024, 600);
  pet_engine_set_rng(1);
  pet_engine_get(&state);
  expect(strcmp(state.mode_id, "wander") == 0, "init wander");
  expect(state.visible, "init visible");
  expect(state.w == 96, "init medium width");
  expect(state.mood == 72, "init mood");

  pet_engine_poke();
  pet_engine_get(&state);
  expect(state.jump_t > 0, "poke jumps");
  expect(state.bubble[0] != '\0', "poke bubble");
  expect(strcmp(state.last_event, "poke") == 0, "poke event");

  pet_engine_feed("fish");
  pet_engine_get(&state);
  expect(strstr(state.bubble, "小鱼干") != NULL, "feed fish");
  expect(state.hunger < 28, "feed lowers hunger");

  pet_engine_feed("fries");
  pet_engine_get(&state);
  expect(strstr(state.bubble, "薯条") != NULL, "feed fries");
  pet_engine_chat("我的薯条呢");
  pet_engine_get(&state);
  expect(strstr(state.last_reply, "薯条") != NULL, "chat fries");

  pet_engine_feed("cake");
  pet_engine_get(&state);
  expect(strstr(state.bubble, "蛋糕") != NULL, "feed cake");

  pet_engine_say("番茄完成啦", false);
  pet_engine_get(&state);
  expect(strcmp(state.bubble, "番茄完成啦") == 0, "say text");
  pet_engine_say("番茄完成啦", false);
  pet_engine_get(&state);
  expect(state.bubble[0] != '\0', "repeat say keeps bubble");

  pet_engine_chat("你好呀");
  pet_engine_get(&state);
  expect(strstr(state.last_reply, "薯条") != NULL, "chat hello");
  expect(state.bubble[0] != '\0', "chat bubble");

  pet_engine_chat("今天天气怎么样");
  pet_engine_get(&state);
  expect(strstr(state.last_reply, "摸鱼") != NULL, "chat weather fallback");
  pet_engine_set_weather_cb(fake_weather);
  pet_engine_weather();
  pet_engine_get(&state);
  expect(strstr(state.bubble, "汕头") != NULL, "weather callback");

  pet_engine_set_mode(PET_STILL);
  pet_engine_get(&state);
  start_x = state.x;
  for (i = 0; i < 40; i++)
    {
      pet_engine_tick(50);
    }

  pet_engine_get(&state);
  expect(state.x == start_x, "still does not walk");
  expect(strcmp(state.mode_id, "still") == 0, "still mode id");

  pet_engine_set_mode(PET_WANDER);
  pet_engine_set_rng(7);
  start_x = state.x;
  start_y = state.y;
  for (i = 0; i < 80; i++)
    {
      pet_engine_tick(50);
    }

  pet_engine_get(&state);
  expect(state.x != start_x || state.y != start_y, "wander activity");

  pet_engine_init(1024, 600);
  pet_engine_set_rng(1);
  start_x = 0;
  pet_engine_get(&state);
  start_x = state.x;
  pet_engine_set_mode(PET_FOLLOW);
  for (i = 0; i < 80; i++)
    {
      pet_engine_tick(50);
    }

  pet_engine_get(&state);
  expect(strcmp(state.mode_id, "follow") == 0, "follow without point");
  expect(state.x < start_x, "follow defaults to center");

  pet_engine_set_follow(120, 200);
  pet_engine_get(&state);
  expect(strcmp(state.mode_id, "follow") == 0, "follow mode");
  start_x = state.x;
  for (i = 0; i < 80; i++)
    {
      pet_engine_tick(50);
    }

  pet_engine_get(&state);
  expect(state.x < start_x, "follow moves left");

  pet_engine_set_visible(false);
  pet_engine_get(&state);
  expect(!state.visible, "hidden");
  pet_engine_toggle_visible();
  pet_engine_get(&state);
  expect(state.visible, "shown again");

  pet_engine_set_size("small");
  pet_engine_get(&state);
  expect(state.w == 72, "small size");

  pet_engine_drag_begin(900, 500);
  pet_engine_drag_move(200, 180);
  pet_engine_get(&state);
  expect(state.dragging, "dragging");
  pet_engine_drag_end();
  pet_engine_get(&state);
  expect(!state.dragging, "drag end");
  expect(state.x <= 200, "drag moved");

  pet_engine_set_follow(INT_MIN, INT_MAX);
  for (i = 0; i < 100; i++) pet_engine_tick(INT_MAX);
  pet_engine_get(&state);
  expect(state.x >= 0 && state.x + state.w <= state.screen_w,
         "extreme follow x remains bounded");
  expect(state.y >= 0 && state.y + state.h <= state.screen_h,
         "extreme follow y remains bounded");
  expect(state.hunger >= 0 && state.hunger <= 100, "extreme dt remains bounded");

  if (g_failed)
    {
      fprintf(stderr, "%d failed\n", g_failed);
      return 1;
    }

  puts("ok");
  return 0;
}
