/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NETSURF_OPENVELA_FONT_H
#define NETSURF_OPENVELA_FONT_H

#include <stdbool.h>
#include <stddef.h>

#include "utils/errors.h"
#include "netsurf/layout.h"

struct ov_plot_target;
struct gui_canvas_table;

/* The target supplies a font file from a read-only filesystem or flash. */
bool ov_font_init(const char *path);
void ov_font_fini(void);
/* Owner-thread article presentation: emergency breaks for long URL/words. */
void ov_font_set_reading_wrap(bool enabled);
struct gui_layout_table *ov_font_layout_table(void);
struct gui_canvas_table *ov_font_canvas_table(void);
bool ov_font_text_plot(struct ov_plot_target *target,
			const struct plot_font_style *style, int x, int y,
			const char *text, size_t length);

#endif
