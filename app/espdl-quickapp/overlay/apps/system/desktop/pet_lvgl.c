/****************************************************************************
 * apps/system/desktop/pet_lvgl.c
 *
 * Always-on floating seagull overlay.  Other apps talk to the engine via
 * system.pet; this file only draws and handles drag/poke.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include "pet_lvgl.h"
#include "glass_ui_visibility.h"
#include "glass_chat.h"
#include "glass_music_pcm.h"
#include "glass_voice.h"
#include "pet_engine.h"
#include "pet_sprites.h"
#include "glass_seagull.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUBBLE_GAP 16
#define DOUBLE_CLICK_MS 300
#define PET_TICK_MS 50
#define PET_AUDIO_RENDER_MS 100
#define PET_SPRITE_PATH "/sdcard/dafeiyu/dafeiyu.lvbin"
#define BODY_BLUE 0x3d9ad1
#define BODY_DEEP 0x2b6f9c
#define BELLY     0xf4ead6
#define BLUSH     0xf3a6b5
#define INK       0x243040

enum pet_voice_stage_e
{
  PET_VOICE_IDLE = 0,
  PET_VOICE_RECORDING,
  PET_VOICE_FINISHING,
  PET_VOICE_QUEUEING,
  PET_VOICE_WAITING
};

struct pet_ui_s
{
  lv_obj_t *root;
  lv_obj_t *bubble;
  lv_obj_t *body;
  lv_obj_t *belly;
  lv_obj_t *tail;
  lv_obj_t *eye_l;
  lv_obj_t *eye_r;
  lv_obj_t *pupil_l;
  lv_obj_t *pupil_r;
  lv_obj_t *blush_l;
  lv_obj_t *blush_r;
  lv_obj_t *mouth;
  lv_obj_t *sprite;
  lv_image_dsc_t sprite_dsc;
  struct pet_sprite_bundle_s sprites;
  char bubble_text[64];
  int bubble_width;
  int bubble_offset;
  bool bubble_inner;
  bool bubble_inner_valid;
  int sprite_view;
  int sprite_size;
  int turn_pose;
  uint32_t turn_at;
  struct pet_state_s rendered;
  int rendered_top_offset;
  int rendered_root_x;
  int rendered_root_y;
  unsigned render_elapsed_ms;
  bool rendered_valid;
  const lv_font_t *font_zh;
  lv_timer_t *timer;
  lv_timer_t *click_timer;
  lv_timer_t *voice_timer;
  struct chat_snapshot *voice_chat;
  enum pet_voice_stage_e voice_stage;
  uint32_t voice_revision,voice_request;
  uint32_t voice_turn_id;
  unsigned voice_queue_ticks;
  char voice_text[GLASS_VOICE_TEXT_BYTES+1];
  int press_x;
  int press_y;
  bool dragging;
  bool press_moved;
  bool open;
};

static struct pet_ui_s g_ui;

static void layout_parts(const struct pet_state_s *state, int top_offset);

static void set_blobs_hidden(bool hidden)
{
  lv_obj_t *objects[] =
    {
      g_ui.body, g_ui.belly, g_ui.tail, g_ui.eye_l, g_ui.eye_r,
      g_ui.pupil_l, g_ui.pupil_r, g_ui.blush_l, g_ui.blush_r, g_ui.mouth
    };
  unsigned i;

  for (i = 0; i < sizeof(objects) / sizeof(objects[0]); i++)
    {
      if (objects[i] == NULL)
        {
          continue;
        }

      if (hidden)
        {
          lv_obj_add_flag(objects[i], LV_OBJ_FLAG_HIDDEN);
        }
      else
        {
          lv_obj_remove_flag(objects[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}





static void layout_sprite(const struct pet_state_s *state, int top_offset)
{
  unsigned target=state->dir==PET_LEFT?0:state->dir==PET_RIGHT?8:4;
  unsigned index;
  uint32_t now=lv_tick_get();
  if(g_ui.turn_pose!=(int)target&&now-g_ui.turn_at>=80){
    g_ui.turn_pose+=g_ui.turn_pose<(int)target?1:-1;g_ui.turn_at=now;
  }
  index=state->dir==PET_UP?SEAGULL_REAR:SEAGULL_YAW_BASE+(unsigned)g_ui.turn_pose;
  if(state->dir==PET_DOWN&&g_ui.turn_pose==4&&state->action_t>0)index=8; /* happy */
  struct seagull_pose pose=seagull_motion_pose(SEAGULL_IDLE,0);
  if(state->motion && !state->dragging && g_ui.turn_pose==4){
    pose=seagull_motion_pose(state->motion,state->motion_ms);
    index=pose.index;
  }
  if(g_ui.sprite_view!=(int)index){
    lv_image_cache_drop(&g_ui.sprite_dsc);
    if(glass_seagull_decode(index,g_ui.sprites.storage,SEAGULL_SIDE)!=0)return;
    memset(&g_ui.sprite_dsc,0,sizeof(g_ui.sprite_dsc));
    g_ui.sprite_dsc.header.magic=LV_IMAGE_HEADER_MAGIC;
    g_ui.sprite_dsc.header.cf=LV_COLOR_FORMAT_RGB565A8;
    g_ui.sprite_dsc.header.w=g_ui.sprite_dsc.header.h=SEAGULL_SIDE;
    g_ui.sprite_dsc.header.stride=SEAGULL_SIDE*2;
    g_ui.sprite_dsc.data_size=SEAGULL_BYTES;
    g_ui.sprite_dsc.data=g_ui.sprites.storage;
    lv_image_set_src(g_ui.sprite,&g_ui.sprite_dsc);
    g_ui.sprite_view=(int)index;
  }
  set_blobs_hidden(true);lv_obj_remove_flag(g_ui.sprite,LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_size(g_ui.root,state->w,state->h+top_offset);
  int side=state->w<state->h?state->w:state->h;
  lv_image_set_pivot(g_ui.sprite,64,64);
  lv_image_set_scale_x(g_ui.sprite,side*pose.sx/SEAGULL_SIDE);
  lv_image_set_scale_y(g_ui.sprite,side*pose.sy/SEAGULL_SIDE);
  lv_image_set_rotation(g_ui.sprite,(pose.angle+3600)%3600);
  lv_image_set_antialias(g_ui.sprite,true);
  lv_obj_set_pos(g_ui.sprite,state->w/2-64,top_offset+state->h/2-64);
}

static lv_obj_t *make_blob(lv_obj_t *parent, uint32_t color, int radius)
{
  lv_obj_t *obj = lv_obj_create(parent);

  if (obj == NULL)
    {
      return NULL;
    }

  lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(obj, 0, 0);
  lv_obj_set_style_radius(obj, radius, 0);
  lv_obj_set_style_pad_all(obj, 0, 0);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  return obj;
}

static void layout_parts(const struct pet_state_s *state, int top_offset)
{
  int w = state->w;
  int h = state->h;
  int body_w = (w * 78) / 100;
  int body_h = (h * 70) / 100;
  int body_x = (w - body_w) / 2;
  int body_y = top_offset + (h - body_h) / 2;
  bool side = state->dir == PET_LEFT || state->dir == PET_RIGHT;
  bool back = state->dir == PET_UP;
  int face = state->facing;

  lv_obj_set_size(g_ui.root, w, h + top_offset);
  lv_obj_set_size(g_ui.body, body_w, body_h);
  lv_obj_set_pos(g_ui.body, body_x, body_y);
  lv_obj_set_size(g_ui.belly, (body_w * 62) / 100, (body_h * 42) / 100);
  lv_obj_set_pos(g_ui.belly, body_x + (body_w * 19) / 100,
                 body_y + (body_h * 42) / 100);

  if (side)
    {
      int tail_x = face > 0 ? body_x + body_w - 6 : body_x - (w * 18) / 100 + 6;

      lv_obj_set_size(g_ui.tail, (w * 22) / 100, (h * 28) / 100);
      lv_obj_set_pos(g_ui.tail, tail_x, body_y + (body_h * 28) / 100);
      lv_obj_remove_flag(g_ui.tail, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(g_ui.eye_r, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(g_ui.pupil_r, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(g_ui.blush_r, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_size(g_ui.eye_l, 10, 10);
      lv_obj_set_pos(g_ui.eye_l,
                     face > 0 ? body_x + 14 : body_x + body_w - 24,
                     body_y + 14);
      lv_obj_set_size(g_ui.pupil_l, 4, 4);
      lv_obj_set_pos(g_ui.pupil_l,
                     face > 0 ? body_x + 17 : body_x + body_w - 21,
                     body_y + 17);
      lv_obj_set_size(g_ui.blush_l, 8, 5);
      lv_obj_set_pos(g_ui.blush_l,
                     face > 0 ? body_x + 12 : body_x + body_w - 22,
                     body_y + 28);
      lv_obj_set_size(g_ui.mouth, 12, 4);
      lv_obj_set_pos(g_ui.mouth,
                     face > 0 ? body_x + 18 : body_x + body_w - 30,
                     body_y + 34);
    }
  else
    {
      lv_obj_set_size(g_ui.tail, (w * 18) / 100, (h * 16) / 100);
      lv_obj_set_pos(g_ui.tail, body_x + body_w / 2 - (w * 9) / 100,
                     body_y + body_h - 8);
      lv_obj_remove_flag(g_ui.tail, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_size(g_ui.eye_l, 11, 11);
      lv_obj_set_size(g_ui.eye_r, 11, 11);
      lv_obj_set_pos(g_ui.eye_l, body_x + body_w / 2 - 18, body_y + 14);
      lv_obj_set_pos(g_ui.eye_r, body_x + body_w / 2 + 7, body_y + 14);
      lv_obj_set_size(g_ui.pupil_l, 4, 4);
      lv_obj_set_size(g_ui.pupil_r, 4, 4);
      lv_obj_set_pos(g_ui.pupil_l, body_x + body_w / 2 - 15, body_y + 17);
      lv_obj_set_pos(g_ui.pupil_r, body_x + body_w / 2 + 10, body_y + 17);
      lv_obj_set_size(g_ui.blush_l, 9, 5);
      lv_obj_set_size(g_ui.blush_r, 9, 5);
      lv_obj_set_pos(g_ui.blush_l, body_x + body_w / 2 - 22, body_y + 28);
      lv_obj_set_pos(g_ui.blush_r, body_x + body_w / 2 + 13, body_y + 28);
      lv_obj_set_size(g_ui.mouth, 16, 5);
      lv_obj_set_pos(g_ui.mouth, body_x + body_w / 2 - 8, body_y + 36);
      if (back)
        {
          lv_obj_add_flag(g_ui.eye_l, LV_OBJ_FLAG_HIDDEN);
          lv_obj_add_flag(g_ui.eye_r, LV_OBJ_FLAG_HIDDEN);
          lv_obj_add_flag(g_ui.pupil_l, LV_OBJ_FLAG_HIDDEN);
          lv_obj_add_flag(g_ui.pupil_r, LV_OBJ_FLAG_HIDDEN);
          lv_obj_add_flag(g_ui.blush_l, LV_OBJ_FLAG_HIDDEN);
          lv_obj_add_flag(g_ui.blush_r, LV_OBJ_FLAG_HIDDEN);
          lv_obj_add_flag(g_ui.mouth, LV_OBJ_FLAG_HIDDEN);
        }
      else
        {
          lv_obj_remove_flag(g_ui.eye_l, LV_OBJ_FLAG_HIDDEN);
          lv_obj_remove_flag(g_ui.eye_r, LV_OBJ_FLAG_HIDDEN);
          lv_obj_remove_flag(g_ui.pupil_l, LV_OBJ_FLAG_HIDDEN);
          lv_obj_remove_flag(g_ui.pupil_r, LV_OBJ_FLAG_HIDDEN);
          lv_obj_remove_flag(g_ui.blush_l, LV_OBJ_FLAG_HIDDEN);
          lv_obj_remove_flag(g_ui.blush_r, LV_OBJ_FLAG_HIDDEN);
          lv_obj_remove_flag(g_ui.mouth, LV_OBJ_FLAG_HIDDEN);
        }
    }

  if (!back && side)
    {
      lv_obj_remove_flag(g_ui.eye_l, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(g_ui.pupil_l, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(g_ui.blush_l, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(g_ui.mouth, LV_OBJ_FLAG_HIDDEN);
    }
}

static void pet_voice_say(const char *prefix, const char *text, bool inner)
{
  char line[64] = "";
  size_t used = 0;

  if (prefix != NULL)
    {
      used = glass_chat_copy_utf8(line, sizeof(line), prefix);
    }

  if (text != NULL && used + 1 < sizeof(line))
    {
      glass_chat_copy_utf8(line + used, sizeof(line) - used, text);
    }

  if (line[0] != '\0')
    {
      pet_engine_say(line, inner);
      pet_lvgl_sync();
    }
}

static void pet_voice_reset(void)
{
  if (g_ui.voice_timer != NULL)
    {
      lv_timer_delete(g_ui.voice_timer);
      g_ui.voice_timer = NULL;
    }

  free(g_ui.voice_chat);
  g_ui.voice_chat = NULL;
  g_ui.voice_stage = PET_VOICE_IDLE;
  g_ui.voice_revision = 0;
  g_ui.voice_turn_id = 0;
  g_ui.voice_queue_ticks = 0;
  g_ui.voice_text[0] = '\0';
}

static void pet_voice_fail(const char *message)
{
  pet_voice_reset();
  pet_voice_say(message, NULL, true);
}

static const char *pet_voice_error(enum glass_voice_error error)
{
  switch (error)
    {
      case GLASS_VOICE_ERROR_CONFIG:
        return "请先配置语音识别";
      case GLASS_VOICE_ERROR_AUTH:
        return "语音识别凭据无效";
      case GLASS_VOICE_ERROR_AUDIO:
        return "麦克风暂时不可用";
      case GLASS_VOICE_ERROR_EMPTY:
        return "没有听清，请再试一次";
      default:
        return "语音识别失败";
    }
}

static const struct chat_turn *pet_voice_turn(void)
{
  unsigned i;

  if (g_ui.voice_chat == NULL)
    {
      return NULL;
    }

  for (i = 0; i < g_ui.voice_chat->count; i++)
    {
      if (g_ui.voice_chat->turns[i].id == g_ui.voice_turn_id)
        {
          return &g_ui.voice_chat->turns[i];
        }
    }

  return NULL;
}

static void pet_voice_poll(lv_timer_t *timer)
{
  struct glass_voice_snapshot voice;
  bool chat_changed;

  LV_UNUSED(timer);
  if (g_ui.voice_stage == PET_VOICE_RECORDING ||
      g_ui.voice_stage == PET_VOICE_FINISHING)
    {
      glass_voice_get(&voice);
      if(voice.request!=g_ui.voice_request){pet_voice_fail("麦克风已交给新的语音请求");return;}
      if (voice.revision != g_ui.voice_revision)
        {
          g_ui.voice_revision = voice.revision;
          if (voice.phase == GLASS_VOICE_CONNECTING)
            {
              pet_voice_say("正在连接语音...", NULL, true);
            }
          else if (voice.phase == GLASS_VOICE_LISTENING)
            {
              pet_voice_say(voice.text[0] != '\0' ? "正在听：" :
                            "请说话，松开发送", voice.text, true);
            }
          else if (voice.phase == GLASS_VOICE_FINISHING)
            {
              pet_voice_say("正在识别...", NULL, true);
            }
          else if (voice.phase == GLASS_VOICE_DONE)
            {
              if (voice.text[0] == '\0')
                {
                  pet_voice_fail("没有听清，请再试一次");
                  return;
                }

              glass_chat_copy_utf8(g_ui.voice_text,
                                   sizeof(g_ui.voice_text), voice.text);
              g_ui.voice_stage = PET_VOICE_QUEUEING;
              g_ui.voice_queue_ticks = 0;
              pet_voice_say("正在思考：", g_ui.voice_text, true);
            }
          else if (voice.phase == GLASS_VOICE_ERROR)
            {
              pet_voice_fail(pet_voice_error(voice.error));
              return;
            }
        }
    }

  if (g_ui.voice_stage != PET_VOICE_QUEUEING &&
      g_ui.voice_stage != PET_VOICE_WAITING)
    {
      return;
    }

  chat_changed = glass_chat_get(g_ui.voice_chat);
  if (g_ui.voice_stage == PET_VOICE_QUEUEING)
    {
      int ret;

      if (!g_ui.voice_chat->loaded)
        {
          if (++g_ui.voice_queue_ticks >= 100)
            {
              pet_voice_fail("AI 服务加载超时");
            }

          return;
        }

      if (!g_ui.voice_chat->configured)
        {
          pet_voice_fail("请先配置 AI 服务");
          return;
        }

      if (g_ui.voice_chat->phase != CHAT_IDLE)
        {
          pet_voice_fail("AI 正在处理上一条消息");
          return;
        }

      ret = glass_chat_send(g_ui.voice_text);
      if (ret < 0)
        {
          pet_voice_fail(ret == -ENOKEY ? "请先配置 AI 服务" :
                         ret == -EBUSY ? "AI 正在处理上一条消息" :
                         "语音消息发送失败");
          return;
        }

      glass_chat_get(g_ui.voice_chat);
      if (g_ui.voice_chat->count == 0)
        {
          pet_voice_fail("语音消息发送失败");
          return;
        }

      g_ui.voice_turn_id =
        g_ui.voice_chat->turns[g_ui.voice_chat->count - 1].id;
      g_ui.voice_stage = PET_VOICE_WAITING;
      return;
    }

  if (chat_changed)
    {
      const struct chat_turn *turn = pet_voice_turn();

      if (turn == NULL)
        {
          pet_voice_fail("没有找到本轮 AI 回复");
        }
      else if (turn->state == CHAT_WAITING)
        {
          if (turn->reply[0] != '\0')
            {
              pet_voice_say(NULL, turn->reply, false);
            }
        }
      else if (turn->state == CHAT_DONE)
        {
          pet_voice_say(NULL, turn->reply[0] != '\0' ? turn->reply :
                        "AI 没有返回文字", false);
          pet_voice_reset();
        }
      else if (turn->state == CHAT_CANCELLED)
        {
          pet_voice_fail("AI 回复已取消");
        }
      else
        {
          pet_voice_fail(turn->error == CHAT_ERROR_AUTH ?
                         "AI 服务认证失败" :
                         turn->error == CHAT_ERROR_TIMEOUT ?
                         "AI 回复超时" : "AI 回复失败");
        }
    }
}

static void pet_voice_begin(void)
{
  struct glass_voice_snapshot voice;
  int ret;

  if (g_ui.voice_stage != PET_VOICE_IDLE)
    {
      pet_voice_say("上一轮语音还在处理中", NULL, true);
      return;
    }

  g_ui.voice_chat = calloc(1, sizeof(*g_ui.voice_chat));
  if (g_ui.voice_chat == NULL)
    {
      pet_voice_say("可用内存不足", NULL, true);
      return;
    }

  ret = glass_chat_start();
  if (ret < 0)
    {
      pet_voice_fail("AI 服务启动失败");
      return;
    }

  ret = glass_voice_start_request(&g_ui.voice_request);
  if (ret < 0)
    {
      pet_voice_fail(ret == -EBUSY ? "语音服务正在使用中" :
                     "语音服务启动失败");
      return;
    }

  glass_voice_get(&voice);
  g_ui.voice_revision = voice.revision;
  g_ui.voice_stage = PET_VOICE_RECORDING;
  g_ui.voice_timer = lv_timer_create(pet_voice_poll, 120, NULL);
  if (g_ui.voice_timer == NULL)
    {
      glass_voice_stop_request(g_ui.voice_request);
      pet_voice_fail("可用内存不足");
      return;
    }

  pet_voice_say("请说话，松开发送", NULL, true);
}

static void pet_voice_release(void)
{
  if (g_ui.voice_stage == PET_VOICE_RECORDING)
    {
      glass_voice_stop_request(g_ui.voice_request);
      g_ui.voice_stage = PET_VOICE_FINISHING;
      pet_voice_say("正在识别...", NULL, true);
    }
}

static int pointer_xy(lv_event_t *e, int *x, int *y)
{
  lv_indev_t *indev = lv_event_get_indev(e);
  lv_point_t point;

  if (indev == NULL || x == NULL || y == NULL)
    {
      return -1;
    }

  lv_indev_get_point(indev, &point);
  *x = point.x;
  *y = point.y;
  return 0;
}

static void pet_click_cb(lv_timer_t *timer)
{
  g_ui.click_timer = NULL;
  lv_timer_delete(timer);
  pet_engine_poke();
  pet_lvgl_sync();
}

static void pet_event_cb(lv_event_t *e)
{
  lv_event_code_t code = lv_event_get_code(e);
  int x;
  int y;

  if (code == LV_EVENT_PRESSED)
    {
      if (pointer_xy(e, &x, &y) < 0)
        {
          return;
        }

      g_ui.press_x = x;
      g_ui.press_y = y;
      g_ui.dragging = false;
      g_ui.press_moved = false;
    }
  else if (code == LV_EVENT_PRESSING)
    {
      if (g_ui.voice_stage == PET_VOICE_RECORDING)
        {
          return;
        }

      if (pointer_xy(e, &x, &y) < 0)
        {
          return;
        }

      if (!g_ui.dragging)
        {
          int dx = x - g_ui.press_x;
          int dy = y - g_ui.press_y;

          if (dx * dx + dy * dy > 64)
            {
              g_ui.dragging = true;
              g_ui.press_moved = true;
              pet_engine_drag_begin(g_ui.press_x, g_ui.press_y);
            }
        }

      if (g_ui.dragging)
        {
          pet_engine_drag_move(x, y);
          pet_lvgl_sync();
        }
    }
  else if (code == LV_EVENT_LONG_PRESSED)
    {
      if (!g_ui.dragging)
        {
          g_ui.press_moved = true;
          if (g_ui.click_timer != NULL)
            {
              lv_timer_delete(g_ui.click_timer);
              g_ui.click_timer = NULL;
            }

          pet_voice_begin();
        }
    }
  else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
    {
      pet_voice_release();
      if (g_ui.dragging)
        {
          pet_engine_drag_end();
        }

      g_ui.dragging = false;
    }
  else if (code == LV_EVENT_CLICKED)
    {
      if (!g_ui.press_moved)
        {
          if (g_ui.click_timer != NULL)
            {
              lv_timer_delete(g_ui.click_timer);
              g_ui.click_timer = NULL;
              pet_engine_feed("fries");
              pet_lvgl_sync();
            }
          else
            {
              g_ui.click_timer = lv_timer_create(pet_click_cb,
                                                 DOUBLE_CLICK_MS, NULL);
              if (g_ui.click_timer == NULL)
                {
                  pet_engine_poke();
                  pet_lvgl_sync();
                }
            }
        }
    }
}

static void pet_timer_cb(lv_timer_t *timer)
{
  LV_UNUSED(timer);
  if (g_ui.voice_stage == PET_VOICE_IDLE)
    {
      pet_engine_tick(PET_TICK_MS);
    }

  g_ui.render_elapsed_ms += PET_TICK_MS;
  if (!glass_ui_covered(g_ui.root) && (!music_pcm_session_active() ||
      g_ui.render_elapsed_ms >= PET_AUDIO_RENDER_MS))
    {
      pet_lvgl_sync();
    }
}

int pet_lvgl_create(lv_obj_t *parent, const lv_font_t *font_zh)
{
  struct pet_state_s state;

  if (parent == NULL)
    {
      return -1;
    }

  pet_lvgl_destroy();
  pet_engine_get(&state);
  g_ui.font_zh = font_zh;
  g_ui.root = lv_obj_create(parent);
  if (g_ui.root == NULL)
    {
      return -1;
    }

  lv_obj_set_style_bg_opa(g_ui.root, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(g_ui.root, 0, 0);
  lv_obj_set_style_pad_all(g_ui.root, 0, 0);
  lv_obj_set_style_shadow_width(g_ui.root, 0, 0);
  lv_obj_remove_flag(g_ui.root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(g_ui.root, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(g_ui.root, pet_event_cb, LV_EVENT_ALL, NULL);

  g_ui.bubble = lv_label_create(g_ui.root);
  if (g_ui.bubble == NULL)
    {
      pet_lvgl_destroy();
      return -1;
    }
  lv_label_set_text(g_ui.bubble, "");
  lv_label_set_long_mode(g_ui.bubble, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(g_ui.bubble, state.w);
  lv_obj_set_style_bg_color(g_ui.bubble, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_bg_opa(g_ui.bubble, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(g_ui.bubble, lv_color_hex(INK), 0);
  lv_obj_set_style_pad_hor(g_ui.bubble, 8, 0);
  lv_obj_set_style_pad_ver(g_ui.bubble, 4, 0);
  lv_obj_set_style_radius(g_ui.bubble, 10, 0);
  lv_obj_set_style_text_align(g_ui.bubble, LV_TEXT_ALIGN_CENTER, 0);
  if (font_zh != NULL)
    {
      lv_obj_set_style_text_font(g_ui.bubble, font_zh, 0);
    }

  lv_obj_add_flag(g_ui.bubble, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(g_ui.bubble, LV_OBJ_FLAG_CLICKABLE);

  g_ui.body = make_blob(g_ui.root, BODY_BLUE, 40);
  g_ui.belly = make_blob(g_ui.root, BELLY, 24);
  g_ui.tail = make_blob(g_ui.root, BODY_DEEP, 12);
  g_ui.eye_l = make_blob(g_ui.root, 0xffffff, 8);
  g_ui.eye_r = make_blob(g_ui.root, 0xffffff, 8);
  g_ui.pupil_l = make_blob(g_ui.root, INK, 4);
  g_ui.pupil_r = make_blob(g_ui.root, INK, 4);
  g_ui.blush_l = make_blob(g_ui.root, BLUSH, 4);
  g_ui.blush_r = make_blob(g_ui.root, BLUSH, 4);
  g_ui.mouth = make_blob(g_ui.root, 0xd46a7a, 4);
  g_ui.sprite = lv_image_create(g_ui.root);
  if (g_ui.bubble == NULL || g_ui.body == NULL || g_ui.belly == NULL ||
      g_ui.tail == NULL || g_ui.eye_l == NULL || g_ui.eye_r == NULL ||
      g_ui.pupil_l == NULL || g_ui.pupil_r == NULL ||
      g_ui.blush_l == NULL || g_ui.blush_r == NULL || g_ui.mouth == NULL ||
      g_ui.sprite == NULL)
    {
      pet_lvgl_destroy();
      return -1;
    }

  lv_obj_remove_flag(g_ui.sprite, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(g_ui.sprite, LV_OBJ_FLAG_HIDDEN);
  g_ui.sprite_view = -1;
  g_ui.sprite_size = -1;
  g_ui.turn_pose=4;
  g_ui.sprites.storage=malloc(SEAGULL_BYTES);
  if(!g_ui.sprites.storage){pet_lvgl_destroy();return -1;}
  g_ui.sprites.storage_size=SEAGULL_BYTES;


  g_ui.timer = lv_timer_create(pet_timer_cb, PET_TICK_MS, NULL);
  if (g_ui.timer == NULL)
    {
      pet_lvgl_destroy();
      return -1;
    }

  g_ui.open = true;
  pet_lvgl_sync();
  return 0;
}

void pet_lvgl_sync(void)
{
  struct pet_state_s state;
  int top_offset = 0;
  int jump;
  int root_y;
  bool was_visible;
  bool bubble_visible;
  bool bubble_was_visible;
  bool layout_changed;

  if (!g_ui.open || g_ui.root == NULL)
    {
      return;
    }

  g_ui.render_elapsed_ms = 0;
  pet_engine_get(&state);
  was_visible = g_ui.rendered_valid && g_ui.rendered.visible;
  if (!state.visible)
    {
      if (!g_ui.rendered_valid || was_visible)
        {
          lv_obj_add_flag(g_ui.root, LV_OBJ_FLAG_HIDDEN);
        }

      g_ui.rendered = state;
      g_ui.rendered_valid = true;
      return;
    }

  if (!was_visible)
    {
      lv_obj_remove_flag(g_ui.root, LV_OBJ_FLAG_HIDDEN);
    }

  bubble_visible = state.bubble[0] != '\0';
  bubble_was_visible = was_visible && g_ui.rendered.bubble[0] != '\0';
  if (bubble_visible)
    {
      if (g_ui.bubble_width != state.w ||
          strcmp(g_ui.bubble_text, state.bubble) != 0)
        {
          lv_obj_set_width(g_ui.bubble, state.w);
          lv_label_set_text(g_ui.bubble, state.bubble);
          lv_obj_update_layout(g_ui.bubble);
          g_ui.bubble_width = state.w;
          g_ui.bubble_offset = lv_obj_get_height(g_ui.bubble) + BUBBLE_GAP;
          snprintf(g_ui.bubble_text, sizeof(g_ui.bubble_text), "%s",
                   state.bubble);
        }

      if (!g_ui.bubble_inner_valid ||
          g_ui.bubble_inner != state.bubble_inner)
        {
          lv_obj_set_style_text_color(g_ui.bubble,
                                      lv_color_hex(state.bubble_inner ?
                                                   0x7d7d8a : INK), 0);
          g_ui.bubble_inner = state.bubble_inner;
          g_ui.bubble_inner_valid = true;
        }

      if (!bubble_was_visible)
        {
          lv_obj_remove_flag(g_ui.bubble, LV_OBJ_FLAG_HIDDEN);
        }

      top_offset = g_ui.bubble_offset;
    }
  else if (bubble_was_visible || !g_ui.rendered_valid)
    {
      lv_obj_add_flag(g_ui.bubble, LV_OBJ_FLAG_HIDDEN);
    }

  layout_changed = !g_ui.rendered_valid || !was_visible ||
                   state.w != g_ui.rendered.w ||
                   state.h != g_ui.rendered.h ||
                   state.dir != g_ui.rendered.dir ||
                   state.facing != g_ui.rendered.facing ||
                   state.motion != g_ui.rendered.motion ||
                   state.motion_ms/SEAGULL_MOTION_FRAME_MS != g_ui.rendered.motion_ms/SEAGULL_MOTION_FRAME_MS ||
                   top_offset != g_ui.rendered_top_offset ||
                   (state.action_t>0)!=(g_ui.rendered.action_t>0) ||
                   g_ui.turn_pose!=(state.dir==PET_LEFT?0:state.dir==PET_RIGHT?8:4);
  if (layout_changed && g_ui.sprites.storage != NULL)
    {
      layout_sprite(&state, top_offset);
    }
  else if (layout_changed)
    {
      layout_parts(&state, top_offset);
    }

  jump = state.jump_t > 0 ? (state.jump_t / 30) : 0;
  struct seagull_pose motion_pose=seagull_motion_pose(state.motion,state.motion_ms);
  int side=state.w<state.h?state.w:state.h;
  int root_x=state.x+motion_pose.x*side/128;
  root_y = state.y - top_offset - jump;
  root_y+=motion_pose.y*side/128;
  if(root_y<0)root_y=0;
  if (!g_ui.rendered_valid || !was_visible ||
      root_x != g_ui.rendered_root_x || root_y != g_ui.rendered_root_y)
    {
      lv_obj_set_pos(g_ui.root, root_x, root_y);
      g_ui.rendered_root_x = root_x;
      g_ui.rendered_root_y = root_y;
    }

  if (top_offset > 0 && (layout_changed || !bubble_was_visible))
    {
      lv_obj_align(g_ui.bubble, LV_ALIGN_TOP_MID, 0, 0);
    }

  g_ui.rendered = state;
  g_ui.rendered_top_offset = top_offset;
  g_ui.rendered_valid = true;
}

void pet_lvgl_destroy(void)
{
  if (g_ui.voice_stage == PET_VOICE_RECORDING ||
      g_ui.voice_stage == PET_VOICE_FINISHING)
    {
      glass_voice_stop_request(g_ui.voice_request);
    }

  pet_voice_reset();

  if (g_ui.click_timer != NULL)
    {
      lv_timer_delete(g_ui.click_timer);
      g_ui.click_timer = NULL;
    }

  if (g_ui.timer != NULL)
    {
      lv_timer_delete(g_ui.timer);
      g_ui.timer = NULL;
    }

  if (g_ui.root != NULL)
    {
      if (g_ui.sprite_dsc.data != NULL)
        {
          lv_image_cache_drop(&g_ui.sprite_dsc);
        }
      lv_obj_delete(g_ui.root);
      g_ui.root = NULL;
    }

  pet_sprites_unload(&g_ui.sprites);

  memset(&g_ui, 0, sizeof(g_ui));
}

bool pet_lvgl_is_open(void)
{
  return g_ui.open && g_ui.root != NULL;
}

bool pet_lvgl_uses_sd_sprites(void)
{ return false; /* Appearance is built into the firmware. */ }

size_t pet_lvgl_sprite_bytes(void)
{
  return g_ui.sprites.storage_size;
}

/* Called on the GUI thread, including after a card is inserted. Keep the
 * current image alive until the entire replacement has passed validation. */
int pet_lvgl_reload_sprites(void)
{
  if(!pet_lvgl_is_open())return -ENODEV;
  g_ui.sprite_view=-1;g_ui.turn_pose=4;g_ui.rendered_valid=false;
  pet_lvgl_sync();return 0;
}
