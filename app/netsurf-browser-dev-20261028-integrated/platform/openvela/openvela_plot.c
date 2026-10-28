/* Software plotters targeting a bounded XRGB8888 strip. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
#include "netsurf/layout.h"
#include "netsurf/plot_style.h"
#include "netsurf/plotters.h"
#include "openvela_bitmap.h"
#include "openvela_font.h"
#include "libnsfb.h"
#include "libnsfb_plot.h"
#include "framebuffer/fbfx.h"

#include "openvela_plot.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* ------------------------------------------------------------- libnsfb --- */

static nsfb_t fx_surface;

nsfb_t *framebuffer_current_surface(void)
{
	return &fx_surface;
}

int nsfb_get_geometry(nsfb_t *surface, int *width, int *height, enum nsfb_format_e *format)
{
	if (surface == NULL)
		return -1;
	*width = surface->width;
	*height = surface->height;
	*format = surface->format;
	return 0;
}

int nsfb_get_buffer(nsfb_t *surface, uint8_t **pixels, int *stride)
{
	if (surface == NULL)
		return -1;
	*pixels = surface->pixels;
	*stride = surface->stride;
	return 0;
}

int nsfb_plot_get_clip(nsfb_t *surface, nsfb_bbox_t *clip)
{
	if (surface == NULL)
		return -1;
	*clip = surface->clip;
	return 0;
}

static void fx_bind(const struct redraw_context *ctx)
{
	const struct ov_plot_target *t = ctx->priv;
	fx_surface.pixels = (uint8_t *)t->pixels;
	fx_surface.width = t->width;
	fx_surface.height = t->height;
	fx_surface.stride = t->stride * 4;
	fx_surface.format = NSFB_FMT_XRGB8888;
	fx_surface.clip = (nsfb_bbox_t){t->clip_x0, t->clip_y0, t->clip_x1, t->clip_y1};
}

/* ------------------------------------------------------------- helpers --- */


void ov_plot_target_init(struct ov_plot_target *target, uint32_t *pixels,
		     int width, int height, int stride)
{
	target->pixels = pixels;
	target->width = width;
	target->height = height;
	target->stride = stride;
	target->clip_x0 = 0;
	target->clip_y0 = 0;
	target->clip_x1 = width;
	target->clip_y1 = height;
}

/* NetSurf colour is 0xTTBBGGRR with TT = transparency (0 = opaque). */
static inline uint32_t to_rgb(colour c)
{
	return ((c & 0xffu) << 16) | (c & 0xff00u) | ((c >> 16) & 0xffu);
}

static inline unsigned alpha_of(colour c)
{
	return 255u - (c >> 24);
}

/* Channel-wise (px * inv) / 255 with exact rounding, for all four bytes. */
static inline uint32_t mul255(uint32_t px, uint32_t inv)
{
	uint32_t rb = (px & 0x00ff00ffu) * inv + 0x00800080u;
	rb = ((rb + ((rb >> 8) & 0x00ff00ffu)) >> 8) & 0x00ff00ffu;
	uint32_t ag = ((px >> 8) & 0x00ff00ffu) * inv + 0x00800080u;
	ag = (ag + ((ag >> 8) & 0x00ff00ffu)) & 0xff00ff00u;
	return rb | ag;
}

static inline uint32_t blend_premul(uint32_t dst, uint32_t premul, uint32_t alpha)
{
	return premul + mul255(dst, 255u - alpha);
}

static void fill_span(uint32_t *dst, int count, uint32_t rgb, unsigned alpha)
{
	if (alpha == 0)
		return;
	if (alpha >= 255) {
		for (int i = 0; i < count; i++)
			dst[i] = rgb;
		return;
	}
	uint32_t premul = mul255(rgb, alpha);
	uint32_t inv = 255u - alpha;
	for (int i = 0; i < count; i++)
		dst[i] = premul + mul255(dst[i], inv);
}

static void fill_rect(const struct ov_plot_target *t, int x0, int y0, int x1, int y1,
		      uint32_t rgb, unsigned alpha)
{
	x0 = MAX(x0, t->clip_x0);
	y0 = MAX(y0, t->clip_y0);
	x1 = MIN(x1, t->clip_x1);
	y1 = MIN(y1, t->clip_y1);
	if (x0 >= x1 || y0 >= y1)
		return;
	for (int y = y0; y < y1; y++)
		fill_span(t->pixels + (size_t)y * t->stride + x0, x1 - x0, rgb, alpha);
}

void ov_plot_blend_mask(struct ov_plot_target *t, int x, int y, int width, int height,
		    const uint8_t *mask, int mask_stride, uint32_t rgb)
{
	int x0 = MAX(x, t->clip_x0), y0 = MAX(y, t->clip_y0);
	int x1 = MIN(x + width, t->clip_x1), y1 = MIN(y + height, t->clip_y1);
	if (x0 >= x1 || y0 >= y1)
		return;
	for (int py = y0; py < y1; py++) {
		const uint8_t *src = mask + (size_t)(py - y) * mask_stride + (x0 - x);
		uint32_t *dst = t->pixels + (size_t)py * t->stride + x0;
		for (int i = 0, n = x1 - x0; i < n; i++) {
			unsigned a = src[i];
			if (a == 0)
				continue;
			dst[i] = a == 255 ? rgb : mul255(rgb, a) + mul255(dst[i], 255u - a);
		}
	}
}

static int stroke_px(const plot_style_t *style)
{
	int w = (int)(style->stroke_width >> PLOT_STYLE_RADIX);
	return w < 1 ? 1 : w;
}

/* ------------------------------------------------------------- plotters --- */

static nserror plot_clip(const struct redraw_context *ctx, const struct rect *clip)
{
	struct ov_plot_target *t = ctx->priv;
	t->clip_x0 = MAX(0, clip->x0);
	t->clip_y0 = MAX(0, clip->y0);
	t->clip_x1 = MIN(t->width, clip->x1);
	t->clip_y1 = MIN(t->height, clip->y1);
	return NSERROR_OK;
}

/* Dashed horizontal or vertical run [a, b) on the line at `pos`, `w` thick. */
static void dashed_run(const struct ov_plot_target *t, bool horizontal, int a, int b,
		       int pos, int w, plot_operation_type_t type, uint32_t rgb, unsigned alpha)
{
	int on = type == PLOT_OP_TYPE_DASH ? 3 * w : w;
	int off = type == PLOT_OP_TYPE_DASH ? 2 * w : w;
	if (type == PLOT_OP_TYPE_SOLID) {
		on = b - a;
		off = 0;
	}
	if (on < 1)
		on = 1;
	for (int p = a; p < b; p += on + off) {
		int e = MIN(p + on, b);
		if (horizontal)
			fill_rect(t, p, pos, e, pos + w, rgb, alpha);
		else
			fill_rect(t, pos, p, pos + w, e, rgb, alpha);
	}
}

static nserror plot_line(const struct redraw_context *ctx, const plot_style_t *style,
			 const struct rect *line)
{
	const struct ov_plot_target *t = ctx->priv;
	if (style->stroke_type == PLOT_OP_TYPE_NONE)
		return NSERROR_OK;
	uint32_t rgb = to_rgb(style->stroke_colour);
	unsigned alpha = alpha_of(style->stroke_colour);
	int w = stroke_px(style);
	int x0 = line->x0, y0 = line->y0, x1 = line->x1, y1 = line->y1;

	if (y0 == y1) {
		if (x0 > x1) { int s = x0; x0 = x1; x1 = s; }
		dashed_run(t, true, x0, x1 + 1, y0 - w / 2, w, style->stroke_type, rgb, alpha);
		return NSERROR_OK;
	}
	if (x0 == x1) {
		if (y0 > y1) { int s = y0; y0 = y1; y1 = s; }
		dashed_run(t, false, y0, y1 + 1, x0 - w / 2, w, style->stroke_type, rgb, alpha);
		return NSERROR_OK;
	}

	/* Diagonal: Bresenham with a square brush. */
	int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
	int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
	int error = dx + dy, step = 0;
	int period = style->stroke_type == PLOT_OP_TYPE_SOLID ? 1 : (style->stroke_type == PLOT_OP_TYPE_DASH ? 5 * w : 2 * w);
	for (;;) {
		if (style->stroke_type == PLOT_OP_TYPE_SOLID || (step % period) < period / 2 + (period & 1))
			fill_rect(t, x0 - w / 2, y0 - w / 2, x0 - w / 2 + w, y0 - w / 2 + w, rgb, alpha);
		if (x0 == x1 && y0 == y1)
			break;
		int e2 = error * 2;
		if (e2 >= dy) { error += dy; x0 += sx; }
		if (e2 <= dx) { error += dx; y0 += sy; }
		step++;
	}
	return NSERROR_OK;
}

static nserror plot_rectangle(const struct redraw_context *ctx, const plot_style_t *style,
			      const struct rect *r)
{
	const struct ov_plot_target *t = ctx->priv;
	if (style->fill_type != PLOT_OP_TYPE_NONE)
		fill_rect(t, r->x0, r->y0, r->x1, r->y1,
			  to_rgb(style->fill_colour), alpha_of(style->fill_colour));
	if (style->stroke_type != PLOT_OP_TYPE_NONE) {
		uint32_t rgb = to_rgb(style->stroke_colour);
		unsigned alpha = alpha_of(style->stroke_colour);
		int w = stroke_px(style);
		int width = r->x1 - r->x0, height = r->y1 - r->y0;
		if (w * 2 > width || w * 2 > height) {
			fill_rect(t, r->x0, r->y0, r->x1, r->y1, rgb, alpha);
		} else {
			plot_operation_type_t k = style->stroke_type;
			dashed_run(t, true, r->x0, r->x1, r->y0, w, k, rgb, alpha);
			dashed_run(t, true, r->x0, r->x1, r->y1 - w, w, k, rgb, alpha);
			dashed_run(t, false, r->y0 + w, r->y1 - w, r->x0, w, k, rgb, alpha);
			dashed_run(t, false, r->y0 + w, r->y1 - w, r->x1 - w, w, k, rgb, alpha);
		}
	}
	return NSERROR_OK;
}

static const float identity[6] = {1, 0, 0, 1, 0, 0};

static nserror plot_polygon(const struct redraw_context *ctx, const plot_style_t *style,
			    const int *p, unsigned n)
{
	if (n < 3)
		return NSERROR_OK;
	float *path = malloc(sizeof(float) * (n * 3 + 1));
	if (path == NULL)
		return NSERROR_NOMEM;
	unsigned k = 0;
	for (unsigned i = 0; i < n; i++) {
		path[k++] = i == 0 ? (float)PLOTTER_PATH_MOVE : (float)PLOTTER_PATH_LINE;
		path[k++] = (float)p[i * 2];
		path[k++] = (float)p[i * 2 + 1];
	}
	path[k++] = (float)PLOTTER_PATH_CLOSE;
	fx_bind(ctx);
	nserror error = fbfx_path(ctx, style, path, k, identity);
	free(path);
	return error;
}

static nserror plot_path(const struct redraw_context *ctx, const plot_style_t *style,
			 const float *p, unsigned n, const float transform[6])
{
	fx_bind(ctx);
	return fbfx_path(ctx, style, p, n, transform);
}

#define KAPPA 0.5522847498f

static nserror plot_disc(const struct redraw_context *ctx, const plot_style_t *style,
			 int x, int y, int radius)
{
	float cx = (float)x, cy = (float)y, r = (float)radius, k = r * KAPPA;
	const float path[] = {
		PLOTTER_PATH_MOVE, cx + r, cy,
		PLOTTER_PATH_BEZIER, cx + r, cy + k, cx + k, cy + r, cx, cy + r,
		PLOTTER_PATH_BEZIER, cx - k, cy + r, cx - r, cy + k, cx - r, cy,
		PLOTTER_PATH_BEZIER, cx - r, cy - k, cx - k, cy - r, cx, cy - r,
		PLOTTER_PATH_BEZIER, cx + k, cy - r, cx + r, cy - k, cx + r, cy,
		PLOTTER_PATH_CLOSE,
	};
	fx_bind(ctx);
	return fbfx_path(ctx, style, path, sizeof(path) / sizeof(path[0]), identity);
}

static nserror plot_arc(const struct redraw_context *ctx, const plot_style_t *style,
			int x, int y, int radius, int angle1, int angle2)
{
	/* Anticlockwise from angle1 to angle2 (degrees, y up) -> screen y down. */
	while (angle2 <= angle1)
		angle2 += 360;
	float path[3 + 7 * 5];
	unsigned n = 0;
	float a = (float)angle1 * 3.14159265f / 180.0f;
	float end = (float)angle2 * 3.14159265f / 180.0f;
	float r = (float)radius;
	path[n++] = PLOTTER_PATH_MOVE;
	path[n++] = x + r * cosf(a);
	path[n++] = y - r * sinf(a);
	while (a < end - 1e-4f && n + 7 <= sizeof(path) / sizeof(path[0])) {
		float b = MIN(a + 1.5707963f, end);
		float h = 4.0f / 3.0f * tanf((b - a) / 4.0f);
		float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b);
		path[n++] = PLOTTER_PATH_BEZIER;
		path[n++] = x + r * (ca - h * sa);
		path[n++] = y - r * (sa + h * ca);
		path[n++] = x + r * (cb + h * sb);
		path[n++] = y - r * (sb - h * cb);
		path[n++] = x + r * cb;
		path[n++] = y - r * sb;
		a = b;
	}
	fx_bind(ctx);
	return fbfx_path(ctx, style, path, n, identity);
}

/* -------------------------------------------------------------- bitmaps --- */

/* Scaled copies are cached as premultiplied ARGB.  Each entry remembers the
 * source version so an animated/modified bitmap is re-converted. */
struct cache_entry {
	const struct bitmap *source;
	uint32_t version;
	int width, height;
	uint32_t *pixels;
	size_t bytes;
	uint64_t used;
	bool opaque;
};

#define CACHE_SLOTS 256
#define CACHE_LIMIT ((size_t)1 << 20)
#define CACHE_ENTRY_LIMIT ((size_t)1 << 20)
#define RESAMPLE_SCRATCH_LIMIT ((size_t)1 << 20)

static struct cache_entry cache[CACHE_SLOTS];
static size_t cache_bytes;
static uint64_t cache_clock;

static void cache_drop(struct cache_entry *e)
{
	if (e->pixels != NULL) {
		cache_bytes -= e->bytes;
		free(e->pixels);
	}
	memset(e, 0, sizeof(*e));
}

void ov_plot_bitmap_cache_invalidate(const void *bitmap)
{
	for (int i = 0; i < CACHE_SLOTS; i++)
		if (cache[i].pixels != NULL && cache[i].source == bitmap)
			cache_drop(&cache[i]);
}

void ov_plot_bitmap_cache_clear(void)
{
	for (int i = 0; i < CACHE_SLOTS; i++)
		cache_drop(&cache[i]);
	cache_clock = 0;
}

/* Premultiply one R,G,B,A pixel into 0xAARRGGBB. */
static inline uint32_t pack_premul(const uint8_t *p)
{
	uint32_t a = p[3];
	uint32_t rgb = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
	if (a == 255)
		return 0xff000000u | rgb;
	if (a == 0)
		return 0;
	return (a << 24) | (mul255(rgb, a) & 0x00ffffffu);
}

struct taps {
	int *start;
	int *count;
	int *offset;
	uint16_t *weight; /* fixed point, each pixel's taps sum to 4096 */
	int max;
};

static void taps_free(struct taps *t)
{
	free(t->start);
	free(t->count);
	free(t->offset);
	free(t->weight);
}

static bool taps_build(struct taps *t, int src, int dst)
{
	memset(t, 0, sizeof(*t));
	t->max = dst < src ? (src + dst - 1) / dst + 2 : 2;
	t->start = malloc(sizeof(int) * dst);
	t->count = malloc(sizeof(int) * dst);
	t->offset = malloc(sizeof(int) * dst);
	t->weight = malloc(sizeof(uint16_t) * (size_t)dst * t->max);
	if (!t->start || !t->count || !t->offset || !t->weight) {
		taps_free(t);
		return false;
	}
	double ratio = (double)src / (double)dst;
	double *w = malloc(sizeof(double) * (size_t)t->max);
	if (w == NULL) {
		taps_free(t);
		return false;
	}
	for (int i = 0; i < dst; i++) {
		int first, n;
		if (dst < src) { /* area average */
			double s0 = i * ratio, s1 = (i + 1) * ratio;
			first = (int)floor(s0);
			int last = MIN((int)ceil(s1) - 1, src - 1);
			n = MIN(last - first + 1, t->max);
			for (int k = 0; k < n; k++) {
				double lo = MAX(s0, first + k), hi = MIN(s1, first + k + 1);
				w[k] = MAX(hi - lo, 0.0) / (s1 - s0);
			}
		} else { /* bilinear */
			double c = (i + 0.5) * ratio - 0.5;
			int i0 = (int)floor(c);
			double f = c - i0;
			first = i0;
			n = 2;
			w[0] = 1.0 - f;
			w[1] = f;
			if (first < 0) { /* clamp: fold the off-image tap into pixel 0 */
				w[1] += w[0];
				first = 0;
				n = 1;
				w[0] = w[1];
			} else if (first + 1 >= src) {
				n = 1;
				w[0] = 1.0;
			}
		}
		int sum = 0, biggest = 0;
		uint16_t *out = t->weight + (size_t)i * t->max;
		for (int k = 0; k < n; k++) {
			int q = (int)(w[k] * 4096.0 + 0.5);
			out[k] = (uint16_t)q;
			sum += q;
			if (q > out[biggest])
				biggest = k;
		}
		out[biggest] = (uint16_t)((int)out[biggest] + 4096 - sum);
		t->start[i] = first;
		t->count[i] = n;
		t->offset[i] = i * t->max;
	}
	free(w);
	return true;
}

static uint32_t *scale_bitmap(const struct bitmap *b, int dw, int dh, bool *opaque)
{
	size_t bytes = (size_t)dw * dh * 4;
	uint32_t *out = malloc(bytes);
	if (out == NULL)
		return NULL;
	*opaque = b->opaque;

	if (dw == b->width && dh == b->height) {
		for (int y = 0; y < dh; y++) {
			const uint8_t *row = b->pixels + (size_t)y * b->stride;
			uint32_t *o = out + (size_t)y * dw;
			if (b->opaque)
				for (int x = 0; x < dw; x++)
					o[x] = 0xff000000u | ((uint32_t)row[x * 4] << 16) |
					       ((uint32_t)row[x * 4 + 1] << 8) | row[x * 4 + 2];
			else
				for (int x = 0; x < dw; x++)
					o[x] = pack_premul(row + x * 4);
		}
		return out;
	}

	struct taps hx, vy;
	if (!taps_build(&hx, b->width, dw)) {
		free(out);
		return NULL;
	}
	if (!taps_build(&vy, b->height, dh)) {
		taps_free(&hx);
		free(out);
		return NULL;
	}
	/* Horizontal pass into 4 premultiplied channels per pixel. */
	if ((size_t)dw > SIZE_MAX / (size_t)b->height / 4 ||
	    (size_t)dw * (size_t)b->height * 4 > RESAMPLE_SCRATCH_LIMIT) {
		free(out);
		taps_free(&hx);
		taps_free(&vy);
		return NULL;
	}
	uint8_t *mid = malloc((size_t)dw * (size_t)b->height * 4);
	uint32_t *acc = malloc(sizeof(uint32_t) * (size_t)dw * 4);
	if (mid == NULL || acc == NULL) {
		free(mid); free(acc); free(out);
		taps_free(&hx); taps_free(&vy);
		return NULL;
	}
	bool any_alpha = false;
	for (int y = 0; y < b->height; y++) {
		const uint8_t *row = b->pixels + (size_t)y * b->stride;
		uint8_t *m = mid + (size_t)y * dw * 4;
		for (int x = 0; x < dw; x++) {
			const uint16_t *w = hx.weight + hx.offset[x];
			uint32_t r = 0, g = 0, bl = 0, a = 0;
			for (int k = 0; k < hx.count[x]; k++) {
				const uint8_t *p = row + (size_t)(hx.start[x] + k) * 4;
				uint32_t pa = b->opaque ? 255u : p[3];
				uint32_t wk = w[k];
				if (pa == 255) {
					r += p[0] * wk; g += p[1] * wk; bl += p[2] * wk; a += 255u * wk;
				} else {
					any_alpha = true;
					r += ((p[0] * pa + 127) / 255) * wk;
					g += ((p[1] * pa + 127) / 255) * wk;
					bl += ((p[2] * pa + 127) / 255) * wk;
					a += pa * wk;
				}
			}
			m[x * 4] = (uint8_t)((r + 2048) >> 12);
			m[x * 4 + 1] = (uint8_t)((g + 2048) >> 12);
			m[x * 4 + 2] = (uint8_t)((bl + 2048) >> 12);
			m[x * 4 + 3] = (uint8_t)((a + 2048) >> 12);
		}
	}
	/* Vertical pass. */
	for (int y = 0; y < dh; y++) {
		memset(acc, 0, sizeof(uint32_t) * (size_t)dw * 4);
		const uint16_t *w = vy.weight + vy.offset[y];
		for (int k = 0; k < vy.count[y]; k++) {
			const uint8_t *m = mid + (size_t)(vy.start[y] + k) * dw * 4;
			uint32_t wk = w[k];
			for (int i = 0; i < dw * 4; i++)
				acc[i] += m[i] * wk;
		}
		uint32_t *o = out + (size_t)y * dw;
		for (int x = 0; x < dw; x++) {
			uint32_t r = (acc[x * 4] + 2048) >> 12, g = (acc[x * 4 + 1] + 2048) >> 12;
			uint32_t bl = (acc[x * 4 + 2] + 2048) >> 12, a = (acc[x * 4 + 3] + 2048) >> 12;
			o[x] = (a << 24) | (r << 16) | (g << 8) | bl;
		}
	}
	*opaque = b->opaque || !any_alpha;
	free(mid);
	free(acc);
	taps_free(&hx);
	taps_free(&vy);
	return out;
}

static struct cache_entry *cache_get(const struct bitmap *b, int w, int h)
{
	if (w <= 0 || h <= 0 || (size_t)w > CACHE_ENTRY_LIMIT / 4 / (size_t)h)
		return NULL;
	struct cache_entry *free_slot = NULL, *oldest = NULL;
	for (int i = 0; i < CACHE_SLOTS; i++) {
		struct cache_entry *e = &cache[i];
		if (e->pixels == NULL) {
			if (free_slot == NULL)
				free_slot = e;
			continue;
		}
		if (e->source == b && e->version == b->version && e->width == w && e->height == h) {
			e->used = ++cache_clock;
			return e;
		}
		if (oldest == NULL || e->used < oldest->used)
			oldest = e;
	}
	size_t bytes = (size_t)w * h * 4;
	while (cache_bytes + bytes > CACHE_LIMIT) {
		struct cache_entry *victim = NULL;
		for (int i = 0; i < CACHE_SLOTS; i++)
			if (cache[i].pixels && (victim == NULL || cache[i].used < victim->used))
				victim = &cache[i];
		if (victim == NULL)
			break;
		if (victim == free_slot)
			free_slot = NULL;
		cache_drop(victim);
	}
	if (free_slot == NULL) {
		cache_drop(oldest);
		free_slot = oldest;
	}
	bool opaque;
	uint32_t *pixels = scale_bitmap(b, w, h, &opaque);
	if (pixels == NULL)
		return NULL;
	*free_slot = (struct cache_entry){
		.source = b, .version = b->version, .width = w, .height = h,
		.pixels = pixels, .bytes = bytes, .used = ++cache_clock, .opaque = opaque,
	};
	cache_bytes += bytes;
	return free_slot;
}

static void blit_entry(const struct ov_plot_target *t, const struct cache_entry *e,
		       int x, int y, int x0, int y0, int x1, int y1)
{
	for (int py = y0; py < y1; py++) {
		int sy = (int)(((int64_t)py - y) % e->height);
		if (sy < 0)
			sy += e->height;
		const uint32_t *row = e->pixels + (size_t)sy * e->width;
		uint32_t *dst = t->pixels + (size_t)py * t->stride;
		int px = x0;
		while (px < x1) {
			int sx = (int)(((int64_t)px - x) % e->width);
			if (sx < 0)
				sx += e->width;
			int run = MIN(x1 - px, e->width - sx);
			const uint32_t *s = row + sx;
			uint32_t *d = dst + px;
			if (e->opaque) {
				memcpy(d, s, (size_t)run * 4);
			} else {
				for (int i = 0; i < run; i++) {
					uint32_t p = s[i], a = p >> 24;
					if (a == 255)
						d[i] = p;
					else if (a != 0)
						d[i] = blend_premul(d[i], p, a);
				}
			}
			px += run;
		}
	}
}

/* Nearest-neighbour fallback for sizes too large to cache. */
static void blit_direct(const struct ov_plot_target *t, const struct bitmap *b, int x, int y,
			int width, int height, int x0, int y0, int x1, int y1)
{
	for (int py = y0; py < y1; py++) {
		int dy = (int)(((int64_t)py - y) % height);
		if (dy < 0)
			dy += height;
		const uint8_t *row = b->pixels + (size_t)((int64_t)dy * b->height / height) * b->stride;
		uint32_t *dst = t->pixels + (size_t)py * t->stride;
		for (int px = x0; px < x1; px++) {
			int dx = (int)(((int64_t)px - x) % width);
			if (dx < 0)
				dx += width;
			const uint8_t *p = row + (size_t)((int64_t)dx * b->width / width) * 4;
			uint32_t a = b->opaque ? 255u : p[3];
			uint32_t rgb = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
			if (a == 255)
				dst[px] = rgb;
			else if (a != 0)
				dst[px] = mul255(rgb, a) + mul255(dst[px], 255u - a);
		}
	}
}

static nserror plot_bitmap(const struct redraw_context *ctx, struct bitmap *bitmap,
			   int x, int y, int width, int height, colour bg, bitmap_flags_t flags)
{
	(void)bg;
	const struct ov_plot_target *t = ctx->priv;
	if (bitmap == NULL || bitmap->pixels == NULL || width <= 0 || height <= 0)
		return NSERROR_OK;

	/* mask-image: draw the bitmap's alpha in the current tint colour. */
	fx_bind(ctx);
	nsfb_t mask = {bitmap->pixels, bitmap->width, bitmap->height, (int)bitmap->stride,
		       NSFB_FMT_ABGR8888, {0, 0, bitmap->width, bitmap->height}};
	if (fbfx_tint_bitmap(&mask, x, y, width, height))
		return NSERROR_OK;

	int x0 = (flags & BITMAPF_REPEAT_X) ? t->clip_x0 : MAX(x, t->clip_x0);
	int x1 = (flags & BITMAPF_REPEAT_X) ? t->clip_x1 : MIN((int)MIN((int64_t)x + width, INT32_MAX), t->clip_x1);
	int y0 = (flags & BITMAPF_REPEAT_Y) ? t->clip_y0 : MAX(y, t->clip_y0);
	int y1 = (flags & BITMAPF_REPEAT_Y) ? t->clip_y1 : MIN((int)MIN((int64_t)y + height, INT32_MAX), t->clip_y1);
	if (x0 >= x1 || y0 >= y1)
		return NSERROR_OK;

	struct cache_entry *e = cache_get(bitmap, width, height);
	if (e != NULL)
		blit_entry(t, e, x, y, x0, y0, x1, y1);
	else
		blit_direct(t, bitmap, x, y, width, height, x0, y0, x1, y1);
	return NSERROR_OK;
}

static nserror plot_text(const struct redraw_context *ctx, const plot_font_style_t *style,
			 int x, int y, const char *text, size_t length)
{
	return ov_font_text_plot(ctx->priv, style, x, y, text, length) ?
		NSERROR_OK : NSERROR_NOMEM;
}

/* ------------------------------------------------------ modern CSS ops --- */

static nserror x_rounded_fill(const struct redraw_context *ctx, const struct rect *outer,
			      const struct plot_radii *oradii, const struct rect *inner,
			      const struct plot_radii *iradii, colour c)
{
	fx_bind(ctx);
	return fbfx_rounded_fill(ctx, outer, oradii, inner, iradii, c);
}

static nserror x_gradient(const struct redraw_context *ctx, const struct rect *area,
			  const struct plot_gradient *gradient)
{
	fx_bind(ctx);
	return fbfx_gradient(ctx, area, gradient);
}

static nserror x_shadow(const struct redraw_context *ctx, const struct rect *box,
			const struct plot_radii *radii, const struct plot_shadow *shadow)
{
	fx_bind(ctx);
	return fbfx_shadow(ctx, box, radii, shadow);
}

static nserror x_layer_begin(const struct redraw_context *ctx, const struct rect *area)
{
	fx_bind(ctx);
	return fbfx_layer_begin(ctx, area);
}

static nserror x_layer_end(const struct redraw_context *ctx, const struct plot_layer *layer)
{
	fx_bind(ctx);
	return fbfx_layer_end(ctx, layer);
}

static nserror x_tint(const struct redraw_context *ctx, bool enable, colour c)
{
	fx_bind(ctx);
	return fbfx_tint(ctx, enable, c);
}

const struct plotter_table ov_plotters = {
	.clip = plot_clip,
	.arc = plot_arc,
	.disc = plot_disc,
	.line = plot_line,
	.rectangle = plot_rectangle,
	.polygon = plot_polygon,
	.path = plot_path,
	.bitmap = plot_bitmap,
	.text = plot_text,
	.group_start = NULL,
	.group_end = NULL,
	.flush = NULL,
	.rounded_fill = x_rounded_fill,
	.gradient = x_gradient,
	.shadow = x_shadow,
	.layer_begin = x_layer_begin,
	.layer_end = x_layer_end,
	.tint = x_tint,
	.option_knockout = false,
};
