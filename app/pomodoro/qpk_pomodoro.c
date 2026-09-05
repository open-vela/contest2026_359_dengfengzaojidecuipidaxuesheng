/****************************************************************************
 * apps/system/desktop/qpk_pomodoro.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include "qpk_pomodoro.h"
#include "pomodoro_engine.h"

static const char *phase_id(enum pomodoro_phase_e phase)
{
  if (phase == POMODORO_SHORT)
    {
      return "short";
    }

  if (phase == POMODORO_LONG)
    {
      return "long";
    }

  return "work";
}

static JSValue js_state(JSContext *context)
{
  struct pomodoro_state_s state;
  JSValue object;

  pomodoro_engine_get(&state);
  object = JS_NewObject(context);
  JS_SetPropertyStr(context, object, "phase",
                    JS_NewString(context, phase_id(state.phase)));
  JS_SetPropertyStr(context, object, "running",
                    JS_NewBool(context, state.running));
  JS_SetPropertyStr(context, object, "remaining",
                    JS_NewInt32(context, state.remaining_ms));
  JS_SetPropertyStr(context, object, "workMin",
                    JS_NewInt32(context, state.work_min));
  JS_SetPropertyStr(context, object, "shortMin",
                    JS_NewInt32(context, state.short_min));
  JS_SetPropertyStr(context, object, "longMin",
                    JS_NewInt32(context, state.long_min));
  JS_SetPropertyStr(context, object, "completed",
                    JS_NewInt32(context, state.completed));
  JS_SetPropertyStr(context, object, "label",
                    JS_NewString(context, state.label));
  JS_SetPropertyStr(context, object, "clock",
                    JS_NewString(context, state.clock));
  JS_SetPropertyStr(context, object, "eventSeq",
                    JS_NewInt32(context, (int32_t)state.event_seq));
  JS_SetPropertyStr(context, object, "toast",
                    JS_NewString(context, state.toast));
  JS_SetPropertyStr(context, object, "dialog",
                    JS_NewString(context, state.dialog));
  return object;
}

static JSValue js_pomodoro_get_state(JSContext *context,
                                     JSValueConst this_value,
                                     int argc, JSValueConst *argv)
{
  (void)this_value;
  (void)argc;
  (void)argv;
  return js_state(context);
}

static JSValue js_pomodoro_start(JSContext *context, JSValueConst this_value,
                                 int argc, JSValueConst *argv)
{
  (void)this_value;
  (void)argc;
  (void)argv;
  pomodoro_engine_start();
  return js_state(context);
}

static JSValue js_pomodoro_pause(JSContext *context, JSValueConst this_value,
                                 int argc, JSValueConst *argv)
{
  (void)this_value;
  (void)argc;
  (void)argv;
  pomodoro_engine_pause();
  return js_state(context);
}

static JSValue js_pomodoro_toggle(JSContext *context, JSValueConst this_value,
                                  int argc, JSValueConst *argv)
{
  (void)this_value;
  (void)argc;
  (void)argv;
  pomodoro_engine_toggle();
  return js_state(context);
}

static JSValue js_pomodoro_skip(JSContext *context, JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
  (void)this_value;
  (void)argc;
  (void)argv;
  pomodoro_engine_skip();
  return js_state(context);
}

static JSValue js_pomodoro_reset(JSContext *context, JSValueConst this_value,
                                 int argc, JSValueConst *argv)
{
  (void)this_value;
  (void)argc;
  (void)argv;
  pomodoro_engine_reset();
  return js_state(context);
}

static JSValue js_pomodoro_scene_work(JSContext *context,
                                      JSValueConst this_value,
                                      int argc, JSValueConst *argv)
{
  (void)this_value;
  (void)argc;
  (void)argv;
  pomodoro_engine_scene_work();
  return js_state(context);
}

static JSValue js_pomodoro_add_work(JSContext *context,
                                    JSValueConst this_value,
                                    int argc, JSValueConst *argv)
{
  int32_t delta = 0;

  (void)this_value;
  if (argc > 0)
    {
      JS_ToInt32(context, &delta, argv[0]);
    }

  pomodoro_engine_add_work((int)delta);
  return js_state(context);
}

static JSValue js_pomodoro_add_break(JSContext *context,
                                     JSValueConst this_value,
                                     int argc, JSValueConst *argv)
{
  int32_t delta = 0;

  (void)this_value;
  if (argc > 0)
    {
      JS_ToInt32(context, &delta, argv[0]);
    }

  pomodoro_engine_add_break((int)delta);
  return js_state(context);
}

static JSValue js_pomodoro_tick(JSContext *context, JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
  (void)this_value;
  (void)argc;
  (void)argv;
  pomodoro_engine_tick();
  return js_state(context);
}

void qpk_pomodoro_bind(JSContext *context, JSValue system)
{
  JSValue object = JS_NewObject(context);

  JS_SetPropertyStr(context, object, "getState",
                    JS_NewCFunction(context, js_pomodoro_get_state,
                                    "getState", 0));
  JS_SetPropertyStr(context, object, "start",
                    JS_NewCFunction(context, js_pomodoro_start, "start", 0));
  JS_SetPropertyStr(context, object, "pause",
                    JS_NewCFunction(context, js_pomodoro_pause, "pause", 0));
  JS_SetPropertyStr(context, object, "toggle",
                    JS_NewCFunction(context, js_pomodoro_toggle,
                                    "toggle", 0));
  JS_SetPropertyStr(context, object, "skip",
                    JS_NewCFunction(context, js_pomodoro_skip, "skip", 0));
  JS_SetPropertyStr(context, object, "reset",
                    JS_NewCFunction(context, js_pomodoro_reset, "reset", 0));
  JS_SetPropertyStr(context, object, "sceneWork",
                    JS_NewCFunction(context, js_pomodoro_scene_work,
                                    "sceneWork", 0));
  JS_SetPropertyStr(context, object, "addWork",
                    JS_NewCFunction(context, js_pomodoro_add_work,
                                    "addWork", 1));
  JS_SetPropertyStr(context, object, "addBreak",
                    JS_NewCFunction(context, js_pomodoro_add_break,
                                    "addBreak", 1));
  JS_SetPropertyStr(context, object, "tick",
                    JS_NewCFunction(context, js_pomodoro_tick, "tick", 0));
  JS_SetPropertyStr(context, system, "pomodoro", object);
}
