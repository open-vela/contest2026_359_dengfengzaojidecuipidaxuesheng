/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NETSURF_OPENVELA_LVGL_VIEW_H
#define NETSURF_OPENVELA_LVGL_VIEW_H

#include <lvgl/lvgl.h>

#include "openvela_platform.h"

struct ov_lvgl_view;

struct ov_lvgl_allocator {
	void *(*allocate)(void *ctx, size_t bytes);
	void (*release)(void *ctx, void *memory);
	void *ctx;
};

struct ov_lvgl_view_ops {
	/* Called after a frame becomes ready. The callback must only wake/schedule
	 * the LVGL owner thread; it must not call LVGL from the browser thread. */
	void (*wake_lvgl)(void *ctx);
	/* Called after LVGL switches to the submitted frame; only wake the core. */
	void (*frame_consumed)(void *ctx);
	/* Required when the frame producer and LVGL owner are different threads. */
	struct ov_queue_sync frame_sync;
	void *ctx;
};

/* Create, poll, and destroy on the LVGL owner thread. Frame operations may
 * run on the single NetSurf renderer thread. Supply frame_sync callbacks for
 * cross-thread use; without them every operation must use one thread. Stop
 * the renderer before destroy. */
struct ov_lvgl_view *ov_lvgl_view_create(lv_obj_t *parent, int width, int height,
					 const struct ov_lvgl_allocator *allocator,
					 const struct ov_lvgl_view_ops *ops);
void ov_lvgl_view_destroy(struct ov_lvgl_view *view);

/* Resize on the LVGL owner thread. The renderer must have no frame in flight. */
bool ov_lvgl_view_resize(struct ov_lvgl_view *view, int width, int height);

/* Only one frame may be in flight. begin_frame returns NULL until the LVGL
 * thread has consumed the previous present. Pixels are little-endian RGB565. */
uint16_t *ov_lvgl_view_begin_frame(struct ov_lvgl_view *view,
				  int *stride_pixels);
bool ov_lvgl_view_present(struct ov_lvgl_view *view,
				  const struct ov_damage *damage,
				  uint64_t serial);
void ov_lvgl_view_abort_frame(struct ov_lvgl_view *view);

/* Consume one submitted frame. Call from the LVGL owner loop. Returns true if
 * an image source changed and a redraw was requested. */
bool ov_lvgl_view_poll(struct ov_lvgl_view *view);
lv_obj_t *ov_lvgl_view_object(struct ov_lvgl_view *view);
int ov_lvgl_view_width(const struct ov_lvgl_view *view);
int ov_lvgl_view_height(const struct ov_lvgl_view *view);

#endif
