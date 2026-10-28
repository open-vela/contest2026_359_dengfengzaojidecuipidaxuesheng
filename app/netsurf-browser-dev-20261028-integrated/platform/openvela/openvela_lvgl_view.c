/* SPDX-License-Identifier: GPL-2.0-only */
#include "openvela_lvgl_view.h"

#include <stdlib.h>
#include <string.h>

struct ov_lvgl_view {
	lv_obj_t *image;
	lv_image_dsc_t images[2];
	uint16_t *pixels[2];
	size_t buffer_bytes;
	int width;
	int height;
	int front;
	int writing;
	int pending;
	bool frame_ready;
	bool resizing;
	struct ov_damage damage;
	uint64_t serial;
	struct ov_lvgl_allocator allocator;
	struct ov_lvgl_view_ops ops;
};

static void frame_lock(struct ov_lvgl_view *view)
{
	if (view->ops.frame_sync.lock != NULL)
		view->ops.frame_sync.lock(view->ops.frame_sync.ctx);
}

static void frame_unlock(struct ov_lvgl_view *view)
{
	if (view->ops.frame_sync.unlock != NULL)
		view->ops.frame_sync.unlock(view->ops.frame_sync.ctx);
}

static void *view_allocate(const struct ov_lvgl_allocator *allocator,
			   size_t bytes)
{
	if (allocator->allocate != NULL)
		return allocator->allocate(allocator->ctx, bytes);
	return malloc(bytes);
}

static void view_release(const struct ov_lvgl_allocator *allocator,
			 void *memory)
{
	if (memory == NULL)
		return;
	if (allocator->release != NULL)
		allocator->release(allocator->ctx, memory);
	else
		free(memory);
}

static void image_descriptor(lv_image_dsc_t *image, int width, int height,
			     const uint16_t *pixels, size_t bytes)
{
	memset(image, 0, sizeof(*image));
	image->header.magic = LV_IMAGE_HEADER_MAGIC;
	image->header.cf = LV_COLOR_FORMAT_RGB565;
	image->header.w = (uint16_t)width;
	image->header.h = (uint16_t)height;
	image->header.stride = (uint32_t)width * sizeof(uint16_t);
	image->data_size = (uint32_t)bytes;
	image->data = (const uint8_t *)pixels;
}

struct ov_lvgl_view *ov_lvgl_view_create(lv_obj_t *parent, int width, int height,
					 const struct ov_lvgl_allocator *allocator,
					 const struct ov_lvgl_view_ops *ops)
{
	if (parent == NULL || width <= 0 || height <= 0 ||
	    width > UINT16_MAX || height > UINT16_MAX ||
	    (allocator != NULL && ((allocator->allocate == NULL) !=
				    (allocator->release == NULL))) ||
	    (ops != NULL &&
	     ((ops->frame_sync.lock == NULL) !=
	      (ops->frame_sync.unlock == NULL))) ||
	    (ops != NULL &&
	     (ops->wake_lvgl != NULL || ops->frame_consumed != NULL) &&
	     ops->frame_sync.lock == NULL))
		return NULL;

	size_t stride = (size_t)width * sizeof(uint16_t);
	if (stride > UINT32_MAX || (size_t)height > SIZE_MAX / stride)
		return NULL;
	size_t buffer_bytes = stride * (size_t)height;
	if (buffer_bytes > UINT32_MAX)
		return NULL;

	struct ov_lvgl_view *view = calloc(1, sizeof(*view));
	if (view == NULL)
		return NULL;
	if (allocator != NULL)
		view->allocator = *allocator;
	if (ops != NULL)
		view->ops = *ops;
	view->width = width;
	view->height = height;
	view->front = -1;
	view->writing = -1;
	view->pending = -1;
	view->buffer_bytes = buffer_bytes;

	view->pixels[0] = view_allocate(&view->allocator, view->buffer_bytes);
	view->pixels[1] = view_allocate(&view->allocator, view->buffer_bytes);
	if (view->pixels[0] == NULL || view->pixels[1] == NULL)
		goto fail;
	memset(view->pixels[0], 0, view->buffer_bytes);
	memset(view->pixels[1], 0, view->buffer_bytes);
	image_descriptor(&view->images[0], width, height, view->pixels[0],
			 view->buffer_bytes);
	image_descriptor(&view->images[1], width, height, view->pixels[1],
			 view->buffer_bytes);

	view->image = lv_image_create(parent);
	if (view->image == NULL)
		goto fail;
	lv_obj_set_size(view->image, width, height);
	lv_obj_remove_flag(view->image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_pad_all(view->image, 0, 0);
	lv_obj_set_style_border_width(view->image, 0, 0);
	return view;

fail:
	view_release(&view->allocator, view->pixels[0]);
	view_release(&view->allocator, view->pixels[1]);
	free(view);
	return NULL;
}

void ov_lvgl_view_destroy(struct ov_lvgl_view *view)
{
	if (view == NULL)
		return;
	/* The renderer must be stopped first; the LVGL image may still reference
	 * the current front buffer until its object is deleted here. */
	if (view->image != NULL)
		lv_obj_delete(view->image);
	view_release(&view->allocator, view->pixels[0]);
	view_release(&view->allocator, view->pixels[1]);
	free(view);
}

bool ov_lvgl_view_resize(struct ov_lvgl_view *view, int width, int height)
{
	if (view == NULL || width <= 0 || height <= 0 ||
	    width > UINT16_MAX || height > UINT16_MAX)
		return false;
	if ((size_t)width > SIZE_MAX / sizeof(uint16_t) ||
	    (size_t)height > SIZE_MAX / ((size_t)width * sizeof(uint16_t)))
		return false;
	size_t bytes = (size_t)width * (size_t)height * sizeof(uint16_t);
	if (bytes > UINT32_MAX)
		return false;
	frame_lock(view);
	bool same_size = width == view->width && height == view->height;
	frame_unlock(view);
	if (same_size) return true;

	uint16_t *pixels0 = view_allocate(&view->allocator, bytes);
	uint16_t *pixels1 = view_allocate(&view->allocator, bytes);
	if (pixels0 == NULL || pixels1 == NULL) {
		view_release(&view->allocator, pixels0);
		view_release(&view->allocator, pixels1);
		return false;
	}
	memset(pixels0, 0, bytes);
	memset(pixels1, 0, bytes);

	frame_lock(view);
	if (width == view->width && height == view->height) {
		frame_unlock(view);
		view_release(&view->allocator, pixels0);
		view_release(&view->allocator, pixels1);
		return true;
	}
	if (view->resizing || view->frame_ready || view->writing >= 0 ||
	    view->pending >= 0) {
		frame_unlock(view);
		view_release(&view->allocator, pixels0);
		view_release(&view->allocator, pixels1);
		return false;
	}
	/* Block a concurrent begin_frame while the image descriptors and buffer
	 * pointers are exchanged. The producer never sees a half-resized view. */
	view->resizing = true;
	uint16_t *old0 = view->pixels[0];
	uint16_t *old1 = view->pixels[1];
	view->pixels[0] = pixels0;
	view->pixels[1] = pixels1;
	view->width = width;
	view->height = height;
	view->buffer_bytes = bytes;
	view->front = 0;
	image_descriptor(&view->images[0], width, height, pixels0, bytes);
	image_descriptor(&view->images[1], width, height, pixels1, bytes);
	view->resizing = false;
	frame_unlock(view);

	lv_obj_set_size(view->image, width, height);
	lv_image_set_src(view->image, &view->images[0]);
	lv_obj_invalidate(view->image);
	view_release(&view->allocator, old0);
	view_release(&view->allocator, old1);
	return true;
}

uint16_t *ov_lvgl_view_begin_frame(struct ov_lvgl_view *view,
				  int *stride_pixels)
{
	if (view == NULL)
		return NULL;
	frame_lock(view);
	if (view->resizing || view->frame_ready || view->writing >= 0) {
		frame_unlock(view);
		return NULL;
	}
	int back = view->front == 0 ? 1 : 0;
	view->writing = back;
	if (stride_pixels != NULL)
		*stride_pixels = view->width;
	frame_unlock(view);
	return view->pixels[back];
}

bool ov_lvgl_view_present(struct ov_lvgl_view *view,
				  const struct ov_damage *damage,
				  uint64_t serial)
{
	if (view == NULL)
		return false;
	frame_lock(view);
	if (view->writing < 0 || view->frame_ready) {
		frame_unlock(view);
		return false;
	}
	view->damage = damage != NULL ? *damage : (struct ov_damage){.full = true};
	view->serial = serial;
	view->pending = view->writing;
	view->frame_ready = true;
	view->writing = -1;
	frame_unlock(view);
	if (view->ops.wake_lvgl != NULL)
		view->ops.wake_lvgl(view->ops.ctx);
	return true;
}

void ov_lvgl_view_abort_frame(struct ov_lvgl_view *view)
{
	if (view != NULL) {
		frame_lock(view);
		view->writing = -1;
		frame_unlock(view);
	}
}

bool ov_lvgl_view_poll(struct ov_lvgl_view *view)
{
	if (view == NULL)
		return false;
	frame_lock(view);
	if (!view->frame_ready || view->pending < 0 || view->pending > 1) {
		frame_unlock(view);
		return false;
	}
	int next_front = view->pending;
	view->front = next_front;
	view->pending = -1;
	view->frame_ready = false;
	frame_unlock(view);
	lv_image_set_src(view->image, &view->images[next_front]);
	/* Damage regions are retained for the next LVGL partial-invalidation path.
	 * Full invalidation is used until the target LVGL draw-area contract is
	 * available to this module, ensuring pixels outside damage remain correct. */
	lv_obj_invalidate(view->image);
	if (view->ops.frame_consumed != NULL)
		view->ops.frame_consumed(view->ops.ctx);
	return true;
}

lv_obj_t *ov_lvgl_view_object(struct ov_lvgl_view *view)
{
	return view != NULL ? view->image : NULL;
}

int ov_lvgl_view_width(const struct ov_lvgl_view *view)
{
	if (view == NULL)
		return 0;
	struct ov_lvgl_view *mutable_view = (struct ov_lvgl_view *)view;
	frame_lock(mutable_view);
	int width = view->width;
	frame_unlock(mutable_view);
	return width;
}

int ov_lvgl_view_height(const struct ov_lvgl_view *view)
{
	if (view == NULL)
		return 0;
	struct ov_lvgl_view *mutable_view = (struct ov_lvgl_view *)view;
	frame_lock(mutable_view);
	int height = view->height;
	frame_unlock(mutable_view);
	return height;
}
