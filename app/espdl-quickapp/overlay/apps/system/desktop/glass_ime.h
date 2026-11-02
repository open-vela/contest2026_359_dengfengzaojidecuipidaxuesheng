/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <lvgl/lvgl.h>
/* Reserve 48 px above the keyboard. Owned by the keyboard's lifetime.
 * Uses white/black in light mode and black/white in dark mode, selected from
 * caller foreground brightness. */
void glass_ime_attach(lv_obj_t *keyboard, const lv_font_t *font, bool chinese,
                       uint32_t foreground, uint32_t surface, uint32_t accent);
void glass_ime_commit(lv_obj_t *keyboard);
/* Retarget a shared keyboard and discard the previous field's composition. */
void glass_ime_set_target(lv_obj_t *keyboard,lv_obj_t *target);
void glass_ime_commit_tree(lv_obj_t *parent);
/* Start the same voice input action as the keyboard microphone. UI thread. */
bool glass_ime_start_voice(lv_obj_t *keyboard);
/* Offline-only applications disable the shared keyboard's network voice path. */
void glass_ime_set_voice_enabled(lv_obj_t *keyboard,bool enabled);
