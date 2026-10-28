/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <lvgl/lvgl.h>
#include "glass_chat.h"
#include "glass_voice.h"
struct glass_grok;
typedef const lv_font_t *(*glass_grok_font_t)(int size);
/* Native SD sprite player. Parent owns reader, timer and two frame buffers.
 * File I/O runs on a reader thread; only the UI thread calls LVGL. */
struct glass_grok *glass_grok_create(lv_obj_t *parent,int width,glass_grok_font_t font);
/* Original pixel rendering at the selected 96 px desktop size. */
struct glass_grok *glass_grok_create_pixel(lv_obj_t *parent,int width,bool black,glass_grok_font_t font);
/* Same 96 px desktop layout, with charcoal / ivory mosaic palette. */
struct glass_grok *glass_grok_create_mosaic(lv_obj_t *parent,int width,bool black,glass_grok_font_t font);
/* Original 48 px pixel mini, with upstream shading and expression poses,
 * in a transparent 64 x 64 viewport. Home presentation remains separate. */
struct glass_grok *glass_grok_create_avatar(lv_obj_t *parent,glass_grok_font_t font);
void glass_grok_set_reduced_motion(struct glass_grok *ball,bool reduced);
/* App-owned, lifetime-stable UI strings; keeps this player locale-independent. */
void glass_grok_set_notice_text(struct glass_grok *ball,const char *loading,const char *unavailable);
/* Optional stable overlay that must outlive the component (desktop panel). */
void glass_grok_set_obscured_by(struct glass_grok *ball,lv_obj_t *overlay);
unsigned glass_grok_emotion(const struct chat_activity_snapshot *chat,
                            const struct glass_voice_snapshot *voice);
unsigned glass_grok_current(const struct glass_grok *ball);
bool glass_grok_ready(const struct glass_grok *ball);
struct glass_grok *glass_grok_create_sprite(lv_obj_t *parent,int x,int y,int width,int height,int diameter,bool black,glass_grok_font_t font);
/* Reposition/resize a sprite without replacing its reader or animation. */
bool glass_grok_geometry(struct glass_grok *ball,int x,int y,int width,int height,int diameter);
lv_obj_t *glass_grok_root(struct glass_grok *ball);
void glass_grok_select(struct glass_grok *ball,unsigned id);
/* Brief varied moods only while the requested state is idle (02).
 * Service states and explicit app expression selection take precedence. */
void glass_grok_set_ambient(struct glass_grok *ball,bool enabled);
/* Front-facing original eye contours; normalized face offsets -1000..1000. */
bool glass_grok_gaze(struct glass_grok *ball,int x,int y);
/* Pre-rendered app gestures: 42 spin; 43..51 gaze grid; 0 restore state. */
void glass_grok_action(struct glass_grok *ball,unsigned action);
void glass_grok_set_active(struct glass_grok *ball,bool active);
/* UI-thread refresh when a direct camera owns display pages and timers pause. */
void glass_grok_refresh(struct glass_grok *ball);
/* Follow the same chat/voice state as ESPClaw; no extra conversation worker. */
void glass_grok_follow_chat(struct glass_grok *ball,bool follow);
void glass_grok_debug_status(void);
#ifdef GROK_HOST_TEST
void glass_grok_test_step(struct glass_grok *ball);
void glass_grok_test_step_visible(void);
void glass_grok_test_stats(struct glass_grok *ball,unsigned out[3]);
/* Exercise the real embedded registry and static renderer without polling. */
unsigned glass_grok_test_expression(struct glass_grok *ball,const char *id);
#endif
