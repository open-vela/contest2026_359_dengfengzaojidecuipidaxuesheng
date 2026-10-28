/****************************************************************************
 * apps/system/desktop/qpk_runtime.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <lvgl/lvgl.h>

/* Optional trusted native presentation hook. Standalone runtime hosts can omit
 * it; no JavaScript capability, callback ownership or pointer bubbling changes. */
#include "glass_motion_hooks.h"

typedef const lv_font_t *(*qpk_font_cb_t)(int size);
typedef void (*qpk_message_cb_t)(const char *message);
/* Reported when a JavaScript entry runs past the response budget without
 * yielding (true) and again once the app has been healthy or stops (false).
 * The desktop shows and hides the wait/terminate prompt. */
typedef void (*qpk_unresponsive_cb_t)(bool unresponsive);

/* Lifecycle and UI calls belong to the LVGL thread. Stop the active runtime
 * before destroying its root; retired pages may be deleted after a handoff.
 */
int qpk_runtime_launch(lv_obj_t *root, const char *name,
                       const char *package, const char *version,
                       const char *filename, const char *source,
                       size_t source_len, qpk_font_cb_t font_cb,
                       qpk_message_cb_t toast_cb,
                       qpk_message_cb_t dialog_cb,
                       qpk_unresponsive_cb_t unresponsive_cb);
void qpk_runtime_stop(void);
bool qpk_runtime_running(void);
/* Pop one app-owned view or input dialog. False lets the shell exit one level. */
bool qpk_runtime_back(void);
/* Select classification (0) or face following (1) on the active built-in
 * ESP-DL app. Bounded JS callback, LVGL thread only; does not start capture. */
int qpk_runtime_select_espdl_mode(unsigned mode);
/* UI-thread diagnostics; the last error survives stop until the next launch. */
const char *qpk_runtime_last_error(void);
unsigned qpk_runtime_widget_count(void);
/* True while direct capture or its cleanup still owns the display pages. */
bool qpk_runtime_camera_active(void);
/* UI-thread ARGB8888 snapshot (little-endian BGRA), copied for direct video.
 * NULL removes it. Never becomes part of a saved photo or recording. */
int qpk_runtime_wake_overlay(const uint8_t *bgra,unsigned width,unsigned height,unsigned stride,int x,int y);
/* A hardware session cannot be replaced until cleanup has finished. */
bool qpk_runtime_camera_busy(void);
bool qpk_runtime_camera_capture(void);
bool qpk_runtime_camera_capture_request(uint32_t *request);
int qpk_runtime_camera_photo_status(uint32_t request,char *path,size_t capacity);
/* Local board diagnostics, dispatched by the persistent desktop task. */
int qpk_runtime_camera_video_action(unsigned action);
/* Call from the UI loop even while LVGL is suspended. Handles worker failure
 * and bounded cleanup retries; returns a one-shot full-redraw request.
 */
bool qpk_runtime_camera_poll(void);
/* UI thread: consume an album request only after display and writer release. */
bool qpk_runtime_camera_take_album(void);
/* Board diagnostics: forward one pointer update to an app that registered
 * ui.onTouch(), using fractions of the app content. Returns 1 when delivered.
 */
int qpk_runtime_touch_test(const char *state, double nx, double ny);
/* Fixed, read-only HA diagnostic. No credential fields are exported. */
int qpk_runtime_ha_status(char *out, size_t capacity);

/* Read-only navigation to a synced HA device; no device command is sent. */
int qpk_runtime_open_ha_device(const char *server, const char *entity);
