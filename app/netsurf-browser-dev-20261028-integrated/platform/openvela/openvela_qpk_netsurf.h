/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NETSURF_OPENVELA_QPK_NETSURF_H
#define NETSURF_OPENVELA_QPK_NETSURF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <quickjs.h>
#include "openvela_platform.h"

#define OV_QPK_TEXT_MAX 256

enum ov_qpk_pointer_state {
	OV_QPK_POINTER_DOWN,
	OV_QPK_POINTER_MOVE,
	OV_QPK_POINTER_UP,
	OV_QPK_POINTER_CANCEL,
};

struct ov_qpk_netsurf_status {
	bool available;
	bool loading;
	bool error_page;
	bool images_visible;
	bool reading_active;
	bool reading_available;
	int zoom_percent;
	bool can_back;
	bool can_forward;
	int scroll_x;
	int scroll_y;
	char url[OV_QPK_TEXT_MAX];
	char title[OV_QPK_TEXT_MAX];
	char message[OV_QPK_TEXT_MAX];
};

/* All operations are invoked by QuickJS on the LVGL/UI owner thread. The
 * implementation must enqueue work for the NetSurf owner thread; it must not
 * call NetSurf or the renderer directly from these callbacks. */
struct ov_qpk_netsurf_ops {
	bool (*navigate)(void *ctx, const char *url);
	bool (*back)(void *ctx);
	bool (*forward)(void *ctx);
	bool (*reload)(void *ctx);
	bool (*stop)(void *ctx);
	bool (*set_viewport)(void *ctx, int x, int y, int width, int height);
	bool (*pointer)(void *ctx, enum ov_qpk_pointer_state state, int x, int y);
	bool (*scroll_by)(void *ctx, int delta_x, int delta_y);
	bool (*status)(void *ctx, struct ov_qpk_netsurf_status *status);
	bool (*take_input)(void *ctx, struct ov_input_request *request);
	bool (*submit_input)(void *ctx, uint32_t id, const char *text);
	bool (*set_images_visible)(void *ctx, bool visible);
	bool (*set_zoom)(void *ctx, int percent);
	bool (*set_reading_enabled)(void *ctx, bool enabled);
	void *ctx;
};

/* Call while installing the system object for the active QPK runtime. */
bool ov_qpk_netsurf_bind(JSContext *context, JSValue system,
			 const struct ov_qpk_netsurf_ops *ops);
void ov_qpk_netsurf_unbind(const struct ov_qpk_netsurf_ops *ops);

#endif
