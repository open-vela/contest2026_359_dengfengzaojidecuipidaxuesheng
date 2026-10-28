/* gui_bitmap_table: 32bpp straight-alpha R,G,B,A bitmaps. */
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "utils/errors.h"
#include "openvela_bitmap.h"
#include "openvela_plot.h"

static void *bitmap_create(int width, int height, enum gui_bitmap_flags flags)
{
	if (width <= 0 || height <= 0 || width > 65535 || height > 65535)
		return NULL;
	size_t stride = (size_t)width * 4;
	if ((size_t)height > SIZE_MAX / stride)
		return NULL;
	size_t bytes = stride * (size_t)height;
	if (bytes > ((size_t)4 << 20))
		return NULL;

	struct bitmap *bitmap = calloc(1, sizeof(*bitmap));
	if (bitmap == NULL)
		return NULL;
	bitmap->pixels = malloc(bytes);
	if (bitmap->pixels == NULL) {
		free(bitmap);
		return NULL;
	}
	if (flags & BITMAP_CLEAR)
		memset(bitmap->pixels, 0, bytes);
	bitmap->width = width;
	bitmap->height = height;
	bitmap->stride = stride;
	bitmap->opaque = (flags & BITMAP_OPAQUE) != 0;
	return bitmap;
}

static void bitmap_destroy(void *handle)
{
	struct bitmap *bitmap = handle;
	if (bitmap == NULL)
		return;
	ov_plot_bitmap_cache_invalidate(bitmap);
	free(bitmap->pixels);
	free(bitmap);
}

static void bitmap_set_opaque(void *handle, bool opaque)
{
	((struct bitmap *)handle)->opaque = opaque;
}

static bool bitmap_get_opaque(void *handle)
{
	return ((struct bitmap *)handle)->opaque;
}

static unsigned char *bitmap_get_buffer(void *handle)
{
	return ((struct bitmap *)handle)->pixels;
}

static size_t bitmap_get_rowstride(void *handle)
{
	return ((struct bitmap *)handle)->stride;
}

static int bitmap_get_width(void *handle)
{
	return ((struct bitmap *)handle)->width;
}

static int bitmap_get_height(void *handle)
{
	return ((struct bitmap *)handle)->height;
}

static void bitmap_modified(void *handle)
{
	struct bitmap *bitmap = handle;
	bitmap->version++;
	ov_plot_bitmap_cache_invalidate(bitmap);
}

static nserror bitmap_render(struct bitmap *bitmap, struct hlcache_handle *content)
{
	(void)bitmap;
	(void)content;
	return NSERROR_NOT_IMPLEMENTED; /* thumbnails are not used */
}

struct gui_bitmap_table ov_bitmap_table = {
	.create = bitmap_create,
	.destroy = bitmap_destroy,
	.set_opaque = bitmap_set_opaque,
	.get_opaque = bitmap_get_opaque,
	.get_buffer = bitmap_get_buffer,
	.get_rowstride = bitmap_get_rowstride,
	.get_width = bitmap_get_width,
	.get_height = bitmap_get_height,
	.modified = bitmap_modified,
	.render = bitmap_render,
};
