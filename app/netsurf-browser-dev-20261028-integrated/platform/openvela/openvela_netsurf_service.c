/* SPDX-License-Identifier: GPL-2.0-only */
#include "openvela_netsurf_service.h"
#include "openvela_resources.h"
#include "openvela_http.h"
#include "openvela_image.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "desktop/browser_history.h"
#include "netsurf/fetch.h"
#include "netsurf/netsurf.h"
#include "utils/nsurl.h"
#include "netsurf/content.h"
#include <dom/dom.h>
#include "content/handlers/html/html_save.h"

static void service_refresh_document_title(struct ov_netsurf_service *service)
{
	uint64_t now = service->config.now_ms(service->config.clock_ctx);
	if (now - service->last_title_poll < 100) return;
	service->last_title_poll = now;
	struct browser_window *browser = ov_window_browser();
	struct hlcache_handle *content = browser ? browser_window_get_content(browser) : NULL;
	if (!content || content_get_type(content) != CONTENT_HTML) return;
	dom_document *document = html_get_document(content);
	dom_string *title = NULL;
	if (!document || dom_html_document_get_title((dom_html_document *)document, &title) != DOM_NO_ERR || !title)
		return;
	if (service->config.queue_sync.lock) service->config.queue_sync.lock(service->config.queue_sync.ctx);
	size_t size = dom_string_byte_length(title);
	if (size >= sizeof(service->title)) size = sizeof(service->title) - 1;
	memcpy(service->title, dom_string_data(title), size);
	service->title[size] = 0;
	if (service->config.queue_sync.unlock) service->config.queue_sync.unlock(service->config.queue_sync.ctx);
	dom_string_unref(title);
}

static void service_lock(struct ov_netsurf_service *service)
{
	if (service->config.queue_sync.lock != NULL)
		service->config.queue_sync.lock(service->config.queue_sync.ctx);
}

static void service_unlock(struct ov_netsurf_service *service)
{
	if (service->config.queue_sync.unlock != NULL)
		service->config.queue_sync.unlock(service->config.queue_sync.ctx);
}

static void service_redraw_requested(void *ctx)
{
	struct ov_netsurf_service *service = ctx;
	service_lock(service);
	service->redraw_pending = true;
	service_unlock(service);
}

static void copy_text(char *dst, size_t capacity, const char *src)
{
	if (capacity == 0)
		return;
	if (src == NULL)
		src = "";
	strncpy(dst, src, capacity - 1u);
	dst[capacity - 1u] = '\0';
}

static void service_title_changed(void *ctx, const char *title)
{
	struct ov_netsurf_service *service = ctx;
	service_lock(service);
	copy_text(service->title, sizeof(service->title), title);
	service_unlock(service);
}

static void service_url_changed(void *ctx, const char *url)
{
	struct ov_netsurf_service *service = ctx;
	service_lock(service);
	copy_text(service->url, sizeof(service->url), url);
	service_unlock(service);
}

static void service_status_changed(void *ctx, const char *status)
{
	struct ov_netsurf_service *service = ctx;
	service_lock(service);
	copy_text(service->status, sizeof(service->status), status);
	service_unlock(service);
}

static void service_loading_changed(void *ctx, bool loading)
{
	struct ov_netsurf_service *service = ctx;
	service_lock(service);
	service->loading = loading;
	service_unlock(service);
}

static void service_scroll_changed(void *ctx, int x, int y)
{
	struct ov_netsurf_service *service = ctx;
	service_lock(service);
	service->scroll_x = x;
	service->scroll_y = y;
	service->redraw_pending = true;
	service_unlock(service);
}

static void service_wake_core(void *ctx);

static void service_input_changed(void *ctx, const struct ov_input_request *request)
{
	struct ov_netsurf_service *service = ctx;
	service_lock(service);
	memset(&service->input_request, 0, sizeof(service->input_request));
	service->input_id = request ? request->id : 0;
	if (request) service->input_request = *request;
	service_unlock(service);
}

static bool qpk_take_input(void *ctx, struct ov_input_request *request)
{
	struct ov_netsurf_service *service = ctx;
	if (!request) return false;
	service_lock(service);
	bool pending = service->running && service->input_request.id != 0;
	*request = service->input_request;
	memset(&service->input_request, 0, sizeof(service->input_request));
	service_unlock(service);
	return pending;
}

static bool qpk_submit_input(void *ctx, uint32_t id, const char *text)
{
	struct ov_netsurf_service *service = ctx;
	if (!id || (text && strlen(text) >= OV_INPUT_TEXT_MAX)) return false;
	service_lock(service);
	bool active = service->running && service->input_id == id;
	service_unlock(service);
	if (!active) return false;
	struct ov_event event = {.type = OV_EVENT_INPUT,
		.payload.input = {.id = id, .cancel = text == NULL}};
	if (text) memcpy(event.payload.input.text, text, strlen(text) + 1);
	return ov_frontend_publish(&service->events, &event,
		&(struct ov_frontend_ops){.wake_core = service_wake_core, .ctx = service});
}

static void service_wake_core(void *ctx)
{
	struct ov_netsurf_service *service = ctx;
	if (service->config.wake_core != NULL)
		service->config.wake_core(service->config.wake_core_ctx);
}

static bool qpk_navigate(void *ctx, const char *url)
{
	struct ov_netsurf_service *service = ctx;
	ov_script_cancel(&service->scripts);
	return ov_frontend_publish_navigation(&service->events, url, 0,
					      &(struct ov_frontend_ops){
							.wake_core = service_wake_core,
							.ctx = service});
}

static bool qpk_navigation_event(void *ctx, enum ov_event_type type)
{
	struct ov_netsurf_service *service = ctx;
	ov_script_cancel(&service->scripts);
	struct ov_event event = {.type = type};
	return ov_frontend_publish(&service->events, &event,
					&(struct ov_frontend_ops){
						.wake_core = service_wake_core,
						.ctx = service});
}

static bool qpk_back(void *ctx) { return qpk_navigation_event(ctx, OV_EVENT_BACK); }
static bool qpk_forward(void *ctx) { return qpk_navigation_event(ctx, OV_EVENT_FORWARD); }
static bool qpk_reload(void *ctx) { return qpk_navigation_event(ctx, OV_EVENT_RELOAD); }
static bool qpk_stop(void *ctx) { return qpk_navigation_event(ctx, OV_EVENT_STOP); }

static bool qpk_set_viewport(void *ctx, int x, int y, int width, int height)
{
	struct ov_netsurf_service *service = ctx;
	if (service->config.view == NULL || x < 0 || y < 0 || width <= 0 ||
	    height <= 0 || width > UINT16_MAX || height > UINT16_MAX)
		return false;
	struct ov_event event = {
		.type = OV_EVENT_RESIZE,
		.payload.resize = {.x = x, .y = y, .width = width, .height = height},
	};
	return ov_frontend_publish(&service->events, &event,
					&(struct ov_frontend_ops){
						.wake_core = service_wake_core,
						.ctx = service});
}

static bool qpk_pointer(void *ctx, enum ov_qpk_pointer_state state, int x, int y)
{
	struct ov_netsurf_service *service = ctx;
	static const enum ov_event_type event_types[] = {
		[OV_QPK_POINTER_DOWN] = OV_EVENT_TOUCH_DOWN,
		[OV_QPK_POINTER_MOVE] = OV_EVENT_TOUCH_MOVE,
		[OV_QPK_POINTER_UP] = OV_EVENT_TOUCH_UP,
		[OV_QPK_POINTER_CANCEL] = OV_EVENT_TOUCH_CANCEL,
	};
	if ((unsigned)state >= sizeof(event_types) / sizeof(event_types[0]))
		return false;
	return ov_frontend_publish_touch(&service->events, event_types[state], x, y,
					  0, &(struct ov_frontend_ops){
						  .wake_core = service_wake_core,
						  .ctx = service});
}

static bool qpk_scroll_by(void *ctx, int delta_x, int delta_y)
{
	struct ov_netsurf_service *service = ctx;
	struct ov_event event = {
		.type = OV_EVENT_SCROLL,
		.payload.scroll = {.delta_x = delta_x, .delta_y = delta_y},
	};
	return ov_frontend_publish(&service->events, &event,
					&(struct ov_frontend_ops){
						.wake_core = service_wake_core,
						.ctx = service});
}

static bool qpk_status(void *ctx, struct ov_qpk_netsurf_status *status)
{
	struct ov_netsurf_service *service = ctx;
	if (status == NULL)
		return false;
	service_lock(service);
	memset(status, 0, sizeof(*status));
	status->available = service->running && service->core_initialized &&
				service->config.view != NULL;
	status->loading = service->loading;
	status->error_page = service->error_page;
	status->images_visible = service->images_visible;
	status->reading_active = service->reading_active;
	status->reading_available = service->reading_available;
	status->zoom_percent = service->zoom_percent;
	status->can_back = service->can_back;
	status->can_forward = service->can_forward;
	status->scroll_x = service->scroll_x;
	status->scroll_y = service->scroll_y;
	copy_text(status->url, sizeof(status->url), service->url);
	copy_text(status->title, sizeof(status->title), service->title);
	copy_text(status->message, sizeof(status->message), service->status);
	service_unlock(service);
	return status->available;
}

static bool qpk_set_images_visible(void *ctx, bool visible)
{
	struct ov_netsurf_service *service = ctx;
	service_lock(service);
	bool accepted = service->running;
	if (accepted) {
		service->images_requested = visible;
		service->images_pending = true;
	}
	service_unlock(service);
	if (accepted) service_wake_core(service);
	return accepted;
}

static void service_apply_images(struct ov_netsurf_service *service)
{
	service_lock(service);
	bool pending = service->images_pending;
	bool visible = service->images_requested;
	service->images_pending = false;
	service_unlock(service);
	if (!pending) return;
	ov_window_set_images_visible(visible);
	service_lock(service);
	service->images_visible = visible;
	service_unlock(service);
}

static bool qpk_set_zoom(void *ctx, int percent)
{
	struct ov_netsurf_service *service = ctx;
	if(percent != 0 && (percent < 25 || percent > 300)) return false;
	service_lock(service);
	bool accepted = service->running;
	if(accepted) {service->zoom_requested=percent; service->zoom_pending=true;}
	service_unlock(service);
	if(accepted) service_wake_core(service);
	return accepted;
}

static bool qpk_set_reading_enabled(void *ctx, bool enabled)
{
	struct ov_netsurf_service *service = ctx;
	service_lock(service);
	bool accepted=service->running;
	if(accepted) {service->reading_requested=enabled; service->reading_pending=true;}
	service_unlock(service);
	if(accepted) service_wake_core(service);
	return accepted;
}

static void service_apply_view_settings(struct ov_netsurf_service *service)
{
	service_lock(service);
	bool reading=service->reading_requested, rp=service->reading_pending;
	int zoom=service->zoom_requested; bool zp=service->zoom_pending;
	service->reading_pending=false; service->zoom_pending=false;
	service_unlock(service);
	if(rp) ov_window_set_reading_enabled(reading);
	if(zp) ov_window_set_zoom(zoom);
}

static void service_refresh_history(struct ov_netsurf_service *service)
{
	struct browser_window *browser = ov_window_browser();
	bool can_back = browser != NULL && browser_window_history_back_available(browser);
	bool can_forward = browser != NULL && browser_window_history_forward_available(browser);
	bool error_page = ov_window_is_error_page();
	service_lock(service);
	service->can_back = can_back;
	service->can_forward = can_forward;
	service->error_page = error_page;
	service_unlock(service);
}

static int service_render(struct ov_netsurf_service *service)
{
	service_lock(service);
	if (service->viewport_pending) {
		service_unlock(service);
		return 0;
	}
	bool pending = service->redraw_pending;
	service->redraw_pending = false;
	service_unlock(service);
	if (!pending || service->config.view == NULL || ov_window_browser() == NULL)
		return 0;
	if (!ov_window_render(service->config.view, service->render_pixels,
			      service->render_stride, service->render_rows,
			      service->config.bind_surface, service->config.bind_ctx,
			      service->config.plotters, service->config.plotter_priv,
			      ++service->frame_serial)) {
		service_lock(service);
		service->redraw_pending = true;
		service_unlock(service);
		return -1;
	}
	return 1;
}

static uint32_t *service_resize_scratch(struct ov_netsurf_service *service,
					int width, int height, int *rows_out)
{
	int rows = service->config.render_rows;
	if (rows > height)
		rows = height;
	if (width <= 0 || rows <= 0 ||
	    (size_t)width > SIZE_MAX / sizeof(uint32_t) / (size_t)rows)
		return NULL;
	uint32_t *pixels = calloc((size_t)width * (size_t)rows, sizeof(uint32_t));
	if (pixels != NULL)
		*rows_out = rows;
	return pixels;
}

nserror ov_netsurf_service_init(struct ov_netsurf_service *service,
				const struct ov_netsurf_service_config *config)
{
	if (service == NULL || config == NULL || config->width <= 0 ||
	    config->height <= 0 || config->width > UINT16_MAX ||
	    config->height > UINT16_MAX || config->render_rows <= 0 ||
	    config->view == NULL || config->plotters == NULL ||
	    config->plotter_priv == NULL || config->bind_surface == NULL ||
	    config->now_ms == NULL ||
	    ((config->queue_sync.lock == NULL) !=
	     (config->queue_sync.unlock == NULL)) ||
	    config->queue_sync.lock == NULL ||
	    ov_lvgl_view_width(config->view) != config->width ||
	    ov_lvgl_view_height(config->view) != config->height)
		return NSERROR_BAD_PARAMETER;
	memset(service, 0, sizeof(*service));
	service->config = *config;
	service->render_stride = config->width;
	service->render_rows = config->render_rows < config->height ?
		config->render_rows : config->height;
	if ((size_t)service->render_stride > SIZE_MAX / sizeof(uint32_t) /
			(size_t)service->render_rows)
		return NSERROR_BAD_SIZE;
	service->render_pixels = calloc((size_t)service->render_stride *
					(size_t)service->render_rows, sizeof(uint32_t));
	if (service->render_pixels == NULL)
		return NSERROR_NOMEM;
	struct ov_queue_sync sync = config->queue_sync;
	ov_event_queue_init(&service->events, &sync);
	if (ov_scheduler_init(&service->scheduler, config->now_ms,
				      config->clock_ctx) != NSERROR_OK ||
	    ov_scheduler_bind(&service->scheduler) != NSERROR_OK ||
	    !ov_script_bind(&service->scripts, config->now_ms, config->clock_ctx) ||
	    ov_window_init(config->width, config->height,
				&(struct ov_window_ops){
					.redraw_requested = service_redraw_requested,
					.title_changed = service_title_changed,
					.url_changed = service_url_changed,
					.status_changed = service_status_changed,
					.loading_changed = service_loading_changed,
					.scroll_changed = service_scroll_changed,
					.input_changed = service_input_changed,
					.ctx = service}) != NSERROR_OK) {
		ov_scheduler_unbind(&service->scheduler);
		ov_script_unbind(&service->scripts);
		free(service->render_pixels);
		service->render_pixels = NULL;
		return NSERROR_INIT_FAILED;
	}
	service->qpk_ops = (struct ov_qpk_netsurf_ops){
		.navigate = qpk_navigate,
		.back = qpk_back,
		.forward = qpk_forward,
		.reload = qpk_reload,
		.stop = qpk_stop,
		.set_viewport = qpk_set_viewport,
		.pointer = qpk_pointer,
		.scroll_by = qpk_scroll_by,
		.status = qpk_status,
		.take_input = qpk_take_input,
		.submit_input = qpk_submit_input,
		.set_images_visible = qpk_set_images_visible,
		.set_zoom = qpk_set_zoom,
		.set_reading_enabled = qpk_set_reading_enabled,
		.ctx = service,
	};
	service->images_visible = true;
	service->reading_requested = true;
	service->zoom_percent = 100;
	service->images_requested = true;
	service->running = true;
	service->redraw_pending = true;
	return NSERROR_OK;
}

nserror ov_netsurf_service_core_init(struct ov_netsurf_service *service,
				     struct netsurf_table *table,
				     const char *store_path)
{
	if (service == NULL || !service->running || table == NULL)
		return NSERROR_BAD_PARAMETER;
	if (table->fetch == NULL || table->fetch->filetype == NULL ||
	    table->bitmap == NULL || table->layout == NULL)
		return NSERROR_BAD_PARAMETER;
	table->misc = ov_scheduler_misc_table();
	table->window = ov_window_table();
	nserror error = netsurf_register(table);
	if (error != NSERROR_OK)
		return error;
	error = ov_core_options_init();
	if (error != NSERROR_OK)
		return error;
	error = netsurf_init(store_path);
	if (error == NSERROR_OK) {
		service_lock(service);
		service->core_initialized = true;
		service_unlock(service);
		error = ov_image_register();
	}
	return error;
}

nserror ov_netsurf_service_open(struct ov_netsurf_service *service,
				const char *url)
{
	if (service == NULL || !service->running || !service->core_initialized ||
	    ov_window_browser() != NULL)
		return NSERROR_BAD_PARAMETER;
	struct nsurl *target = NULL;
	if (url != NULL && url[0] != '\0') {
		nserror error = nsurl_create(url, &target);
		if (error != NSERROR_OK)
			return error;
	}
	nserror error = browser_window_create(BW_CREATE_HISTORY,
						      target, NULL, NULL, NULL);
	if (target != NULL)
		nsurl_unref(target);
	if (error == NSERROR_OK) {
		service_refresh_history(service);
		service_lock(service);
		service->redraw_pending = true;
		service_unlock(service);
	}
	return error;
}

bool ov_netsurf_service_step(struct ov_netsurf_service *service,
				     unsigned callback_budget,
				     uint32_t time_budget_ms)
{
	if (service == NULL || !service->running)
		return false;
	/* A presentation setting must still apply when input has filled the queue.
	 * Only this owner thread touches NetSurf or requests a page redraw. */
	service_apply_images(service);
	service_apply_view_settings(service);
	struct ov_event event;
	unsigned processed = 0;
	while ((callback_budget == 0 || processed < callback_budget) &&
	       ov_event_pop(&service->events, &event)) {
		if (event.type == OV_EVENT_TICK) {
			ov_scheduler_run_due(&service->scheduler, callback_budget,
					     time_budget_ms);
		} else {
			uint32_t *new_pixels = NULL;
			int new_rows = 0;
			if (event.type == OV_EVENT_RESIZE) {
				new_pixels = service_resize_scratch(service,
						event.payload.resize.width,
						event.payload.resize.height, &new_rows);
				if (new_pixels == NULL) {
					service_status_changed(service, "页面尺寸超出可用内存");
				} else {
					nserror error = ov_window_dispatch(&event);
					if (error != NSERROR_OK) {
						service_status_changed(service, "浏览器操作失败");
					} else {
						free(service->render_pixels);
						service->render_pixels = new_pixels;
						service->render_stride = event.payload.resize.width;
						service->render_rows = new_rows;
						service->config.width = event.payload.resize.width;
						service->config.height = event.payload.resize.height;
						service_lock(service);
						service->viewport_x = event.payload.resize.x;
						service->viewport_y = event.payload.resize.y;
						service->viewport_width = event.payload.resize.width;
						service->viewport_height = event.payload.resize.height;
						service->viewport_pending = true;
						service_unlock(service);
						new_pixels = NULL;
					}
				}
				free(new_pixels);
			} else {
				nserror error = ov_window_dispatch(&event);
				if (error != NSERROR_OK)
					service_status_changed(service, "浏览器操作失败");
			}
		}
		processed++;
	}
	ov_scheduler_run_due(&service->scheduler, callback_budget,
				    time_budget_ms);
	service_refresh_history(service);
	service_refresh_document_title(service);
	ov_window_update_layout();
	bool reading_active, reading_available;
	ov_window_reading_state(&reading_active, &reading_available);
	int zoom_percent=ov_window_zoom();
	service_lock(service);
	service->reading_active=reading_active;
	service->reading_available=reading_available;
	service->zoom_percent=zoom_percent;
	service_unlock(service);
	service_render(service);
	return true;
}

bool ov_netsurf_service_ui_poll(struct ov_netsurf_service *service)
{
	if (service == NULL || service->config.view == NULL)
		return false;
	int x, y, width, height;
	service_lock(service);
	if (!service->viewport_pending) {
		service_unlock(service);
		return false;
	}
	x = service->viewport_x;
	y = service->viewport_y;
	width = service->viewport_width;
	height = service->viewport_height;
	service_unlock(service);
	if (!ov_lvgl_view_resize(service->config.view, width, height))
		return false;
	lv_obj_set_pos(ov_lvgl_view_object(service->config.view), x, y);
	service_lock(service);
	if (service->viewport_x == x && service->viewport_y == y &&
	    service->viewport_width == width && service->viewport_height == height)
		service->viewport_pending = false;
	service_unlock(service);
	return true;
}

void ov_netsurf_service_fini(struct ov_netsurf_service *service)
{
	if (service == NULL)
		return;
	service_lock(service);
	service->running = false;
	service_unlock(service);
	if (ov_window_browser() != NULL)
		browser_window_destroy(ov_window_browser());
	if (service->core_initialized) {
		ov_http_stop();
		netsurf_exit();
		ov_core_options_fini();
		service_lock(service);
		service->core_initialized = false;
		service_unlock(service);
	}
	ov_window_fini();
	ov_scheduler_unbind(&service->scheduler);
	ov_script_unbind(&service->scripts);
	free(service->render_pixels);
	memset(service, 0, sizeof(*service));
}

const struct ov_qpk_netsurf_ops *ov_netsurf_service_qpk_ops(
		const struct ov_netsurf_service *service)
{
	return service != NULL && service->running ? &service->qpk_ops : NULL;
}
