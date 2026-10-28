/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NETSURF_OPENVELA_WINDOW_H
#define NETSURF_OPENVELA_WINDOW_H

#include "openvela_lvgl_view.h"
#include "utils/errors.h"

struct browser_window;
struct gui_window_table;
struct plotter_table;

typedef bool (*ov_plotter_surface_bind_fn)(void *ctx, void *plotter_priv,
						 uint32_t *pixels, int width,
						 int height, int stride_pixels);

struct ov_window_ops {
	/* These callbacks run on the NetSurf owner thread and must not call LVGL. */
	void (*redraw_requested)(void *ctx);
	void (*title_changed)(void *ctx, const char *title);
	void (*url_changed)(void *ctx, const char *url);
	void (*status_changed)(void *ctx, const char *status);
	void (*loading_changed)(void *ctx, bool loading);
	void (*scroll_changed)(void *ctx, int x, int y);
	/* NULL revokes the previous request. */
	void (*input_changed)(void *ctx, const struct ov_input_request *request);
	void *ctx;
};

/* Bind one root browser window. The OpenVela app owns the NetSurf core loop. */
nserror ov_window_init(int width, int height,
		      const struct ov_window_ops *ops);
/* Fails while NetSurf still owns a gui_window; destroy the browser first. */
nserror ov_window_fini(void);
nserror ov_window_resize(int width, int height);
bool ov_window_scroll_by(int delta_x, int delta_y);
struct gui_window_table *ov_window_table(void);
struct browser_window *ov_window_browser(void);
/* Owner thread only; distinguish generated failure documents from normal HTML. */
bool ov_window_is_error_page(void);
/* Owner thread only. Repaint without navigation, DOM edits or reformatting. */
void ov_window_set_images_visible(bool visible);
/* Owner thread, after scheduled layout work and before drawing. */
void ov_window_update_layout(void);
/* 0 restores automatic fit; otherwise 25..300 percent. Owner thread only. */
void ov_window_set_zoom(int percent);
int ov_window_zoom(void);
void ov_window_set_reading_enabled(bool enabled);
void ov_window_reading_state(bool *active, bool *available);

/* Consume invalidated page regions on the NetSurf owner thread. */
bool ov_window_take_damage(struct ov_damage *damage);

/* Dispatch one event on the NetSurf owner thread. */
nserror ov_window_dispatch(const struct ov_event *event);

/* Full viewport redraw in vertical strips. render_pixels is a caller-owned
 * XRGB8888 scratch surface with render_rows capacity. bind_surface must bind
 * plotter_priv to each scratch strip before NetSurf draws it. */
bool ov_window_render(struct ov_lvgl_view *view,
		      uint32_t *render_pixels, int render_stride,
		      int render_rows,
		      ov_plotter_surface_bind_fn bind_surface,
		      void *bind_ctx,
		      const struct plotter_table *plotters,
		      void *plotter_priv,
		      uint64_t serial);

#endif
