/* SPDX-License-Identifier: GPL-2.0-only */
#include "openvela_image.h"
#include "openvela_bitmap.h"
#include <stdlib.h>
#include <string.h>
#include <png.h>
#include "qpk_tjpgd.h"
#include "content/content_protected.h"
#include "content/content_factory.h"
#include "content/llcache.h"
#include "content/handlers/image/image.h"

#define MAX_IMAGE_PIXELS (1024u * 1024u)
#define MAX_PNG_WORKSPACE (1024u * 1024u)
#if JD_FORMAT != 1
#error "The desktop JPEG decoder must output RGB565"
#endif
union png_block { max_align_t alignment; size_t bytes; };
struct png_input {
	const uint8_t *data;
	size_t size, position, allocated;
	struct bitmap *bitmap;
	png_bytep *rows;
};
static png_voidp png_allocate(png_structp png, png_alloc_size_t bytes)
{
	struct png_input *in = png_get_mem_ptr(png);
	if (bytes > MAX_PNG_WORKSPACE - in->allocated) return NULL;
	union png_block *block = malloc(sizeof(*block) + bytes);
	if (!block) return NULL;
	in->allocated += bytes;
	block->bytes = bytes;
	return block + 1;
}
static void png_release(png_structp png, png_voidp memory)
{
	if (!memory) return;
	struct png_input *in = png_get_mem_ptr(png);
	union png_block *block = (union png_block *)memory - 1;
	in->allocated -= block->bytes;
	free(block);
}
static void png_input(png_structp png, png_bytep data, png_size_t size)
{
	struct png_input *in = png_get_io_ptr(png);
	if (size > in->size - in->position) png_error(png, "truncated PNG");
	memcpy(data, in->data + in->position, size);
	in->position += size;
}
static struct bitmap *decode_png(const uint8_t *data, size_t size)
{
	struct png_input *in = calloc(1, sizeof(*in));
	if (!in) return NULL;
	in->data = data; in->size = size;
	png_structp png = png_create_read_struct_2(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL,
					in, png_allocate, png_release);
	if (!png) { free(in); return NULL; }
	png_infop info = png_create_info_struct(png);
	if (!info) { png_destroy_read_struct(&png, NULL, NULL); free(in); return NULL; }
	if (setjmp(png_jmpbuf(png))) {
		if (in->bitmap) ov_bitmap_table.destroy(in->bitmap);
		free(in->rows);
		png_destroy_read_struct(&png, &info, NULL);
		free(in); return NULL;
	}
	png_set_read_fn(png, in, png_input);
	png_set_user_limits(png, 8192, 8192);
	png_set_chunk_malloc_max(png, 64u * 1024u);
	png_set_chunk_cache_max(png, 32);
	png_read_info(png, info);
	png_uint_32 width = png_get_image_width(png, info), height = png_get_image_height(png, info);
	if (!width || !height || width > MAX_IMAGE_PIXELS / height) png_error(png, "image limit");
	int depth = png_get_bit_depth(png, info), colour = png_get_color_type(png, info);
	if (depth == 16) png_set_strip_16(png);
	if (colour == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
	if (colour == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
	bool transparent = png_get_valid(png, info, PNG_INFO_tRNS) != 0;
	if (transparent) png_set_tRNS_to_alpha(png);
	if (colour == PNG_COLOR_TYPE_GRAY || colour == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
	if (!(colour & PNG_COLOR_MASK_ALPHA) && !transparent) png_set_add_alpha(png, 255, PNG_FILLER_AFTER);
	png_set_interlace_handling(png);
	png_read_update_info(png, info);
	if (png_get_rowbytes(png, info) != (size_t)width * 4) png_error(png, "unsupported layout");
	in->bitmap = ov_bitmap_table.create(width, height, BITMAP_CLEAR);
	in->rows = malloc((size_t)height * sizeof(*in->rows));
	if (!in->bitmap || !in->rows) png_error(png, "out of memory");
	for (png_uint_32 row = 0; row < height; row++) in->rows[row] = in->bitmap->pixels + row * in->bitmap->stride;
	png_read_image(png, in->rows);
	png_read_end(png, NULL);
	struct bitmap *bitmap = in->bitmap;
	free(in->rows); png_destroy_read_struct(&png, &info, NULL); free(in);
	return bitmap;
}
struct jpeg_input {
	const uint8_t *data;
	size_t size, position;
	struct bitmap *bitmap;
};
static size_t jpeg_read(JDEC *decoder, uint8_t *buffer, size_t size)
{
	struct jpeg_input *in = decoder->device;
	if (size > in->size - in->position) size = in->size - in->position;
	if (buffer) memcpy(buffer, in->data + in->position, size);
	in->position += size; return size;
}
static int jpeg_write(JDEC *decoder, void *pixels, JRECT *rect)
{
	struct bitmap *b = ((struct jpeg_input *)decoder->device)->bitmap;
	if (rect->right >= b->width || rect->bottom >= b->height) return 0;
	const uint16_t *src = pixels;
	for (unsigned y = rect->top; y <= rect->bottom; y++) {
		uint8_t *dst = b->pixels + y * b->stride + rect->left * 4;
		for (unsigned x = rect->left; x <= rect->right; x++) {
			unsigned p = *src++;
			dst[0] = ((p >> 11) * 255 + 15) / 31;
			dst[1] = (((p >> 5) & 63) * 255 + 31) / 63;
			dst[2] = ((p & 31) * 255 + 15) / 31;
			dst[3] = 255; dst += 4;
		}
	}
	return 1;
}
static struct bitmap *decode_jpeg(const uint8_t *data, size_t size)
{
	struct jpeg_input in = {.data=data, .size=size};
	void *workspace = malloc(16384);
	if (!workspace) return NULL;
	JDEC decoder;
	if (qpk_jd_prepare(&decoder, jpeg_read, workspace, 16384, &in) == JDR_OK &&
	    decoder.width && decoder.height && decoder.width <= MAX_IMAGE_PIXELS / decoder.height) {
		in.bitmap = ov_bitmap_table.create(decoder.width, decoder.height, BITMAP_OPAQUE);
		if (in.bitmap && qpk_jd_decomp(&decoder, jpeg_write, 0) != JDR_OK) {
			ov_bitmap_table.destroy(in.bitmap); in.bitmap = NULL;
		}
	}
	free(workspace); return in.bitmap;
}
struct bitmap *ov_image_decode(const uint8_t *data, size_t size)
{
	if (!data || size < 8 || size > 4u * 1024u * 1024u) return NULL;
	struct bitmap *bitmap = !png_sig_cmp(data, 0, 8) ? decode_png(data, size) :
		data[0] == 0xff && data[1] == 0xd8 ? decode_jpeg(data, size) : NULL;
	if (bitmap) {
		bool opaque = true;
		for (int y = 0; y < bitmap->height && opaque; y++)
			for (int x = 0; x < bitmap->width; x++)
				if (bitmap->pixels[y * bitmap->stride + x * 4 + 3] != 255) { opaque = false; break; }
		ov_bitmap_table.set_opaque(bitmap, opaque);
		ov_bitmap_table.modified(bitmap);
	}
	return bitmap;
}
struct image_content { struct content base; struct bitmap *bitmap; };
static nserror create(const content_handler *handler, lwc_string *mime,
		const struct http_parameter *params, llcache_handle *cache,
		const char *charset, bool quirks, struct content **result)
{
	struct image_content *image = calloc(1, sizeof(*image));
	if (!image) return NSERROR_NOMEM;
	nserror error = content__init(&image->base, handler, mime, params, cache, charset, quirks);
	if (error) { free(image); return error; }
	*result = &image->base; return NSERROR_OK;
}
static bool convert(struct content *content)
{
	struct image_content *image = (struct image_content *)content;
	size_t size; const uint8_t *data = content__get_source_data(content, &size);
	image->bitmap = ov_image_decode(data, size);
	if (!image->bitmap) { content_broadcast_error(content, NSERROR_BAD_ENCODING, NULL); return false; }
	content->width = image->bitmap->width; content->height = image->bitmap->height;
	content->size += image->bitmap->stride * image->bitmap->height;
	content_set_ready(content); content_set_done(content); content_set_status(content, "");
	return true;
}
static void destroy(struct content *content)
{ ov_bitmap_table.destroy(((struct image_content *)content)->bitmap); }
static bool redraw(struct content *content, struct content_redraw_data *data,
		   const struct rect *clip, const struct redraw_context *ctx)
{ return image_bitmap_plot(((struct image_content *)content)->bitmap, data, clip, ctx); }
static nserror clone(const struct content *old, struct content **result)
{
	struct image_content *image = calloc(1, sizeof(*image));
	if (!image) return NSERROR_NOMEM;
	nserror error = content__clone(old, &image->base);
	if (error) { free(image); return error; }
	if ((old->status == CONTENT_STATUS_READY || old->status == CONTENT_STATUS_DONE) && !convert(&image->base)) {
		content_destroy(&image->base); return NSERROR_CLONE_FAILED;
	}
	*result = &image->base; return NSERROR_OK;
}
static void *internal(const struct content *content, void *ctx)
{ (void)ctx; return ((const struct image_content *)content)->bitmap; }
static content_type type(void) { return CONTENT_IMAGE; }
static bool opaque(struct content *content)
{ struct bitmap *b = ((struct image_content *)content)->bitmap; return b && b->opaque; }
nserror ov_image_register(void)
{
	static const content_handler handler = {
		.create=create, .data_complete=convert, .destroy=destroy, .redraw=redraw,
		.clone=clone, .get_internal=internal, .type=type, .is_opaque=opaque,
	};
	const char *types[] = {"image/png", "image/x-png", "image/jpeg", "image/jpg", "image/pjpeg"};
	for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
		nserror error = content_factory_register_handler(types[i], &handler);
		if (error) return error;
	}
	return NSERROR_OK;
}
