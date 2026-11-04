/****************************************************************************
 * apps/system/desktop/pet_engine.c
 *
 * Desktop-pet state machine ported from dafeiyu-pet (MIT).
 * Dialogue is original seagull/fries humor; rendering is native LVGL.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include "pet_engine.h"

#include <stdio.h>
#include <string.h>

#define PET_W_SMALL   72
#define PET_H_SMALL   66
#define PET_W_MED     96
#define PET_H_MED     88
#define PET_W_LARGE   120
#define PET_H_LARGE   110
#define PET_SPEED     90
#define PET_TOP       56
#define PET_MARGIN    8
#define BUBBLE_MS     2800
#define REST_MIN_MS   8000
#define REST_SPAN_MS  10000

struct pet_engine_s
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
  int target_x;
  int target_y;
  int follow_x;
  int follow_y;
  int drag_off_x;
  int drag_off_y;
  int jump_t;
  int action_t;
  unsigned motion, motion_ms, last_motion;
  int motion_cool_ms;
  int rest_ms;
  int speak_cool_ms;
  int bubble_ms;
  int hunger_acc;
  int home_x,home_y;
  bool home_valid;
  bool has_target;
  bool has_follow;
  bool visible;
  bool dragging;
  bool bubble_inner;
  unsigned rng;
  unsigned event_seq;
  pet_weather_fn weather_cb;
  char size_id[12];
  char bubble[64];
  char last_line[64];
  char last_event[16];
  char last_reply[64];
  char toast[64];
};

static struct pet_engine_s g_pet;

static const char *const g_lines[] =
{
  "本鸥路过，顺便接管你的薯条。",
  "你负责工作，我负责薯条。",
  "飞得不高，饭量不小。",
  "没有偷吃，是薯条先靠近我的。",
  "海很大，我的胃也不小。",
  "表面在巡逻，实际在找薯条。",
  "我不是胖，是羽绒服比较蓬。",
  "你继续忙，我替你盯着午饭。",
  "嘎一下，今天也要脆脆的。",
  "工作可以慢慢来，薯条凉了不行。",
  "这片桌面，归本鸥巡逻。",
  "你的薯条很好，现在归鸥了。"
};

static const char *const g_react[] =
{
  "戳我可以，别戳我的薯条。",
  "嘎！有事请带薯条来谈。",
  "再戳一下，收一根薯条。",
  "本鸥很忙，忙着等投喂。",
  "摸摸可以，薯条不能撤回。",
  "刚才那一下，算你欠我薯条。",
  "别催，正在加载今日饭量。"
};

static const char *const g_inner[] =
{
  "他是不是又把薯条藏起来了。",
  "我就看看，绝对不叼走。",
  "薯条只有一包，鸥也只有一只。",
  "再忍一下，薯条就上桌了。",
  "今天的摸鱼指标已经完成。",
  "等他一走，我就嘎嘎开吃。"
};

static const char *const g_drag[] =
{
  "起飞可以，薯条跟上！",
  "轻点，本鸥还有一肚子薯条。",
  "空中运输，谢绝托运薯条。",
  "降落之前，先确认餐点。"
};

static unsigned xorshift(void)
{
  unsigned x = g_pet.rng ? g_pet.rng : 1u;

  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  g_pet.rng = x ? x : 1u;
  return g_pet.rng;
}

static int rand_range(int min, int span)
{
  if (span <= 0)
    {
      return min;
    }

  return min + (int)(xorshift() % (unsigned)span);
}

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

static void copy_text(char *dst, size_t size, const char *text)
{
  if (dst == NULL || size == 0)
    {
      return;
    }

  snprintf(dst, size, "%s", text != NULL ? text : "");
}

static const char *pick(const char *const *table, size_t count)
{
  return table[xorshift() % count];
}

static void apply_size(const char *size_id)
{
  if (size_id != NULL && strcmp(size_id, "small") == 0)
    {
      g_pet.w = PET_W_SMALL;
      g_pet.h = PET_H_SMALL;
      copy_text(g_pet.size_id, sizeof(g_pet.size_id), "small");
    }
  else if (size_id != NULL && strcmp(size_id, "large") == 0)
    {
      g_pet.w = PET_W_LARGE;
      g_pet.h = PET_H_LARGE;
      copy_text(g_pet.size_id, sizeof(g_pet.size_id), "large");
    }
  else
    {
      g_pet.w = PET_W_MED;
      g_pet.h = PET_H_MED;
      copy_text(g_pet.size_id, sizeof(g_pet.size_id), "medium");
    }
}

static void clamp_pos(void)
{
  int max_x;
  int max_y;

  max_x = g_pet.screen_w - g_pet.w - PET_MARGIN;
  max_y = g_pet.screen_h - g_pet.h - PET_MARGIN;
  if (max_x < PET_MARGIN)
    {
      max_x = PET_MARGIN;
    }

  if (max_y < PET_TOP)
    {
      max_y = PET_TOP;
    }

  g_pet.x = clamp_int(g_pet.x, PET_MARGIN, max_x);
  g_pet.y = clamp_int(g_pet.y, PET_TOP, max_y);
}

static void emit(const char *event, const char *toast)
{
  g_pet.event_seq++;
  copy_text(g_pet.last_event, sizeof(g_pet.last_event), event);
  copy_text(g_pet.toast, sizeof(g_pet.toast), toast);
}

static void say_raw(const char *text, bool inner)
{
  if (text == NULL || text[0] == '\0')
    {
      return;
    }

  if (strcmp(g_pet.last_line, text) == 0 && g_pet.bubble_ms > 0)
    {
      g_pet.bubble_ms = BUBBLE_MS;
      return;
    }

  copy_text(g_pet.last_line, sizeof(g_pet.last_line), text);
  copy_text(g_pet.bubble, sizeof(g_pet.bubble), text);
  g_pet.bubble_inner = inner;
  g_pet.bubble_ms = BUBBLE_MS;
  g_pet.speak_cool_ms = 2500;
}

static void set_dir_from_delta(int dx, int dy)
{
  int adx = dx < 0 ? -dx : dx;
  int ady = dy < 0 ? -dy : dy;

  if (adx > (ady * 115) / 100)
    {
      g_pet.dir = dx < 0 ? PET_LEFT : PET_RIGHT;
      g_pet.facing = dx < 0 ? 1 : -1;
    }
  else
    {
      g_pet.dir = dy < 0 ? PET_UP : PET_DOWN;
      g_pet.facing = 1;
    }
}

static void pick_wander_target(void)
{
  int max_x = g_pet.screen_w - g_pet.w - PET_MARGIN;
  int max_y = g_pet.screen_h - g_pet.h - PET_MARGIN;

  if (max_x < PET_MARGIN + 8)
    {
      max_x = PET_MARGIN + 8;
    }

  if (max_y < PET_TOP + 8)
    {
      max_y = PET_TOP + 8;
    }

  g_pet.target_x = rand_range(PET_MARGIN, max_x - PET_MARGIN);
  g_pet.target_y = rand_range(PET_TOP, max_y - PET_TOP);
  g_pet.has_target = true;
}

enum pet_mode_e pet_engine_parse_mode(const char *mode)
{
  if (mode != NULL && strcmp(mode, "follow") == 0)
    {
      return PET_FOLLOW;
    }

  if (mode != NULL && strcmp(mode, "still") == 0)
    {
      return PET_STILL;
    }

  return PET_WANDER;
}

void pet_engine_init(int screen_w, int screen_h)
{
  memset(&g_pet, 0, sizeof(g_pet));
  g_pet.rng = 0x9e3779b9u;
  g_pet.screen_w = screen_w > 0 ? screen_w : 1024;
  g_pet.screen_h = screen_h > 0 ? screen_h : 600;
  g_pet.mode = PET_WANDER;
  g_pet.dir = PET_DOWN;
  g_pet.facing = 1;
  g_pet.visible = true;
  g_pet.mood = 72;
  g_pet.hunger = 28;
  apply_size("medium");
  g_pet.x = g_pet.screen_w - g_pet.w - 48;
  g_pet.y = g_pet.screen_h - g_pet.h - 40;
  clamp_pos();
  g_pet.rest_ms = 1200;
}

void pet_engine_set_rng(unsigned seed)
{
  g_pet.rng = seed ? seed : 1u;
}

void pet_engine_set_bounds(int screen_w, int screen_h)
{
  if (screen_w > 0)
    {
      g_pet.screen_w = screen_w;
    }

  if (screen_h > 0)
    {
      g_pet.screen_h = screen_h;
    }

  clamp_pos();
}

/* Give aerial actions a small boost while preserving the existing repertoire.
 * Idle excludes the last gesture directly, without redirecting duplicates to
 * the next enum value (which would bias the distribution). */
static unsigned pick_motion(bool reaction, unsigned exclude)
{
  static const unsigned char idle_weights[SEAGULL_MOTION_COUNT] =
    {0,5,5,5,5,5,5,5,7,7,7};
  static const unsigned char touch_weights[SEAGULL_MOTION_COUNT] =
    {0,3,3,0,0,3,3,0,4,4,4};
  const unsigned char *weights = reaction ? touch_weights : idle_weights;
  unsigned total=0;
  for(unsigned i=1;i<SEAGULL_MOTION_COUNT;i++)if(i!=exclude)total+=weights[i];
  unsigned choice=xorshift()%total;
  for(unsigned i=1;i<SEAGULL_MOTION_COUNT;i++){
    if(i==exclude)continue;
    if(choice<weights[i])return i;
    choice-=weights[i];
  }
  return SEAGULL_TILT;
}

void pet_engine_motion(unsigned motion)
{
  if (!g_pet.visible || g_pet.dragging || motion == SEAGULL_IDLE ||
      motion >= SEAGULL_MOTION_COUNT) return;
  g_pet.motion = g_pet.last_motion = motion;
  g_pet.motion_ms = 0;
  g_pet.motion_cool_ms = 6000 + rand_range(0, 6000);
  g_pet.jump_t = 0;
  g_pet.dir = PET_DOWN;
}

void pet_engine_tick(int dt_ms)
{
  int step;
  int dx;
  int dy;
  int dist;
  int nx;
  int ny;

  if (dt_ms <= 0)
    {
      dt_ms = 16;
    }
  if (dt_ms > 1000) dt_ms = 1000;

  if (g_pet.jump_t > 0)
    {
      g_pet.jump_t = g_pet.jump_t > dt_ms ? g_pet.jump_t - dt_ms : 0;
    }

  if (g_pet.action_t > 0)
    {
      g_pet.action_t = g_pet.action_t > dt_ms ? g_pet.action_t - dt_ms : 0;
    }

  if (g_pet.speak_cool_ms > 0)
    {
      g_pet.speak_cool_ms = g_pet.speak_cool_ms > dt_ms ?
                            g_pet.speak_cool_ms - dt_ms : 0;
    }

  if (g_pet.bubble_ms > 0)
    {
      g_pet.bubble_ms = g_pet.bubble_ms > dt_ms ?
                        g_pet.bubble_ms - dt_ms : 0;
      if (g_pet.bubble_ms == 0)
        {
          g_pet.bubble[0] = '\0';
          g_pet.bubble_inner = false;
        }
    }

  g_pet.hunger_acc += dt_ms;
  if (g_pet.hunger_acc >= 8000)
    {
      g_pet.hunger_acc = 0;
      g_pet.hunger = clamp_int(g_pet.hunger + 1, 0, 100);
      if (g_pet.mood > 40)
        {
          g_pet.mood--;
        }
    }

  if (!g_pet.visible || g_pet.dragging)
    {
      return;
    }

  if (g_pet.motion != SEAGULL_IDLE)
    {
      g_pet.motion_ms += dt_ms;
      if (g_pet.motion_ms >= SEAGULL_MOTION_MS)
        {
          g_pet.motion = SEAGULL_IDLE;
          g_pet.motion_ms = 0;
        }
      return;
    }
  if (g_pet.motion_cool_ms > 0)
    g_pet.motion_cool_ms = g_pet.motion_cool_ms > dt_ms ?
                          g_pet.motion_cool_ms - dt_ms : 0;
  if (g_pet.motion_cool_ms == 0 &&
      (g_pet.mode == PET_STILL || (!g_pet.has_target && g_pet.rest_ms > 0)))
    {
      unsigned motion = pick_motion(false,g_pet.last_motion);
      pet_engine_motion(motion);
      return;
    }

  if (g_pet.mode == PET_STILL)
    {
      if (g_pet.speak_cool_ms == 0 && (xorshift() % 200) == 0)
        {
          say_raw(pick(g_inner, sizeof(g_inner) / sizeof(g_inner[0])), true);
        }

      return;
    }

  if (g_pet.mode == PET_FOLLOW && g_pet.has_follow)
    {
      g_pet.target_x = g_pet.follow_x - g_pet.w / 2;
      g_pet.target_y = g_pet.follow_y - g_pet.h / 2;
      g_pet.has_target = true;
    }
  else if (g_pet.mode == PET_WANDER && !g_pet.has_target)
    {
      if (g_pet.rest_ms > 0)
        {
          g_pet.rest_ms = g_pet.rest_ms > dt_ms ? g_pet.rest_ms - dt_ms : 0;
          if (g_pet.speak_cool_ms == 0 && (xorshift() % 180) == 0)
            {
              if ((xorshift() & 1u) != 0)
                {
                  say_raw(pick(g_inner, sizeof(g_inner) / sizeof(g_inner[0])),
                          true);
                }
              else
                {
                  say_raw(pick(g_lines, sizeof(g_lines) / sizeof(g_lines[0])),
                          false);
                }
            }

          return;
        }

      pick_wander_target();
    }

  if (!g_pet.has_target)
    {
      return;
    }

  dx = g_pet.target_x - g_pet.x;
  dy = g_pet.target_y - g_pet.y;
  dist = dx * dx + dy * dy;
  if (dist < 100)
    {
      g_pet.has_target = false;
      g_pet.dir = PET_DOWN;
      g_pet.facing = 1;
      g_pet.rest_ms = REST_MIN_MS + rand_range(0, REST_SPAN_MS);
      return;
    }

  step = PET_SPEED * dt_ms / 1000;
  if (step < 1)
    {
      step = 1;
    }

  nx = g_pet.x;
  ny = g_pet.y;
  if (dx * dx >= dy * dy)
    {
      nx += dx > 0 ? step : -step;
    }
  else
    {
      ny += dy > 0 ? step : -step;
    }

  g_pet.x = nx;
  g_pet.y = ny;
  clamp_pos();
  set_dir_from_delta(dx, dy);
  if ((xorshift() % 400) == 0)
    {
      g_pet.jump_t = 400;
    }
}

void pet_engine_get(struct pet_state_s *out)
{
  if (out == NULL)
    {
      return;
    }

  memset(out, 0, sizeof(*out));
  out->mode = g_pet.mode;
  out->dir = g_pet.dir;
  out->facing = g_pet.facing;
  out->x = g_pet.x;
  out->y = g_pet.y;
  out->w = g_pet.w;
  out->h = g_pet.h;
  out->screen_w = g_pet.screen_w;
  out->screen_h = g_pet.screen_h;
  out->mood = g_pet.mood;
  out->hunger = g_pet.hunger;
  out->visible = g_pet.visible;
  out->dragging = g_pet.dragging;
  out->jump_t = g_pet.jump_t;
  out->action_t = g_pet.action_t;
  out->motion = g_pet.motion;
  out->motion_ms = g_pet.motion_ms;
  out->event_seq = g_pet.event_seq;
  out->bubble_inner = g_pet.bubble_inner;
  copy_text(out->size_id, sizeof(out->size_id), g_pet.size_id);
  copy_text(out->bubble, sizeof(out->bubble), g_pet.bubble);
  copy_text(out->last_event, sizeof(out->last_event), g_pet.last_event);
  copy_text(out->last_reply, sizeof(out->last_reply), g_pet.last_reply);
  copy_text(out->toast, sizeof(out->toast), g_pet.toast);
  if (g_pet.mode == PET_FOLLOW)
    {
      copy_text(out->mode_id, sizeof(out->mode_id), "follow");
    }
  else if (g_pet.mode == PET_STILL)
    {
      copy_text(out->mode_id, sizeof(out->mode_id), "still");
    }
  else
    {
      copy_text(out->mode_id, sizeof(out->mode_id), "wander");
    }

  if (g_pet.dir == PET_UP)
    {
      copy_text(out->dir_id, sizeof(out->dir_id), "up");
    }
  else if (g_pet.dir == PET_LEFT)
    {
      copy_text(out->dir_id, sizeof(out->dir_id), "left");
    }
  else if (g_pet.dir == PET_RIGHT)
    {
      copy_text(out->dir_id, sizeof(out->dir_id), "right");
    }
  else
    {
      copy_text(out->dir_id, sizeof(out->dir_id), "down");
    }
}

void pet_engine_set_mode(enum pet_mode_e mode)
{
  g_pet.motion = SEAGULL_IDLE;
  g_pet.motion_ms = 0;
  g_pet.mode = mode;
  g_pet.has_target = false;
  g_pet.rest_ms = 0;
  if (mode == PET_FOLLOW && !g_pet.has_follow)
    {
      g_pet.follow_x = g_pet.screen_w / 2;
      g_pet.follow_y = g_pet.screen_h / 2;
      g_pet.has_follow = true;
    }

  emit("mode", mode == PET_FOLLOW ? "跟着你" :
       (mode == PET_STILL ? "原地待着" : "自由散步"));
}

void pet_engine_set_home(int x,int y)
{
  g_pet.home_x=x;g_pet.home_y=y;g_pet.home_valid=true;
}

void pet_engine_set_visible(bool on)
{
  bool entering = on && !g_pet.visible;
  if(on&&!g_pet.visible&&g_pet.home_valid){
    g_pet.x=g_pet.home_x-g_pet.w/2;g_pet.y=g_pet.home_y-g_pet.h/2;
    g_pet.has_target=false;g_pet.dragging=false;g_pet.jump_t=360;
    g_pet.rest_ms=0;clamp_pos();
  }
  g_pet.visible = on;
  if (entering) pet_engine_motion(SEAGULL_FLY);
  if (!on) {g_pet.motion=SEAGULL_IDLE;g_pet.motion_ms=0;}
  emit(on ? "show" : "hide", on ? "我回来了" : "我隐身了");
}

void pet_engine_toggle_visible(void)
{
  pet_engine_set_visible(!g_pet.visible);
}

void pet_engine_set_size(const char *size_id)
{
  apply_size(size_id);
  clamp_pos();
  emit("size", g_pet.size_id);
}

void pet_engine_poke(void)
{
  pet_engine_motion(pick_motion(true,SEAGULL_IDLE));
  g_pet.mood = clamp_int(g_pet.mood + 3, 0, 100);
  say_raw(pick(g_react, sizeof(g_react) / sizeof(g_react[0])), false);
  emit("poke", g_pet.bubble);
}

void pet_engine_feed(const char *food)
{
  const char *line = "好吃！";

  if (food != NULL && (strstr(food, "fries") != NULL || strstr(food, "薯条") != NULL))
    {
      line = "嘎！这包薯条已被本鸥接管。";
    }
  else if (food != NULL && (strstr(food, "cake") != NULL ||
                       strstr(food, "蛋糕") != NULL))
    {
      line = "蛋糕！罪恶但快乐……";
    }
  else if (food != NULL && (strstr(food, "candy") != NULL ||
                            strstr(food, "糖") != NULL))
    {
      line = "棒棒糖！转圈圈～";
    }
  else if (food != NULL && (strstr(food, "dango") != NULL ||
                            strstr(food, "团子") != NULL))
    {
      line = "三色团子！软乎乎～";
    }
  else if (food != NULL && (strstr(food, "gem") != NULL ||
                            strstr(food, "钻石") != NULL))
    {
      line = "钻石？！这能吃吗……咕咚。真香！";
    }
  else
    {
      line = "小鱼干！我的最爱！";
    }

  pet_engine_motion(SEAGULL_PECK);
  g_pet.hunger = clamp_int(g_pet.hunger - 18, 0, 100);
  g_pet.mood = clamp_int(g_pet.mood + 8, 0, 100);
  say_raw(line, false);
  emit("feed", line);
}

void pet_engine_say(const char *text, bool inner)
{
  say_raw(text, inner);
  emit("say", text);
}

void pet_engine_set_weather_cb(pet_weather_fn fn)
{
  g_pet.weather_cb = fn;
}

static const char *weather_reply(void)
{
  char line[64];

  copy_text(line, sizeof(line), "今天适合摸鱼，顺便等薯条。");
  if (g_pet.weather_cb != NULL)
    {
      char filled[64];

      if (g_pet.weather_cb(filled, sizeof(filled)) == 0 && filled[0] != '\0')
        {
          copy_text(line, sizeof(line), filled);
        }
    }

  copy_text(g_pet.last_reply, sizeof(g_pet.last_reply), line);
  return g_pet.last_reply;
}

void pet_engine_weather(void)
{
  weather_reply();
  say_raw(g_pet.last_reply, false);
  emit("weather", g_pet.last_reply);
}

void pet_engine_chat(const char *user_msg)
{
  const char *reply;

  if (user_msg == NULL || user_msg[0] == '\0')
    {
      reply = "嘎？是在叫我，还是在叫外卖？";
    }
  else if (strstr(user_msg, "天气") != NULL)
    {
      weather_reply();
      say_raw(g_pet.last_reply, false);
      emit("chat", g_pet.last_reply);
      return;
    }
  else if (strstr(user_msg, "你好") != NULL || strstr(user_msg, "嗨") != NULL)
    {
      reply = "嘎！带薯条来了吗？";
    }
  else if (strstr(user_msg, "薯条") != NULL || strstr(user_msg, "fries") != NULL)
    {
      reply = "薯条可以分你一根，袋子归我。";
    }
  else if (strstr(user_msg, "饿") != NULL || strstr(user_msg, "吃") != NULL)
    {
      reply = "先来一包薯条，别忘了番茄酱。";
    }
  else if (strstr(user_msg, "困") != NULL || strstr(user_msg, "睡") != NULL)
    {
      reply = "我睡一会儿，薯条帮我看好。";
    }
  else if (strstr(user_msg, "训练") != NULL || strstr(user_msg, "AGI") != NULL)
    {
      reply = "正在训练：隔着屏幕寻找薯条。";
    }
  else
    {
      reply = pick(g_lines, sizeof(g_lines) / sizeof(g_lines[0]));
    }

  copy_text(g_pet.last_reply, sizeof(g_pet.last_reply), reply);
  say_raw(reply, false);
  emit("chat", reply);
}

void pet_engine_set_follow(int x, int y)
{
  g_pet.motion=SEAGULL_IDLE;g_pet.motion_ms=0;
  g_pet.follow_x = clamp_int(x, 0, g_pet.screen_w);
  g_pet.follow_y = clamp_int(y, 0, g_pet.screen_h);
  g_pet.has_follow = true;
  if (g_pet.mode != PET_FOLLOW)
    {
      pet_engine_set_mode(PET_FOLLOW);
    }
}

void pet_engine_drag_begin(int x, int y)
{
  g_pet.motion=SEAGULL_IDLE;g_pet.motion_ms=0;g_pet.jump_t=0;
  g_pet.dragging = true;
  g_pet.has_target = false;
  g_pet.drag_off_x = x - g_pet.x;
  g_pet.drag_off_y = y - g_pet.y;
}

void pet_engine_drag_move(int x, int y)
{
  int nx;
  int ny;

  if (!g_pet.dragging)
    {
      return;
    }

  nx = x - g_pet.drag_off_x;
  ny = y - g_pet.drag_off_y;
  set_dir_from_delta(nx - g_pet.x, ny - g_pet.y);
  g_pet.x = nx;
  g_pet.y = ny;
  clamp_pos();
}

void pet_engine_drag_end(void)
{
  if (!g_pet.dragging)
    {
      return;
    }

  g_pet.dragging = false;
  pet_engine_motion(SEAGULL_SHAKE);
  g_pet.dir = PET_DOWN;
  g_pet.facing = 1;
  g_pet.rest_ms = REST_MIN_MS + rand_range(0, REST_SPAN_MS);
  if ((xorshift() & 1u) != 0)
    {
      say_raw(pick(g_drag, sizeof(g_drag) / sizeof(g_drag[0])), false);
    }

  emit("drag", g_pet.bubble);
}
