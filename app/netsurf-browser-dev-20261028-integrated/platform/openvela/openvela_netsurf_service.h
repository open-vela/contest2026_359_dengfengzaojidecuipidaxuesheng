/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NETSURF_OPENVELA_SERVICE_H
#define NETSURF_OPENVELA_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "openvela_lvgl_view.h"
#include "openvela_qpk_netsurf.h"
#include "openvela_schedule.h"
#include "openvela_script.h"
#include "openvela_window.h"

#include "desktop/gui_table.h"
#include "netsurf/browser_window.h"
#include "utils/errors.h"

struct ov_netsurf_service;

struct ov_netsurf_service_config {
	int width;
	int height;
	int render_rows;
	struct ov_lvgl_view *view;
	const struct plotter_table *plotters;
	void *plotter_priv;
	ov_plotter_surface_bind_fn bind_surface;
	void *bind_ctx;
	ov_now_ms_fn now_ms;
	void *clock_ctx;
	struct ov_queue_sync queue_sync;
	void (*wake_core)(void *ctx);
	void *wake_core_ctx;
};

struct ov_netsurf_service {
	struct ov_event_queue events;
	struct ov_scheduler scheduler;
	struct ov_script_control scripts;
	struct ov_netsurf_service_config config;
	struct ov_qpk_netsurf_ops qpk_ops;
	struct ov_input_request input_request;
	uint32_t input_id;
	uint32_t *render_pixels;
	int render_stride;
	int render_rows;
	uint64_t frame_serial;
	uint64_t last_title_poll;
	char url[OV_QPK_TEXT_MAX];
	char title[OV_QPK_TEXT_MAX];
	char status[OV_QPK_TEXT_MAX];
	int scroll_x;
	int scroll_y;
	bool loading;
	bool error_page;
	bool images_visible;
	bool images_requested;
	bool images_pending;
	bool reading_requested;
	bool reading_pending;
	bool reading_active;
	bool reading_available;
	int zoom_requested;
	int zoom_percent;
	bool zoom_pending;
	bool can_back;
	bool can_forward;
	bool viewport_pending;
	int viewport_x;
	int viewport_y;
	int viewport_width;
	int viewport_height;
	bool redraw_pending;
	bool running;
	bool core_initialized;
};

/* Start the platform frontend. NetSurf core registration is intentionally
 * separate because the target must provide its fetch, file, and misc tables. */
nserror ov_netsurf_service_init(struct ov_netsurf_service *service,
				const struct ov_netsurf_service_config *config);

/* Register and initialize the caller-supplied NetSurf operation table. */
nserror ov_netsurf_service_core_init(struct ov_netsurf_service *service,
				     struct netsurf_table *table,
				     const char *store_path);

/* Create the single root browser window after core_init. */
nserror ov_netsurf_service_open(struct ov_netsurf_service *service,
				const char *url);

/* Run on the NetSurf owner thread. Returns true while the service is active. */
bool ov_netsurf_service_step(struct ov_netsurf_service *service,
				     unsigned callback_budget,
				     uint32_t time_budget_ms);

/* Apply a browser-thread resize on the LVGL owner thread. Call after consuming
 * any completed frame; returns true if a pending viewport was applied. */
bool ov_netsurf_service_ui_poll(struct ov_netsurf_service *service);

/* Stop rendering, destroy the root browser, and release service resources.
 * Unbind the QuickJS bridge and stop its runtime on the UI thread first. */
void ov_netsurf_service_fini(struct ov_netsurf_service *service);

const struct ov_qpk_netsurf_ops *ov_netsurf_service_qpk_ops(
		const struct ov_netsurf_service *service);

#endif
