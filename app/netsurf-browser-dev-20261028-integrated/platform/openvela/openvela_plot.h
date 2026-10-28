/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NETSURF_OPENVELA_PLOT_H
#define NETSURF_OPENVELA_PLOT_H

#include <stdbool.h>
#include <stdint.h>

#include "utils/errors.h"
#include "netsurf/plotters.h"

struct plot_font_style;

struct ov_plot_target {
	uint32_t *pixels;
	int width;
	int height;
	int stride;
	int clip_x0;
	int clip_y0;
	int clip_x1;
	int clip_y1;
};

void ov_plot_target_init(struct ov_plot_target *target, uint32_t *pixels,
			 int width, int height, int stride);
void ov_plot_blend_mask(struct ov_plot_target *target, int x, int y,
			int width, int height, const uint8_t *mask,
			int mask_stride, uint32_t rgb);
void ov_plot_bitmap_cache_invalidate(const void *bitmap);
void ov_plot_bitmap_cache_clear(void);

extern const struct plotter_table ov_plotters;

#endif
