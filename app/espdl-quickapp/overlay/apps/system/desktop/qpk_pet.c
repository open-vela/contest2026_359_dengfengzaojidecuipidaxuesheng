/****************************************************************************
 * apps/system/desktop/qpk_pet.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include "qpk_pet.h"
#include "pet_engine.h"
#include "pet_lvgl.h"

#include <string.h>

static JSValue js_state(JSContext *context)
{
  struct pet_state_s state;
  JSValue object;

  pet_engine_get(&state);
  object = JS_NewObject(context);
  JS_SetPropertyStr(context, object, "appearance",
                    JS_NewString(context, "seagull"));
  JS_SetPropertyStr(context, object, "mode",
                    JS_NewString(context, state.mode_id));
  JS_SetPropertyStr(context, object, "dir",
                    JS_NewString(context, state.dir_id));
  JS_SetPropertyStr(context, object, "size",
                    JS_NewString(context, state.size_id));
  JS_SetPropertyStr(context, object, "facing",
                    JS_NewInt32(context, state.facing));
  JS_SetPropertyStr(context, object, "x", JS_NewInt32(context, state.x));
  JS_SetPropertyStr(context, object, "y", JS_NewInt32(context, state.y));
  JS_SetPropertyStr(context, object, "w", JS_NewInt32(context, state.w));
  JS_SetPropertyStr(context, object, "h", JS_NewInt32(context, state.h));
  JS_SetPropertyStr(context, object, "mood",
                    JS_NewInt32(context, state.mood));
  JS_SetPropertyStr(context, object, "hunger",
                    JS_NewInt32(context, state.hunger));
  JS_SetPropertyStr(context, object, "visible",
                    JS_NewBool(context, state.visible));
  JS_SetPropertyStr(context, object, "dragging",
                    JS_NewBool(context, state.dragging));
  JS_SetPropertyStr(context, object, "jump",
                    JS_NewInt32(context, state.jump_t));
  JS_SetPropertyStr(context, object, "eventSeq",
                    JS_NewInt32(context, (int32_t)state.event_seq));
  JS_SetPropertyStr(context, object, "bubble",
                    JS_NewString(context, state.bubble));
  JS_SetPropertyStr(context, object, "inner",
                    JS_NewBool(context, state.bubble_inner));
  JS_SetPropertyStr(context, object, "event",
                    JS_NewString(context, state.last_event));
  JS_SetPropertyStr(context, object, "reply",
                    JS_NewString(context, state.last_reply));
  JS_SetPropertyStr(context, object, "toast",
                    JS_NewString(context, state.toast));
  return object;
}

static JSValue js_pet_get_state(JSContext *context, JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
  (void)this_value;
  (void)argc;
  (void)argv;
  return js_state(context);
}

static JSValue js_pet_set_mode(JSContext *context, JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
  const char *mode = NULL;

  (void)this_value;
  if (argc > 0)
    {
      mode = JS_ToCString(context, argv[0]);
    }

  pet_engine_set_mode(pet_engine_parse_mode(mode));
  if (mode != NULL)
    {
      JS_FreeCString(context, mode);
    }

  return js_state(context);
}

static JSValue js_pet_show(JSContext *context, JSValueConst this_value,
                           int argc, JSValueConst *argv)
{
  (void)this_value;
  (void)argc;
  (void)argv;
  pet_engine_set_visible(true);
  return js_state(context);
}

static JSValue js_pet_hide(JSContext *context, JSValueConst this_value,
                           int argc, JSValueConst *argv)
{
  (void)this_value;
  (void)argc;
  (void)argv;
  pet_engine_set_visible(false);
  return js_state(context);
}

static JSValue js_pet_toggle(JSContext *context, JSValueConst this_value,
                             int argc, JSValueConst *argv)
{
  (void)this_value;
  (void)argc;
  (void)argv;
  pet_engine_toggle_visible();
  return js_state(context);
}

static JSValue js_pet_poke(JSContext *context, JSValueConst this_value,
                           int argc, JSValueConst *argv)
{
  (void)this_value;
  (void)argc;
  (void)argv;
  pet_engine_poke();
  return js_state(context);
}

static JSValue js_pet_feed(JSContext *context, JSValueConst this_value,
                           int argc, JSValueConst *argv)
{
  const char *food = NULL;

  (void)this_value;
  if (argc > 0)
    {
      food = JS_ToCString(context, argv[0]);
    }

  pet_engine_feed(food != NULL ? food : "fish");
  if (food != NULL)
    {
      JS_FreeCString(context, food);
    }

  return js_state(context);
}

static JSValue js_pet_say(JSContext *context, JSValueConst this_value,
                          int argc, JSValueConst *argv)
{
  const char *text = NULL;
  bool inner = false;

  (void)this_value;
  if (argc > 0)
    {
      text = JS_ToCString(context, argv[0]);
    }

  if (argc > 1)
    {
      inner = JS_ToBool(context, argv[1]);
    }

  pet_engine_say(text, inner);
  if (text != NULL)
    {
      JS_FreeCString(context, text);
    }

  return js_state(context);
}

static JSValue js_pet_chat(JSContext *context, JSValueConst this_value,
                           int argc, JSValueConst *argv)
{
  const char *text = NULL;

  (void)this_value;
  if (argc > 0)
    {
      text = JS_ToCString(context, argv[0]);
    }

  pet_engine_chat(text);
  if (text != NULL)
    {
      JS_FreeCString(context, text);
    }

  return js_state(context);
}

static JSValue js_pet_set_size(JSContext *context, JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
  const char *size = NULL;

  (void)this_value;
  if (argc > 0)
    {
      size = JS_ToCString(context, argv[0]);
    }

  pet_engine_set_size(size);
  if (size != NULL)
    {
      JS_FreeCString(context, size);
    }

  return js_state(context);
}

static JSValue js_pet_follow(JSContext *context, JSValueConst this_value,
                             int argc, JSValueConst *argv)
{
  struct pet_state_s state;
  pet_engine_get(&state);
  int32_t x = state.screen_w / 2;
  int32_t y = state.screen_h / 2;

  (void)this_value;
  if (argc > 0)
    {
      if (JS_ToInt32(context, &x, argv[0]) < 0) return JS_EXCEPTION;
    }

  if (argc > 1)
    {
      if (JS_ToInt32(context, &y, argv[1]) < 0) return JS_EXCEPTION;
    }

  pet_engine_set_follow((int)x, (int)y);
  return js_state(context);
}

static JSValue js_pet_weather(JSContext *context, JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
  (void)this_value;
  (void)argc;
  (void)argv;
  pet_engine_weather();
  return js_state(context);
}

static JSValue js_pet_tick(JSContext *context, JSValueConst this_value,
                           int argc, JSValueConst *argv)
{
  int32_t dt = 50;

  (void)this_value;
  if (argc > 0)
    {
      if (JS_ToInt32(context, &dt, argv[0]) < 0) return JS_EXCEPTION;
    }

  pet_engine_tick((int)dt);
  return js_state(context);
}

static JSValue js_pet_load_appearance(JSContext *context,
                                     JSValueConst this_value,
                                     int argc, JSValueConst *argv)
{
  (void)this_value;
  (void)argc;
  (void)argv;
  return JS_NewBool(context, pet_lvgl_reload_sprites() == 0);
}

void qpk_pet_bind(JSContext *context, JSValue system)
{
  JSValue object = JS_NewObject(context);

  JS_SetPropertyStr(context, object, "loadAppearance",
                    JS_NewCFunction(context, js_pet_load_appearance,
                                    "loadAppearance", 0));

  JS_SetPropertyStr(context, object, "getState",
                    JS_NewCFunction(context, js_pet_get_state,
                                    "getState", 0));
  JS_SetPropertyStr(context, object, "setMode",
                    JS_NewCFunction(context, js_pet_set_mode,
                                    "setMode", 1));
  JS_SetPropertyStr(context, object, "show",
                    JS_NewCFunction(context, js_pet_show, "show", 0));
  JS_SetPropertyStr(context, object, "hide",
                    JS_NewCFunction(context, js_pet_hide, "hide", 0));
  JS_SetPropertyStr(context, object, "toggle",
                    JS_NewCFunction(context, js_pet_toggle, "toggle", 0));
  JS_SetPropertyStr(context, object, "poke",
                    JS_NewCFunction(context, js_pet_poke, "poke", 0));
  JS_SetPropertyStr(context, object, "feed",
                    JS_NewCFunction(context, js_pet_feed, "feed", 1));
  JS_SetPropertyStr(context, object, "say",
                    JS_NewCFunction(context, js_pet_say, "say", 2));
  JS_SetPropertyStr(context, object, "speak",
                    JS_NewCFunction(context, js_pet_say, "speak", 2));
  JS_SetPropertyStr(context, object, "chat",
                    JS_NewCFunction(context, js_pet_chat, "chat", 1));
  JS_SetPropertyStr(context, object, "setSize",
                    JS_NewCFunction(context, js_pet_set_size,
                                    "setSize", 1));
  JS_SetPropertyStr(context, object, "follow",
                    JS_NewCFunction(context, js_pet_follow, "follow", 2));
  JS_SetPropertyStr(context, object, "weather",
                    JS_NewCFunction(context, js_pet_weather, "weather", 0));
  JS_SetPropertyStr(context, object, "tick",
                    JS_NewCFunction(context, js_pet_tick, "tick", 1));
  JS_SetPropertyStr(context, system, "pet", object);
}
