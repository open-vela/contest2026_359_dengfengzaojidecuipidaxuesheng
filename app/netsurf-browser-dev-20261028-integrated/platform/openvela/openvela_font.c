/* SPDX-License-Identifier: GPL-2.0-only */
#include "openvela_font.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H

#include "netsurf/canvas.h"
#include "netsurf/plot_style.h"
#include "openvela_plot.h"
#include "utils/errors.h"

static FT_Library font_library;
static FT_Face font_face;
/* FreeType performs many tiny, non-sequential table reads. A bounded stream
 * cache keeps DOM measurement off the SD/SPI path without copying a CJK font
 * (which can be tens of MiB) into the browser heap. Owner thread only. */
#define FONT_PAGE_BYTES 4096u
#define FONT_PAGE_COUNT 32u
#define FONT_METRIC_COUNT 2048u
struct font_file {
	FT_StreamRec stream;
	FILE *file;
	unsigned long tags[FONT_PAGE_COUNT];
	size_t lengths[FONT_PAGE_COUNT];
	uint8_t pages[FONT_PAGE_COUNT][FONT_PAGE_BYTES];
};
static struct font_file *font_file;
static FT_F26Dot6 current_size;
struct font_metric {
	uint32_t codepoint;
	FT_F26Dot6 size;
	FT_Pos advance;
	unsigned flags;
	bool bold, valid;
};
static struct font_metric metrics[FONT_METRIC_COUNT];

/* Fixed storage avoids allocating/freeing a mask for every letter on every
 * scroll frame. Large glyphs use the row-wise fallback, never grow the cache. */
#ifndef OV_FONT_GLYPH_CACHE
#define OV_FONT_GLYPH_CACHE 1
#endif
#define FONT_GLYPH_COUNT 128u
#define FONT_GLYPH_BYTES 512u
struct font_glyph {
	uint32_t codepoint;
	FT_F26Dot6 size;
	FT_Pos advance;
	unsigned flags;
	int left, top, width, rows;
	bool bold, valid;
	uint8_t mask[FONT_GLYPH_BYTES];
};
#if OV_FONT_GLYPH_CACHE
static struct font_glyph glyphs[FONT_GLYPH_COUNT];
#endif

static void glyph_cache_clear(void)
{
#if OV_FONT_GLYPH_CACHE
	for (unsigned i=0;i<FONT_GLYPH_COUNT;i++) glyphs[i].valid=false;
#endif
}

static unsigned long font_read(FT_Stream stream, unsigned long offset,
			       unsigned char *buffer, unsigned long count)
{
	struct font_file *source=stream->descriptor.pointer;
	if (!count) return offset>stream->size;
	if (!source || !source->file || !buffer || offset>=stream->size) return 0;
	if (count>stream->size-offset) count=stream->size-offset;
	unsigned long copied=0;
	while (copied<count) {
		unsigned long position=offset+copied,tag=position/FONT_PAGE_BYTES;
		unsigned slot=tag%FONT_PAGE_COUNT;
		size_t within=position%FONT_PAGE_BYTES;
		if (!source->lengths[slot] || source->tags[slot]!=tag) {
			source->lengths[slot]=0;
			if (position-within>LONG_MAX || fseek(source->file,(long)(position-within),SEEK_SET)) break;
			size_t bytes=fread(source->pages[slot],1,FONT_PAGE_BYTES,source->file);
			if (!bytes) break;
			source->tags[slot]=tag; source->lengths[slot]=bytes;
		}
		if (within>=source->lengths[slot]) break;
		size_t bytes=source->lengths[slot]-within;
		if (bytes>count-copied) bytes=count-copied;
		memcpy(buffer+copied,source->pages[slot]+within,bytes); copied+=bytes;
	}
	return copied;
}
static void font_close(FT_Stream stream)
{
	struct font_file *source=stream->descriptor.pointer;
	if (source && source->file) { fclose(source->file); source->file=NULL; }
}

static int decode_utf8(const char **cursor, const char *end, uint32_t *codepoint)
{
	const uint8_t *s = (const uint8_t *)*cursor;
	if ((const char *)s >= end)
		return 0;
	unsigned length;
	uint32_t value;
	if (s[0] < 0x80) {
		length = 1;
		value = s[0];
	} else if (s[0] >= 0xc2 && s[0] <= 0xdf) {
		length = 2;
		value = s[0] & 0x1f;
	} else if (s[0] >= 0xe0 && s[0] <= 0xef) {
		length = 3;
		value = s[0] & 0x0f;
	} else if (s[0] >= 0xf0 && s[0] <= 0xf4) {
		length = 4;
		value = s[0] & 0x07;
	} else {
		*cursor += 1;
		*codepoint = 0xfffd;
		return 1;
	}
	if ((size_t)(end - (const char *)s) < length) {
		*cursor += 1;
		*codepoint = 0xfffd;
		return 1;
	}
	for (unsigned i = 1; i < length; i++) {
		if ((s[i] & 0xc0) != 0x80) {
			*cursor += 1;
			*codepoint = 0xfffd;
			return 1;
		}
		value = (value << 6) | (s[i] & 0x3f);
	}
	if ((length == 2 && value < 0x80) ||
	    (length == 3 && value < 0x800) ||
	    (length == 4 && value < 0x10000) ||
	    value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
		*cursor += 1;
		*codepoint = 0xfffd;
		return 1;
	}
	*cursor += length;
	*codepoint = value;
	return 1;
}

static bool set_size(const struct plot_font_style *style)
{
	if (font_face == NULL || style == NULL)
		return false;
	double pixels = (double)style->size / PLOT_STYLE_SCALE * (96.0 / 72.0);
	if (pixels < 1.0)
		pixels = 1.0;
	if (pixels > 256.0)
		pixels = 256.0;
	FT_F26Dot6 size = (FT_F26Dot6)(pixels * 64.0 + 0.5);
	if (size==current_size) return true;
	if (FT_Set_Char_Size(font_face,0,size,72,72)) return false;
	current_size=size; return true;
}

static FT_UInt glyph_index(uint32_t codepoint)
{
	FT_UInt index = FT_Get_Char_Index(font_face, codepoint);
	if (index == 0 && codepoint != 0xfffd)
		index = FT_Get_Char_Index(font_face, 0xfffd);
	return index;
}

static bool load_glyph(const struct plot_font_style *style, uint32_t codepoint)
{
	if (!set_size(style))
		return false;
	FT_UInt index = glyph_index(codepoint);
	if (FT_Load_Glyph(font_face, index, FT_LOAD_DEFAULT) != 0)
		return false;
	if (style->weight >= 600)
		FT_GlyphSlot_Embolden(font_face->glyph);
	if (style->flags & (FONTF_ITALIC | FONTF_OBLIQUE))
		FT_GlyphSlot_Oblique(font_face->glyph);
	return true;
}

static FT_Pos glyph_advance(const struct plot_font_style *style,uint32_t cp)
{
	unsigned flags=style->flags & (FONTF_ITALIC | FONTF_OBLIQUE);
	bool bold=style->weight>=600;
	unsigned slot=(cp*2654435761u ^ (uint32_t)current_size*97u ^ flags*17u ^ (bold?0x13579u:0)) % FONT_METRIC_COUNT;
	struct font_metric *entry=&metrics[slot];
	if (entry->valid && entry->codepoint==cp && entry->size==current_size &&
	    entry->flags==flags && entry->bold==bold) return entry->advance;
	if (!load_glyph(style,cp)) return 0;
	*entry=(struct font_metric){.codepoint=cp,.size=current_size,
		.advance=font_face->glyph->advance.x,.flags=flags,.bold=bold,.valid=true};
	return entry->advance;
}

static uint32_t text_rgb(colour c)
{
	return ((c & 0xffu) << 16) | (c & 0xff00u) | ((c >> 16) & 0xffu);
}

static nserror layout_width(const struct plot_font_style *style,
			    const char *text, size_t length, int *width)
{
	if (text == NULL || width == NULL || length > INT_MAX)
		return NSERROR_BAD_PARAMETER;
	if (!set_size(style))
		return NSERROR_INIT_FAILED;
	int64_t advance = 0;
	const char *p = text, *end = text + length;
	uint32_t cp;
	while (decode_utf8(&p, end, &cp)) {
		advance += glyph_advance(style,cp);
	}
	int64_t px = (advance + 32) / 64;
	if (px > INT_MAX)
		return NSERROR_BAD_SIZE;
	*width = (int)px;
	return NSERROR_OK;
}

static nserror layout_position(const struct plot_font_style *style,
			       const char *text, size_t length, int x,
			       size_t *char_offset, int *actual_x)
{
	if (text == NULL || char_offset == NULL || actual_x == NULL)
		return NSERROR_BAD_PARAMETER;
	if (!set_size(style))
		return NSERROR_INIT_FAILED;
	const char *p = text, *end = text + length;
	int64_t advance = 0;
	uint32_t cp;
	while (p < end) {
		const char *start = p;
		decode_utf8(&p, end, &cp);
		int64_t next = advance;
		next += glyph_advance(style,cp);
		if ((next + 32) / 64 >= x) {
			if (llabs(advance / 64 - x) <= llabs(next / 64 - x))
				p = start;
			else
				advance = next;
			break;
		}
		advance = next;
	}
	*char_offset = (size_t)(p - text);
	*actual_x = (int)((advance + 32) / 64);
	return NSERROR_OK;
}

static bool cjk(uint32_t c)
{
	return (c >= 0x2e80 && c <= 0x9fff) ||
	       (c >= 0xac00 && c <= 0xd7af) ||
	       (c >= 0xf900 && c <= 0xfaff) ||
	       (c >= 0x20000 && c <= 0x3ffff);
}

static bool reading_wrap;
static bool no_line_start(uint32_t c)
{
	return c==0x3001 || c==0x3002 || c==0xff0c || c==0xff01 ||
	       c==0xff1f || c==0xff1a || c==0xff1b || c==0xff09 ||
	       c==0x300b || c==0x3011 || c==0x201d || c==0x2019 ||
	       c==0x300d || c==0x300f || c==0x3015 || c==0x3009;
}
static bool no_line_end(uint32_t c)
{
	return c==0xff08 || c==0x300a || c==0x3010 || c==0x201c ||
	       c==0x2018 || c==0x300c || c==0x300e || c==0x3014 || c==0x3008;
}
void ov_font_set_reading_wrap(bool enabled)
{
	reading_wrap = enabled;
}

static nserror layout_split(const struct plot_font_style *style,
			    const char *text, size_t length, int max_width,
			    size_t *char_offset, int *actual_x)
{
	if (text == NULL || char_offset == NULL || actual_x == NULL)
		return NSERROR_BAD_PARAMETER;
	if (!set_size(style))
		return NSERROR_INIT_FAILED;
	const char *p = text, *end = text + length;
	const char *best = NULL;
	int best_width = 0;
	int64_t advance = 0;
	uint32_t previous = 0, cp;
	const char *previous_start = text;
	int previous_width = 0;
	while (p < end) {
		const char *start = p;
		if (!decode_utf8(&p, end, &cp)) break;
		int before = (int)((advance + 32) / 64);
		/* A break is before this scalar, so both byte offset and measured
		 * width must exclude it. Previously both included the overflow glyph. */
		if (start > text && (cp == ' ' ||
		    (previous != 0 && (cjk(previous) || cjk(cp)) &&
		     !no_line_start(cp) && !no_line_end(previous)))) {
			if (before <= max_width || best == NULL) {
				best = start;
				best_width = before;
			}
		}
		advance += glyph_advance(style,cp);
		int width = (int)((advance + 32) / 64);
		if (width > max_width && best != NULL) {
			*char_offset = (size_t)(best - text);
			*actual_x = best_width;
			return NSERROR_OK;
		}
		if (width > max_width && reading_wrap) {
			if ((no_line_start(cp) || no_line_end(previous)) && previous_start>text) {
				*char_offset=(size_t)(previous_start-text);
				*actual_x=previous_width;
				return NSERROR_OK;
			}
			*char_offset = (size_t)((start > text ? start : p) - text);
			*actual_x = start > text ? before : width;
			return NSERROR_OK;
		}
		previous = cp;
		previous_start = start;
		previous_width = before;
	}
	*char_offset = length;
	*actual_x = (int)((advance + 32) / 64);
	return NSERROR_OK;
}

static struct gui_layout_table layout_table = {
	.width = layout_width,
	.position = layout_position,
	.split = layout_split,
};

bool ov_font_init(const char *path)
{
	if (path == NULL || path[0] == '\0')
		return false;
	if (font_face != NULL)
		return true;
	if (FT_Init_FreeType(&font_library) != 0)
		return false;
	font_file=calloc(1,sizeof(*font_file));
	if (!font_file) goto fail;
	font_file->file=fopen(path,"rb");
	if (!font_file->file || fseek(font_file->file,0,SEEK_END)) goto fail;
	long size=ftell(font_file->file);
	if (size<=0) goto fail;
	font_file->stream=(FT_StreamRec){.size=(unsigned long)size,
		.descriptor.pointer=font_file,.read=font_read,.close=font_close};
	FT_Open_Args args={.flags=FT_OPEN_STREAM,.stream=&font_file->stream};
	if (FT_Open_Face(font_library,&args,0,&font_face)) goto fail;
	current_size=0; memset(metrics,0,sizeof(metrics)); glyph_cache_clear();
	fprintf(stderr,"[browser-font] bounded page cache=%u bytes; metric cache=%zu bytes\n",
		FONT_PAGE_BYTES*FONT_PAGE_COUNT,sizeof(metrics));
	return true;
fail:
	if (font_file) {
		if (font_file->file) fclose(font_file->file);
		free(font_file); font_file=NULL;
	}
	FT_Done_FreeType(font_library); font_library=NULL;
	return false;
}

void ov_font_fini(void)
{
	if (font_face != NULL) {
		FT_Done_Face(font_face);
		font_face = NULL;
	}
	if (font_library != NULL) {
		FT_Done_FreeType(font_library);
		font_library = NULL;
	}
	if (font_file) {
		font_close(&font_file->stream); free(font_file); font_file=NULL;
	}
	current_size=0; memset(metrics,0,sizeof(metrics)); glyph_cache_clear();
}

struct gui_layout_table *ov_font_layout_table(void)
{
	return &layout_table;
}

static const uint8_t *bitmap_row(const FT_Bitmap *bitmap, unsigned row)
{
	return bitmap->buffer +
		(bitmap->pitch>=0?row:bitmap->rows-row-1)*(size_t)abs(bitmap->pitch);
}

static uint8_t bitmap_alpha(const FT_Bitmap *bitmap, const uint8_t *row, unsigned col)
{
	if (bitmap->pixel_mode==FT_PIXEL_MODE_MONO)
		return (row[col>>3] & (0x80u>>(col&7)))?255:0;
	return (uint8_t)(row[col]*255u/(bitmap->num_grays>1?bitmap->num_grays-1:255));
}

static void plot_glyph_bitmap(struct ov_plot_target *target, int x, int y,
		const FT_Bitmap *bitmap, uint32_t rgb)
{
	if (!bitmap->width || !bitmap->rows) return;
	if (bitmap->pixel_mode==FT_PIXEL_MODE_GRAY && bitmap->num_grays==256 && bitmap->pitch>0) {
		ov_plot_blend_mask(target,x,y,bitmap->width,bitmap->rows,
			bitmap->buffer,bitmap->pitch,rgb);
		return;
	}
	uint8_t row[128];
	for (unsigned py=0;py<bitmap->rows;py++) {
		const uint8_t *src=bitmap_row(bitmap,py);
		for (unsigned px=0;px<bitmap->width;px+=sizeof(row)) {
			unsigned count=bitmap->width-px;
			if (count>sizeof(row)) count=sizeof(row);
			for (unsigned col=0;col<count;col++) row[col]=bitmap_alpha(bitmap,src,px+col);
			ov_plot_blend_mask(target,x+px,y+py,count,1,row,count,rgb);
		}
	}
}

bool ov_font_text_plot(struct ov_plot_target *target,
		       const struct plot_font_style *style, int x, int y,
		       const char *text, size_t length)
{
	if (target == NULL || text == NULL || font_face == NULL || !set_size(style))
		return false;
	int64_t pen = (int64_t)x * 64;
	const char *p = text, *end = text + length;
	uint32_t cp;
	while (decode_utf8(&p, end, &cp)) {
		if (cp<=' ') { pen+=glyph_advance(style,cp); continue; }
#if OV_FONT_GLYPH_CACHE
		unsigned flags=style->flags & (FONTF_ITALIC|FONTF_OBLIQUE);
		bool bold=style->weight>=600;
		unsigned index=(cp*2654435761u ^ (uint32_t)current_size*97u ^ flags*17u ^ (bold?0x13579u:0))%FONT_GLYPH_COUNT;
		struct font_glyph *cached=&glyphs[index];
		if (cached->valid && cached->codepoint==cp && cached->size==current_size &&
		    cached->flags==flags && cached->bold==bold) {
			ov_plot_blend_mask(target,(int)(pen/64)+cached->left,y-cached->top,
				cached->width,cached->rows,cached->mask,cached->width,text_rgb(style->foreground));
			pen+=cached->advance;
			continue;
		}
#endif
		if (!load_glyph(style, cp))
			continue;
		FT_GlyphSlot slot = font_face->glyph;
		if (cp > ' ' && FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) == 0) {
			const FT_Bitmap *bitmap = &slot->bitmap;
			if (bitmap->pixel_mode == FT_PIXEL_MODE_GRAY ||
			    bitmap->pixel_mode == FT_PIXEL_MODE_MONO) {
				plot_glyph_bitmap(target,(int)(pen/64)+slot->bitmap_left,
					y-slot->bitmap_top,bitmap,text_rgb(style->foreground));
#if OV_FONT_GLYPH_CACHE
				if (bitmap->rows && bitmap->width && bitmap->width<=FONT_GLYPH_BYTES/bitmap->rows) {
					*cached=(struct font_glyph){.codepoint=cp,.size=current_size,.advance=slot->advance.x,
						.flags=flags,.left=slot->bitmap_left,.top=slot->bitmap_top,
						.width=bitmap->width,.rows=bitmap->rows,.bold=bold,.valid=true};
					for (unsigned row=0;row<bitmap->rows;row++) {
						const uint8_t *src=bitmap_row(bitmap,row);
						for (unsigned col=0;col<bitmap->width;col++)
							cached->mask[(size_t)row*bitmap->width+col]=bitmap_alpha(bitmap,src,col);
					}
				}
#endif
			}
		}
		pen += slot->advance.x;
	}
	return true;
}

static bool canvas_fill_text(uint8_t *pixels, int width, int height, size_t stride,
			     const struct plot_font_style *style, int x, int y,
			     const char *text, size_t length, uint32_t colour_value,
			     float alpha, const int clip[4])
{
	if (pixels == NULL || width <= 0 || height <= 0 ||
	    stride < (size_t)width * 4 || clip == NULL || text == NULL)
		return false;
	if (!set_size(style))
		return false;
	if (alpha < 0.0f) alpha = 0.0f;
	if (alpha > 1.0f) alpha = 1.0f;
	uint8_t red = colour_value & 0xff;
	uint8_t green = (colour_value >> 8) & 0xff;
	uint8_t blue = (colour_value >> 16) & 0xff;
	int64_t pen = (int64_t)x * 64;
	const char *p = text, *end = text + length;
	uint32_t cp;
	while (decode_utf8(&p, end, &cp)) {
		if (load_glyph(style, cp)) {
			FT_GlyphSlot slot = font_face->glyph;
			if (cp > ' ' && FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) == 0 &&
			    slot->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
				const FT_Bitmap *bitmap = &slot->bitmap;
				for (unsigned row = 0; row < bitmap->rows; row++) {
					const uint8_t *src = bitmap->buffer +
						(bitmap->pitch >= 0 ? row : bitmap->rows - row - 1) *
						(size_t)abs(bitmap->pitch);
					int py = y - slot->bitmap_top + row;
					if (py < 0 || py >= height || py < clip[1] || py >= clip[3])
						continue;
					for (unsigned col = 0; col < bitmap->width; col++) {
						int px = (int)(pen / 64) + slot->bitmap_left + col;
						if (px < 0 || px >= width || px < clip[0] || px >= clip[2])
							continue;
						unsigned a = (unsigned)(src[col] * alpha + 0.5f);
						uint8_t *dst = pixels + (size_t)py * stride + (size_t)px * 4;
						unsigned old_a = dst[3];
						unsigned out_a = a + (old_a * (255 - a) + 127) / 255;
						if (out_a != 0) {
							dst[0] = (uint8_t)((red * a * 255 + dst[0] * old_a * (255 - a) + out_a * 127) / (out_a * 255));
							dst[1] = (uint8_t)((green * a * 255 + dst[1] * old_a * (255 - a) + out_a * 127) / (out_a * 255));
							dst[2] = (uint8_t)((blue * a * 255 + dst[2] * old_a * (255 - a) + out_a * 127) / (out_a * 255));
						}
						dst[3] = (uint8_t)out_a;
					}
				}
			}
			pen += slot->advance.x;
		}
	}
	return true;
}

static struct gui_canvas_table canvas_table = {.fill_text = canvas_fill_text};

struct gui_canvas_table *ov_font_canvas_table(void)
{
	return &canvas_table;
}
