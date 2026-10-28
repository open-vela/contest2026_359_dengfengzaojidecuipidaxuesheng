/* SPDX-License-Identifier: GPL-2.0-only */
#include "openvela_desktop.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "openvela_bitmap.h"
#include "openvela_font.h"
#include "openvela_http_nuttx.h"
#include "openvela_netsurf_service.h"
#include "openvela_plot.h"
#include "glass_https.h"
#include "openvela_resources.h"
#include "utils/log.h"

#ifndef OV_NETSURF_FONT_PATH
#define OV_NETSURF_FONT_PATH "/sdcard/fonts/VelaGlobal-Regular.ttf"
#endif
#ifndef OV_NETSURF_CA_PATH
#define OV_NETSURF_CA_PATH "/sdcard/netsurf/ca-bundle.pem"
#endif

struct desktop {
	struct ov_netsurf_service service;
	struct ov_plot_target target;
	struct netsurf_table tables;
	struct ov_http_nuttx_config trust;
	struct ov_http_transport transport;
	pthread_mutex_t lock, frame_lock;
	pthread_t thread;
	struct ov_lvgl_view *view;
	lv_timer_t *timer;
	bool attached, failed;
	unsigned generation;
};
static struct desktop *desktop;
static void lock(void *ctx) { pthread_mutex_lock(ctx); }
static void unlock(void *ctx) { pthread_mutex_unlock(ctx); }
static uint64_t now_ms(void *ctx)
{
	(void)ctx;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static bool bind_surface(void *ctx, void *target, uint32_t *pixels,
			 int width, int height, int stride)
{
	(void)ctx;
	ov_plot_target_init(target, pixels, width, height, stride);
	return true;
}
static void failure(struct desktop *d, const char *text)
{
	pthread_mutex_lock(&d->lock);
	d->failed = true;
	snprintf(d->service.status, sizeof(d->service.status), "%s", text);
	pthread_mutex_unlock(&d->lock);
	fprintf(stderr, "[netsurf] %s\n", text);
}
static void load_trust(struct desktop *d)
{
	FILE *file = fopen(OV_NETSURF_CA_PATH, "rb");
	if (!file) return;
	if (fseek(file, 0, SEEK_END)) { fclose(file); return; }
	long size = ftell(file);
	if (size <= 0 || size > 256 * 1024 || fseek(file, 0, SEEK_SET)) {
		fclose(file); return;
	}
	unsigned char *pem = malloc((size_t)size + 2);
	if (!pem) { fclose(file); return; }
	bool valid = fread(pem, 1, size, file) == (size_t)size;
	fclose(file);
	if (!valid) { free(pem); return; }
	pem[size] = 0;
	unsigned char *filtered = malloc((size_t)size + 2);
	if (!filtered) { free(pem); return; }
	/* mbedTLS returns a positive failure count for a mixed bundle. The
	 * shared TLS service intentionally requires a completely parseable
	 * bundle. Keep only individually parseable anchors from this trusted
	 * file; never introduce another root or weaken peer/hostname checks. */
	const char *begin_marker = "-----BEGIN CERTIFICATE-----";
	const char *end_marker = "-----END CERTIFICATE-----";
	char *cursor = (char *)pem;
	size_t used = 0;
	unsigned accepted = 0, rejected = 0;
	for (;;) {
		char *begin = strstr(cursor, begin_marker);
		if (!begin) break;
		char *end = strstr(begin, end_marker);
		if (!end) { free(filtered); free(pem); return; }
		end += strlen(end_marker);
		char saved = *end;
		if (saved && saved != '\n' && saved != '\r' && saved != ' ' && saved != '\t') {
			free(filtered); free(pem); return;
		}
		*end = 0;
		size_t bytes = (size_t)(end - begin);
		bool supported = glass_https_validate_ca((unsigned char *)begin, bytes + 1) == 0;
		*end = saved;
		if (supported) {
			memcpy(filtered + used, begin, bytes);
			used += bytes;
			filtered[used++] = '\n';
			accepted++;
		} else rejected++;
		cursor = end;
		if (!saved) break;
	}
	free(pem);
	if (!accepted) { free(filtered); return; }
	filtered[used] = 0;
	fprintf(stderr, "[browser-trust] accepted=%u unsupported=%u\n", accepted, rejected);
	d->trust.ca_pem = filtered;
	d->trust.ca_size = used + 1;
}
static void *run(void *ctx)
{
	struct desktop *d = ctx;
	/* The prepared core compiles WARNING and higher. Initialise its sink so
	 * script exceptions are visible instead of silently leaving a blank page. */
	int log_argc=2;
	char *log_argv[]={"netsurf", "-v", NULL};
	nslog_init(NULL,&log_argc,log_argv);
	if (!ov_font_init(OV_NETSURF_FONT_PATH)) {
		failure(d, "无法读取浏览器字体文件"); return NULL;
	}
	load_trust(d);
	d->tables = (struct netsurf_table){
		.fetch = ov_resource_fetch_table(), .bitmap = &ov_bitmap_table,
		.layout = ov_font_layout_table(), .canvas = ov_font_canvas_table(),
	};
	nserror error = ov_netsurf_service_core_init(&d->service, &d->tables, NULL);
	if (error) { failure(d, "浏览器核心初始化失败"); return NULL; }
	d->transport = (struct ov_http_transport){.perform = ov_http_nuttx_perform, .ctx = &d->trust};
	if (ov_http_register(&d->transport)) {
		failure(d, "浏览器网络服务初始化失败"); return NULL;
	}
	unsigned generation = 0;
	for (;;) {
		pthread_mutex_lock(&d->lock);
		bool attached = d->attached;
		unsigned requested = d->generation;
		pthread_mutex_unlock(&d->lock);
		if ((!attached || generation != requested) && ov_window_browser()) {
			browser_window_destroy(ov_window_browser());
			ov_http_stop();
			ov_plot_bitmap_cache_clear();
		}
		if (attached) {
			if (!ov_window_browser()) {
				error = ov_netsurf_service_open(&d->service, "about:blank");
				if (error) { failure(d, "无法创建浏览页面"); return NULL; }
				generation = requested;
			}
			ov_netsurf_service_step(&d->service, 32, 8);
		} else {
			/* Allow cancelled fetches and cache housekeeping to retire while
			 * the UI is closed. All callbacks still belong to this thread. */
			ov_scheduler_run_due(&d->service.scheduler, 32, 8);
		}
		usleep(10000);
	}
	return NULL;
}
static void poll_ui(lv_timer_t *timer)
{
	struct desktop *d = lv_timer_get_user_data(timer);
	ov_lvgl_view_poll(d->view);
	ov_netsurf_service_ui_poll(&d->service);
}
static struct desktop *create(lv_obj_t *root)
{
	struct desktop *d = calloc(1, sizeof(*d));
	if (!d) return NULL;
	if (pthread_mutex_init(&d->lock, NULL)) { free(d); return NULL; }
	if (pthread_mutex_init(&d->frame_lock, NULL)) {
		pthread_mutex_destroy(&d->lock); free(d); return NULL;
	}
	int width = lv_obj_get_width(root), height = lv_obj_get_height(root);
	if (width <= 0 || height <= 0) goto fail;
	d->view = ov_lvgl_view_create(root, width, height, NULL,
		&(struct ov_lvgl_view_ops){.frame_sync = {lock, unlock, &d->frame_lock}});
	if (!d->view) goto fail;
	struct ov_netsurf_service_config config = {
		.width = width, .height = height, .render_rows = 32,
		.view = d->view, .plotters = &ov_plotters, .plotter_priv = &d->target,
		.bind_surface = bind_surface, .now_ms = now_ms,
		.queue_sync = {lock, unlock, &d->lock},
	};
	if (ov_netsurf_service_init(&d->service, &config)) goto fail;
	pthread_attr_t attr;
	int result = pthread_attr_init(&attr);
	if (!result) {
		result = pthread_attr_setstacksize(&attr, 128u * 1024u);
#ifdef __NuttX__
		/* Keep the LVGL owner runnable while webpage bytecode is busy. */
		int policy;
		struct sched_param priority;
		if (!result) result = pthread_getschedparam(pthread_self(), &policy, &priority);
		if (!result && priority.sched_priority > sched_get_priority_min(policy)) {
			priority.sched_priority--;
			result = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
			if (!result) result = pthread_attr_setschedpolicy(&attr, policy);
			if (!result) result = pthread_attr_setschedparam(&attr, &priority);
		}
#endif
		if (!result) result = pthread_create(&d->thread, &attr, run, d);
		pthread_attr_destroy(&attr);
	}
	if (!result) { pthread_detach(d->thread); return d; }
	ov_netsurf_service_fini(&d->service);
fail:
	ov_lvgl_view_destroy(d->view);
	pthread_mutex_destroy(&d->frame_lock);
	pthread_mutex_destroy(&d->lock);
	free(d);
	return NULL;
}
bool ov_desktop_netsurf_install(JSContext *context, JSValue system,
			      lv_obj_t *root, const char *package)
{
	if (!package || strcmp(package, "com.netsurf.browser")) return true;
	lv_obj_update_layout(root);
	if (!desktop) desktop = create(root);
	struct desktop *d = desktop;
	if (!d) return false;
	pthread_mutex_lock(&d->lock);
	bool failed = d->failed;
	pthread_mutex_unlock(&d->lock);
	if (failed || d->timer) return false;
	lv_obj_t *image = ov_lvgl_view_object(d->view);
	lv_obj_set_parent(image, root);
	lv_obj_move_to_index(image, 0);
	lv_obj_remove_flag(image, LV_OBJ_FLAG_HIDDEN);
	d->timer = lv_timer_create(poll_ui, 20, d);
	if (!d->timer || !ov_qpk_netsurf_bind(context, system,
					ov_netsurf_service_qpk_ops(&d->service))) {
		ov_desktop_netsurf_stop(); return false;
	}
	pthread_mutex_lock(&d->lock);
	d->generation++;
	d->attached = true;
	pthread_mutex_unlock(&d->lock);
	return true;
}
void ov_desktop_netsurf_stop(void)
{
	struct desktop *d = desktop;
	if (!d) return;
	ov_script_cancel(&d->service.scripts);
	ov_qpk_netsurf_unbind(ov_netsurf_service_qpk_ops(&d->service));
	pthread_mutex_lock(&d->lock);
	d->attached = false;
	pthread_mutex_unlock(&d->lock);
	struct ov_event event;
	while (ov_event_pop(&d->service.events, &event)) {}
	if (d->timer) { lv_timer_delete(d->timer); d->timer = NULL; }
	lv_obj_t *image = ov_lvgl_view_object(d->view);
	lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_parent(image, lv_layer_sys());
}
