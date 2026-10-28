/* SPDX-License-Identifier: GPL-2.0-only */
#include "openvela_window.h"
#include "openvela_reading.h"
#include "openvela_font.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "desktop/browser_history.h"
#include "netsurf/browser_window.h"
#include "netsurf/plotters.h"
#include "netsurf/content.h"
#include "desktop/browser_private.h"
#include "content/hlcache.h"
#include "content/handlers/html/private.h"
#include "content/handlers/html/box.h"
#include "content/handlers/html/box_construct.h"
#include "content/handlers/html/box_inspect.h"
#include "content/handlers/html/form_internal.h"
#include "utils/corestrings.h"
#include <dom/dom.h>
#include "netsurf/browser_window.h"
#include "netsurf/mouse.h"
#include "netsurf/plotters.h"
#include "netsurf/window.h"
#include "utils/errors.h"
#include "utils/nsurl.h"

struct gui_window {
	struct browser_window *bw;
	int scroll_x;
	int scroll_y;
	int press_x;
	int press_y;
	int last_x;
	int last_y;
	bool touch_down;
	bool drag_started;
	bool scroll_started;
	bool fit_armed;
	bool extent_pending;
	unsigned fit_passes;
	bool reading_pending;
	bool reading_active;
	bool reading_available;
	dom_node *view_anchor;
	float anchor_fraction;
};

struct ov_window_runtime {
	int width;
	int height;
	struct ov_window_ops ops;
	struct gui_window *window;
	struct ov_damage damage;
	bool initialized;
	bool redraw_notified;
	bool images_visible;
	bool reading_enabled;
	int manual_zoom;
	/* Owner-thread references only. No box/gadget pointer survives a callback. */
	dom_node *input_node;
	dom_string *input_value;
	struct hlcache_handle *input_content;
	uint32_t input_id;
	bool input_password;
};

static struct ov_window_runtime runtime;
/* Survives window/service reopen, so old UI callbacks cannot target a new edit. */
static uint32_t input_sequence;

static void anchor_clear(struct gui_window *gw)
{
	if(gw->view_anchor) dom_node_unref(gw->view_anchor);
	gw->view_anchor=NULL;
}

static void anchor_box(struct box *box, int y, struct box **best)
{
	for(;box;box=box->next) {
		int x,top;box_coords(box,&x,&top);
		if(box->node && box->type==BOX_BLOCK && box->height>0 &&
		   top<=y && (int64_t)top+box->height>y &&
		   (!*best || box->height<(*best)->height)) *best=box;
		anchor_box(box->children,y,best);
	}
}

static void anchor_capture(struct gui_window *gw)
{
	anchor_clear(gw);
	struct hlcache_handle *c=gw->bw->current_content;
	if(!c || content_get_type(c)!=CONTENT_HTML || !browser_window_redraw_ready(gw->bw)) return;
	html_content *html=(html_content *)hlcache_handle_get_content(c);
	if(html->rebuild_pending) return;
	int y=(int)(gw->scroll_y/browser_window_get_scale(gw->bw));
	struct box *box=NULL;anchor_box(html->layout,y,&box);
	if(box) {
		int x,top;box_coords(box,&x,&top);
		gw->view_anchor=dom_node_ref(box->node);
		gw->anchor_fraction=(float)(y-top)/box->height;
	}
}

static void input_clear(void)
{
	if (runtime.input_node != NULL) dom_node_unref(runtime.input_node);
	if (runtime.input_value != NULL) dom_string_unref(runtime.input_value);
	runtime.input_node = NULL;
	runtime.input_value = NULL;
	runtime.input_content = NULL;
	runtime.input_id = 0;
	if (runtime.ops.input_changed != NULL)
		runtime.ops.input_changed(runtime.ops.ctx, NULL);
}

static struct form_control *input_control(struct hlcache_handle **content)
{
	struct browser_window *bw = runtime.window ? runtime.window->bw : NULL;
	if (bw != NULL && bw->focus != NULL) bw = bw->focus;
	*content = bw ? browser_window_get_content(bw) : NULL;
	if (*content == NULL || content_get_type(*content) != CONTENT_HTML)
		return NULL;
	html_content *html = (html_content *)hlcache_handle_get_content(*content);
	if (html->focus_type != HTML_FOCUS_TEXTAREA || !html->focus_owner.textarea)
		return NULL;
	struct form_control *control = html->focus_owner.textarea->gadget;
	/* The current Vela prompt is single-line. Leave textarea editing for a
	 * multiline input surface rather than silently destroying line breaks. */
	if (control == NULL || control->node == NULL ||
	    (control->type != GADGET_TEXTBOX && control->type != GADGET_PASSWORD))
		return NULL;
	bool readonly = true, disabled = true;
	dom_html_input_element *node = control->node;
	if (dom_html_input_element_get_read_only(node, &readonly) != DOM_NO_ERR ||
	    dom_html_input_element_get_disabled(node, &disabled) != DOM_NO_ERR ||
	    readonly || disabled)
		return NULL;
	return control;
}

/* Validate scalar UTF-8 and count characters without truncating multibyte
 * text. Reject line breaks: this bridge edits single-line controls only. */
static int input_characters(const char *text)
{
	const unsigned char *p = (const unsigned char *)text;
	int count = 0;
	while (*p) {
		uint32_t c = *p++; unsigned extra;
		if (c < 0x80) { if (c < 0x20 || c == 0x7f) return -1; extra = 0; }
		else if (c >= 0xc2 && c <= 0xdf) { c &= 0x1f; extra = 1; }
		else if (c >= 0xe0 && c <= 0xef) { c &= 0x0f; extra = 2; }
		else if (c >= 0xf0 && c <= 0xf4) { c &= 7; extra = 3; }
		else return -1;
		unsigned n = extra;
		while (n--) { if ((*p & 0xc0) != 0x80) return -1; c = (c << 6) | (*p++ & 0x3f); }
		if ((extra == 1 && c < 0x80) || (extra == 2 && c < 0x800) ||
		    (extra == 3 && c < 0x10000) || c > 0x10ffff ||
		    (c >= 0xd800 && c <= 0xdfff)) return -1;
		count++;
	}
	return count;
}

static int input_limit(struct form_control *control)
{
	int32_t maxlength = -1;
	if (dom_html_input_element_get_max_length(control->node, &maxlength) != DOM_NO_ERR)
		return 0;
	return maxlength >= 0 && maxlength < OV_INPUT_CHAR_MAX ? maxlength : OV_INPUT_CHAR_MAX;
}

static bool input_value_read(dom_node *node, dom_string **value)
{
	if (dom_html_input_element_get_value((dom_html_input_element *)node, value) != DOM_NO_ERR)
		return false;
	/* libdom represents an unset/empty initial value with NULL. */
	return *value != NULL || dom_string_create((const uint8_t *)"", 0, value) == DOM_NO_ERR;
}

static bool input_password(struct form_control *control)
{
	dom_string *type = NULL;
	bool password = control->type == GADGET_PASSWORD;
	if (dom_html_input_element_get_type(control->node, &type) == DOM_NO_ERR && type) {
		password |= dom_string_byte_length(type) == 8 &&
			!strncasecmp(dom_string_data(type), "password", 8);
		dom_string_unref(type);
	}
	return password;
}

static void input_request(int x, int y)
{
	input_clear();
	struct browser_window_features features = {0};
	if (browser_window_get_features(runtime.window->bw, x, y, &features) != NSERROR_OK ||
	    features.form_features != CTX_FORM_TEXT) return;
	struct hlcache_handle *content;
	struct form_control *control = input_control(&content);
	if (control == NULL || input_sequence == INT32_MAX) return;
	dom_string *value = NULL;
	if (!input_value_read(control->node, &value))
		return;
	struct ov_input_request request = {.max_length = input_limit(control),
		.password = input_password(control)};
	size_t size = dom_string_byte_length(value);
	if (!request.password) {
		if (size >= sizeof(request.value)) goto too_long;
		memcpy(request.value, dom_string_data(value), size);
		int count = input_characters(request.value);
		if (count < 0 || count > request.max_length) goto too_long;
	}
	if (request.max_length == 0) { dom_string_unref(value); return; }
	request.id = ++input_sequence;
	runtime.input_id = request.id;
	runtime.input_password = request.password;
	runtime.input_node = dom_node_ref(control->node);
	runtime.input_value = value;
	runtime.input_content = content;
	if (runtime.ops.input_changed)
		runtime.ops.input_changed(runtime.ops.ctx, &request);
	return;
too_long:
	dom_string_unref(value);
	if (runtime.ops.status_changed)
		runtime.ops.status_changed(runtime.ops.ctx, "输入内容超出当前键盘长度限制");
}

static nserror input_submit(const struct ov_event *event)
{
	if (!runtime.input_id || runtime.input_id != event->payload.input.id)
		return NSERROR_BAD_PARAMETER;
	if (event->payload.input.cancel) { input_clear(); return NSERROR_OK; }
	struct hlcache_handle *content;
	struct form_control *control = input_control(&content);
	dom_string *current = NULL;
	bool valid = control && content == runtime.input_content &&
		control->node == runtime.input_node &&
		input_password(control) == runtime.input_password &&
		input_value_read(control->node, &current) &&
		current && dom_string_isequal(current, runtime.input_value);
	if (current) dom_string_unref(current);
	const char *text = event->payload.input.text;
	int count = memchr(text, 0, sizeof(event->payload.input.text)) ? input_characters(text) : -1;
	if (!valid || count < 0 || count > input_limit(control)) {
		input_clear(); return NSERROR_BAD_PARAMETER;
	}
	bool changed = dom_string_byte_length(runtime.input_value) != strlen(text) ||
		memcmp(dom_string_data(runtime.input_value), text, strlen(text)) != 0;
	char *copy = strdup(text);
	if (!copy) { input_clear(); return NSERROR_NOMEM; }
	dom_node *node = dom_node_ref(runtime.input_node);
	input_clear();
	form_gadget_update_value(control, copy); /* core owns copy, syncs DOM/widget */
	/* Event handlers may replace the entire box tree. Hold only the DOM node
	 * across dispatch, never dereference control after executing webpage JS. */
	if (changed) {
		fire_generic_dom_event(corestring_dom_input, node, true, false);
		fire_generic_dom_event(corestring_dom_change, node, true, false);
	}
	dom_node_unref(node);
	return NSERROR_OK;
}

static int subtract_scroll(int value, int scroll)
{
	int64_t result = (int64_t)value - scroll;
	if (result < INT_MIN)
		return INT_MIN;
	if (result > INT_MAX)
		return INT_MAX;
	return (int)result;
}

static int page_coordinate(int value, int scroll)
{
	int64_t result = (int64_t)value + scroll;
	if (result < INT_MIN)
		return INT_MIN;
	if (result > INT_MAX)
		return INT_MAX;
	return (int)result;
}

static int scaled_coordinate(int value, double scale)
{
	double result = value * scale;
	return result < INT_MIN ? INT_MIN : result > INT_MAX ? INT_MAX : (int)result;
}

static void request_redraw(void)
{
	if (runtime.redraw_notified)
		return;
	runtime.redraw_notified = true;
	if (runtime.ops.redraw_requested != NULL)
		runtime.ops.redraw_requested(runtime.ops.ctx);
}

static nserror window_invalidate(struct gui_window *gw,
				 const struct rect *rect)
{
	if (gw == NULL || gw != runtime.window)
		return NSERROR_BAD_PARAMETER;
	if (rect == NULL) {
		runtime.damage.count = 0;
		runtime.damage.full = true;
	} else if (!runtime.damage.full) {
		struct ov_rect area = {
			.x0 = subtract_scroll(rect->x0, gw->scroll_x),
			.y0 = subtract_scroll(rect->y0, gw->scroll_y),
			.x1 = subtract_scroll(rect->x1, gw->scroll_x),
			.y1 = subtract_scroll(rect->y1, gw->scroll_y),
		};
		ov_damage_add(&runtime.damage, area, runtime.width, runtime.height);
	}
	request_redraw();
	return NSERROR_OK;
}

static struct gui_window *window_create(struct browser_window *bw,
					struct gui_window *existing,
					gui_window_create_flags flags)
{
	if (!runtime.initialized || bw == NULL || existing != NULL ||
	    runtime.window != NULL ||
	    (flags & (GW_CREATE_CLONE | GW_CREATE_TAB)) != 0)
		return NULL;
	struct gui_window *gw = calloc(1, sizeof(*gw));
	if (gw == NULL)
		return NULL;
	gw->bw = bw;
	runtime.window = gw;
	window_invalidate(gw, NULL);
	return gw;
}

static void window_destroy(struct gui_window *gw)
{
	if (gw == NULL)
		return;
	if (runtime.window == gw) {
		input_clear();
		anchor_clear(gw);
		runtime.window = NULL;
		ov_damage_reset(&runtime.damage);
		runtime.redraw_notified = false;
	}
	free(gw);
}

static bool window_get_scroll(struct gui_window *gw, int *x, int *y)
{
	if (gw == NULL || gw != runtime.window || x == NULL || y == NULL)
		return false;
	/* Core history, fragments and script scrolling use CSS coordinates.
	 * Keep the service/display offsets in physical pixels. */
	float scale = browser_window_get_scale(gw->bw);
	*x = scaled_coordinate(gw->scroll_x, 1.0 / scale);
	*y = scaled_coordinate(gw->scroll_y, 1.0 / scale);
	return true;
}

static nserror window_set_scroll_pixels(struct gui_window *gw, const struct rect *rect)
{
	if (gw == NULL || gw != runtime.window || rect == NULL)
		return NSERROR_BAD_PARAMETER;
	int x = gw->scroll_x;
	int y = gw->scroll_y;
	int64_t visible_x1 = (int64_t)x + runtime.width;
	int64_t visible_y1 = (int64_t)y + runtime.height;
	if (rect->x0 == rect->x1) {
		x = rect->x0;
	} else if (rect->x0 < x) {
		x = rect->x0;
	} else if (rect->x1 > visible_x1) {
		x = rect->x1 - runtime.width;
	}
	if (rect->y0 == rect->y1) {
		y = rect->y0;
	} else if (rect->y0 < y) {
		y = rect->y0;
	} else if (rect->y1 > visible_y1) {
		y = rect->y1 - runtime.height;
	}
	gw->scroll_x = x > 0 ? x : 0;
	gw->scroll_y = y > 0 ? y : 0;
	if (runtime.ops.scroll_changed != NULL)
		runtime.ops.scroll_changed(runtime.ops.ctx, gw->scroll_x,
					   gw->scroll_y);
	return window_invalidate(gw, NULL);
}

static nserror window_set_scroll(struct gui_window *gw, const struct rect *rect)
{
	if (gw == NULL || gw != runtime.window || rect == NULL)
		return NSERROR_BAD_PARAMETER;
	float scale = browser_window_get_scale(gw->bw);
	struct rect pixels = {
		scaled_coordinate(rect->x0, scale), scaled_coordinate(rect->y0, scale),
		scaled_coordinate(rect->x1, scale), scaled_coordinate(rect->y1, scale)
	};
	return window_set_scroll_pixels(gw, &pixels);
}

static void window_scale(struct gui_window *gw, float scale)
{
	float old = browser_window_get_scale(gw->bw);
	if (browser_window_set_scale(gw->bw, scale, true) != NSERROR_OK) return;
	struct rect position = {0};
	position.x0 = position.x1 = scaled_coordinate(gw->scroll_x, scale / old);
	position.y0 = position.y1 = scaled_coordinate(gw->scroll_y, scale / old);
	window_set_scroll_pixels(gw, &position);
}

static void window_begin_fit(struct gui_window *gw)
{
	gw->fit_armed = runtime.manual_zoom == 0 && !gw->reading_active;
	gw->extent_pending = true;
	gw->fit_passes = 0;
	window_scale(gw, runtime.manual_zoom ? runtime.manual_zoom / 100.0f : 1.0f);
}

void ov_window_set_zoom(int percent)
{
	if(percent != 0 && (percent < 25 || percent > 300)) return;
	runtime.manual_zoom = percent;
	if(runtime.window) {anchor_capture(runtime.window);window_begin_fit(runtime.window);}
}

int ov_window_zoom(void)
{
	return (int)(browser_window_get_scale(ov_window_browser()) * 100 + 0.5f);
}

void ov_window_set_reading_enabled(bool enabled)
{
	if(runtime.reading_enabled == enabled) return;
	runtime.reading_enabled = enabled;
	if(runtime.window) {
		input_clear();
		anchor_capture(runtime.window);
		runtime.window->reading_pending = true;
		runtime.window->extent_pending = true;
	}
}

void ov_window_reading_state(bool *active, bool *available)
{
	*active = runtime.window && runtime.window->reading_active;
	*available = runtime.window && runtime.window->reading_available;
}

void ov_window_update_layout(void)
{
	struct gui_window *gw = runtime.window;
	if (!gw || !gw->extent_pending || !gw->bw->current_content ||
	    !browser_window_redraw_ready(gw->bw)) return;
	struct hlcache_handle *content = gw->bw->current_content;
	float scale = browser_window_get_scale(gw->bw);
	if (content_get_type(content) == CONTENT_HTML) {
		html_content *html = (html_content *)hlcache_handle_get_content(content);
		/* Scale/resize can schedule both reformat and asynchronous media-query
		 * rebuild. Never measure the old tree against the new viewport. */
		if (!html->layout || html->rebuild_pending ||
		    html->base.available_width != (int)(runtime.width / scale)) return;
		if(gw->reading_pending) {
			gw->reading_pending = false;
			gw->reading_active = ov_reading_apply(content, runtime.reading_enabled,
				&gw->reading_available);
			ov_font_set_reading_wrap(gw->reading_active);
			window_begin_fit(gw);
			return;
		}
		int width, height;
		if (gw->fit_armed && gw->fit_passes < 4 &&
		    browser_window_get_extents(gw->bw, false, &width, &height) == NSERROR_OK &&
		    width > runtime.width / scale + 2) {
			float fit = (runtime.width - 1.0f) / width;
			/* Bound pathological overflow and the CSS layout allocation. */
			float minimum = runtime.width / 4096.0f;
			if (minimum < 0.25f) minimum = 0.25f;
			if (minimum > 1.0f) minimum = 1.0f;
			if (fit < minimum) fit = minimum;
			if (fit < scale - 0.0001f) {
				gw->fit_passes++;
				window_scale(gw, fit);
				return;
			}
		}
	}
	gw->extent_pending = false;
	if(gw->view_anchor) {
		struct box *box=box_for_node(gw->view_anchor);
		if(box) {
			int x,y;box_coords(box,&x,&y);
			double target=(double)y+gw->anchor_fraction*box->height;
			struct rect position={gw->scroll_x,scaled_coordinate(1,target*scale),0,0};
			position.x1=position.x0;position.y1=position.y0;
			window_set_scroll_pixels(gw,&position);
		}
		anchor_clear(gw);
	}
	int width, height;
	if (browser_window_get_extents(gw->bw, true, &width, &height) == NSERROR_OK) {
		int max_x = width > runtime.width ? width - runtime.width : 0;
		int max_y = height > runtime.height ? height - runtime.height : 0;
		struct rect position = {gw->scroll_x > max_x ? max_x : gw->scroll_x,
			gw->scroll_y > max_y ? max_y : gw->scroll_y, 0, 0};
		position.x1 = position.x0; position.y1 = position.y0;
		if (position.x0 != gw->scroll_x || position.y0 != gw->scroll_y)
			window_set_scroll_pixels(gw, &position);
	}
}

static nserror window_get_dimensions(struct gui_window *gw, int *width,
				    int *height)
{
	if (gw == NULL || gw != runtime.window || width == NULL || height == NULL)
		return NSERROR_BAD_PARAMETER;
	*width = runtime.width;
	*height = runtime.height;
	return NSERROR_OK;
}

static nserror window_event(struct gui_window *gw, enum gui_window_event event)
{
	if (gw == NULL || gw != runtime.window)
		return NSERROR_BAD_PARAMETER;
	switch (event) {
	case GW_EVENT_START_THROBBER:
		if (runtime.ops.loading_changed != NULL)
			runtime.ops.loading_changed(runtime.ops.ctx, true);
		break;
	case GW_EVENT_STOP_THROBBER:
		if (runtime.ops.loading_changed != NULL)
			runtime.ops.loading_changed(runtime.ops.ctx, false);
		break;
	case GW_EVENT_NEW_CONTENT:
		input_clear();
		anchor_clear(gw);
		gw->reading_active = false;
		ov_font_set_reading_wrap(false);
		gw->reading_available = false;
		gw->reading_pending = true;
		window_begin_fit(gw);
		window_invalidate(gw, NULL);
		break;
	case GW_EVENT_UPDATE_EXTENT:
		gw->extent_pending = true;
		break;
	default:
		break;
	}
	return NSERROR_OK;
}

static void window_set_title(struct gui_window *gw, const char *title)
{
	if (gw == runtime.window && runtime.ops.title_changed != NULL)
		runtime.ops.title_changed(runtime.ops.ctx, title != NULL ? title : "");
}

static nserror window_set_url(struct gui_window *gw, struct nsurl *url)
{
	if (gw == NULL || gw != runtime.window)
		return NSERROR_BAD_PARAMETER;
	if (runtime.ops.url_changed != NULL) {
		char *display_url = NULL;
		if (url != NULL) {
			size_t display_url_length;
			nserror error = nsurl_get(url,
						  NSURL_COMPLETE & ~NSURL_CREDENTIALS,
						  &display_url, &display_url_length);
			if (error != NSERROR_OK)
				return error;
		}
		runtime.ops.url_changed(runtime.ops.ctx,
					url != NULL ? display_url : "");
		free(display_url);
	}
	return NSERROR_OK;
}

static void window_set_status(struct gui_window *gw, const char *status)
{
	if (gw == runtime.window && runtime.ops.status_changed != NULL)
		runtime.ops.status_changed(runtime.ops.ctx,
					   status != NULL ? status : "");
}

static struct gui_window_table window_table = {
	.create = window_create,
	.destroy = window_destroy,
	.invalidate = window_invalidate,
	.get_scroll = window_get_scroll,
	.set_scroll = window_set_scroll,
	.get_dimensions = window_get_dimensions,
	.event = window_event,
	.set_title = window_set_title,
	.set_url = window_set_url,
	.set_status = window_set_status,
};

nserror ov_window_init(int width, int height,
		      const struct ov_window_ops *ops)
{
	if (width <= 0 || height <= 0 || width > UINT16_MAX ||
	    height > UINT16_MAX || runtime.initialized)
		return NSERROR_BAD_PARAMETER;
	if ((size_t)height > SIZE_MAX / (size_t)width / sizeof(uint16_t) ||
	    (size_t)width * (size_t)height > UINT32_MAX / sizeof(uint16_t))
		return NSERROR_BAD_SIZE;
	memset(&runtime, 0, sizeof(runtime));
	runtime.width = width;
	runtime.height = height;
	if (ops != NULL)
		runtime.ops = *ops;
	ov_damage_reset(&runtime.damage);
	runtime.images_visible = true;
	runtime.reading_enabled = true;
	ov_font_set_reading_wrap(false);
	nsw_content_set_images_visible(true);
	runtime.initialized = true;
	return NSERROR_OK;
}

nserror ov_window_fini(void)
{
	if (runtime.window != NULL)
		return NSERROR_INIT_FAILED;
	nsw_content_set_images_visible(true);
	memset(&runtime, 0, sizeof(runtime));
	ov_font_set_reading_wrap(false);
	return NSERROR_OK;
}

void ov_window_set_images_visible(bool visible)
{
	if (!runtime.initialized || runtime.images_visible == visible) return;
	runtime.images_visible = visible;
	nsw_content_set_images_visible(visible);
	if (runtime.window != NULL) window_invalidate(runtime.window, NULL);
}

nserror ov_window_resize(int width, int height)
{
	if (!runtime.initialized || width <= 0 || height <= 0 ||
	    width > UINT16_MAX || height > UINT16_MAX)
		return NSERROR_BAD_PARAMETER;
	if ((size_t)height > SIZE_MAX / (size_t)width / sizeof(uint16_t) ||
	    (size_t)width * (size_t)height > UINT32_MAX / sizeof(uint16_t))
		return NSERROR_BAD_SIZE;
	bool changed = runtime.width != width || runtime.height != height;
	if(changed && runtime.window) anchor_capture(runtime.window);
	runtime.width = width;
	runtime.height = height;
	if (runtime.window != NULL) {
		if (changed) window_begin_fit(runtime.window);
		/* Root dimensions come from get_dimensions, not set_dimensions (which
		 * only accepts core-managed child windows). */
		browser_window_schedule_reformat(runtime.window->bw);
		window_invalidate(runtime.window, NULL);
	}
	return NSERROR_OK;
}

static bool scroll_at(struct gui_window *gw, int x, int y, int dx, int dy)
{
	if (browser_window_scroll_at_point(gw->bw,
		page_coordinate(x, gw->scroll_x), page_coordinate(y, gw->scroll_y),
		scaled_coordinate(dx, 1.0 / browser_window_get_scale(gw->bw)),
		scaled_coordinate(dy, 1.0 / browser_window_get_scale(gw->bw))))
		return true;
	int width, height;
	if (browser_window_get_extents(gw->bw, true, &width, &height) != NSERROR_OK)
		return false;
	int max_x = width > runtime.width ? width - runtime.width : 0;
	int max_y = height > runtime.height ? height - runtime.height : 0;
	int64_t nx = (int64_t)gw->scroll_x + dx, ny = (int64_t)gw->scroll_y + dy;
	int sx = nx < 0 ? 0 : nx > max_x ? max_x : (int)nx;
	int sy = ny < 0 ? 0 : ny > max_y ? max_y : (int)ny;
	if (sx == gw->scroll_x && sy == gw->scroll_y) return true;
	struct rect rect = {sx, sy, sx, sy};
	return window_set_scroll_pixels(gw, &rect) == NSERROR_OK;
}

bool ov_window_scroll_by(int delta_x, int delta_y)
{
	struct gui_window *gw = runtime.window;
	if (gw == NULL)
		return false;
	gw->fit_armed = false;
	anchor_clear(gw);
	int x = runtime.width / 2;
	int y = runtime.height / 2;
	return scroll_at(gw, x, y, delta_x, delta_y);
}

struct gui_window_table *ov_window_table(void)
{
	return &window_table;
}

struct browser_window *ov_window_browser(void)
{
	return runtime.window != NULL ? runtime.window->bw : NULL;
}

bool ov_window_is_error_page(void)
{
	struct browser_window *bw = ov_window_browser();
	if (!bw || !bw->internal_nav || !bw->current_content) return false;
	struct nsurl *url = hlcache_handle_get_url(bw->current_content);
	return nsurl_compare(url, corestring_nsurl_about_query_timeout, NSURL_COMPLETE) ||
		nsurl_compare(url, corestring_nsurl_about_query_fetcherror, NSURL_COMPLETE) ||
		nsurl_compare(url, corestring_nsurl_about_query_ssl, NSURL_COMPLETE) ||
		nsurl_compare(url, corestring_nsurl_about_query_auth, NSURL_COMPLETE);
}

bool ov_window_take_damage(struct ov_damage *damage)
{
	if (damage == NULL || ov_damage_is_empty(&runtime.damage))
		return false;
	*damage = runtime.damage;
	ov_damage_reset(&runtime.damage);
	runtime.redraw_notified = false;
	return true;
}

static void touch_cancel(struct gui_window *gw)
{
	if (!gw->touch_down)
		return;
	browser_window_mouse_track(gw->bw, BROWSER_MOUSE_LEAVE,
				   page_coordinate(gw->last_x, gw->scroll_x),
				   page_coordinate(gw->last_y, gw->scroll_y));
	gw->touch_down = false;
	gw->drag_started = false;
	gw->scroll_started = false;
}

static int touch_coordinate(int value, int extent)
{
	if (value < 0)
		return 0;
	if (value >= extent)
		return extent - 1;
	return value;
}

static nserror dispatch_touch(struct gui_window *gw,
			     const struct ov_event *event)
{
	int x = touch_coordinate(event->payload.touch.x, runtime.width);
	int y = touch_coordinate(event->payload.touch.y, runtime.height);
	int document_x = page_coordinate(x, gw->scroll_x);
	int document_y = page_coordinate(y, gw->scroll_y);
	int previous_y = gw->last_y;
	switch (event->type) {
	case OV_EVENT_TOUCH_DOWN:
		/* Do not change the scale underneath a finger or a reading position. */
		gw->fit_armed = false;
		anchor_clear(gw);
		touch_cancel(gw);
		gw->last_x = x;
		gw->last_y = y;
		gw->touch_down = true;
		gw->drag_started = false;
		gw->scroll_started = false;
		gw->press_x = x;
		gw->press_y = y;
		browser_window_mouse_click(gw->bw, BROWSER_MOUSE_PRESS_1,
					   document_x, document_y);
		break;
	case OV_EVENT_TOUCH_MOVE:
		gw->last_x = x;
		gw->last_y = y;
		if (!gw->touch_down) {
			browser_window_mouse_track(gw->bw, BROWSER_MOUSE_HOVER,
						   document_x, document_y);
			break;
		}
		if (!gw->drag_started && !gw->scroll_started &&
		    (abs(x - gw->press_x) > 5 || abs(y - gw->press_y) > 5)) {
			if (abs(y - gw->press_y) >= abs(x - gw->press_x)) {
				browser_window_mouse_track(gw->bw, BROWSER_MOUSE_LEAVE,
							  page_coordinate(gw->press_x, gw->scroll_x),
							  page_coordinate(gw->press_y, gw->scroll_y));
				gw->scroll_started = true;
				/* No scroll has been applied before crossing the threshold. */
				previous_y = gw->press_y;
			} else {
				browser_window_mouse_click(gw->bw, BROWSER_MOUSE_DRAG_1,
							   page_coordinate(gw->press_x, gw->scroll_x),
							   page_coordinate(gw->press_y, gw->scroll_y));
				gw->drag_started = true;
			}
		}
		if (gw->scroll_started) {
			scroll_at(gw, x, y, 0, previous_y - y);
			break;
		}
		browser_window_mouse_track(gw->bw,
					   BROWSER_MOUSE_HOLDING_1 |
					   (gw->drag_started ?
					    BROWSER_MOUSE_DRAG_ON : 0),
					   document_x, document_y);
		break;
	case OV_EVENT_TOUCH_UP:
		/* The input device may deliver its final position only with UP. */
		if (gw->touch_down && (x != gw->last_x || y != gw->last_y)) {
			struct ov_event move = *event;
			move.type = OV_EVENT_TOUCH_MOVE;
			dispatch_touch(gw, &move);
			document_x = page_coordinate(x, gw->scroll_x);
			document_y = page_coordinate(y, gw->scroll_y);
		}
		gw->last_x = x;
		gw->last_y = y;
		if (gw->touch_down) {
			if (gw->drag_started || gw->scroll_started) {
				browser_window_mouse_track(gw->bw, BROWSER_MOUSE_HOVER,
							   document_x, document_y);
			} else {
				browser_window_mouse_click(gw->bw,
							   BROWSER_MOUSE_CLICK_1,
							   document_x, document_y);
				input_request(document_x, document_y);
			}
			gw->touch_down = false;
			gw->drag_started = false;
			gw->scroll_started = false;
		} else {
			browser_window_mouse_track(gw->bw, BROWSER_MOUSE_HOVER,
						   document_x, document_y);
		}
		break;
	case OV_EVENT_TOUCH_CANCEL:
		touch_cancel(gw);
		break;
	default:
		return NSERROR_NOT_IMPLEMENTED;
	}
	return NSERROR_OK;
}

static nserror reload_page(struct browser_window *bw)
{
	/* Core timeout/fetch-error documents have an internal about: URL. A plain
	 * reload lacks their multipart data and returns NEED_DATA. Reuse the core's
	 * existing retry form handler, which retains the original loading request.
	 * Never send this action to the certificate/auth query handlers. */
	struct nsurl *url = bw->current_content ? hlcache_handle_get_url(bw->current_content) : NULL;
	if (bw->internal_nav && url && bw->loading_content == NULL &&
	    (nsurl_compare(url, corestring_nsurl_about_query_timeout, NSURL_COMPLETE) ||
	     nsurl_compare(url, corestring_nsurl_about_query_fetcherror, NSURL_COMPLETE))) {
		struct fetch_multipart_data *retry = NULL;
		nserror error = fetch_multipart_data_new_kv(&retry, "retry", "1");
		if (error != NSERROR_OK) return error;
		error = browser_window_navigate(bw, url, NULL, BW_NAVIGATE_NONE,
				NULL, retry, NULL);
		fetch_multipart_data_destroy(retry);
		return error;
	}
	return browser_window_reload(bw, false);
}

nserror ov_window_dispatch(const struct ov_event *event)
{
	if (event == NULL)
		return NSERROR_BAD_PARAMETER;
	struct gui_window *gw = runtime.window;
	if (gw == NULL)
		return NSERROR_NOT_FOUND;
	if (event->type == OV_EVENT_NAVIGATE || event->type == OV_EVENT_BACK ||
	    event->type == OV_EVENT_FORWARD || event->type == OV_EVENT_RELOAD ||
	    event->type == OV_EVENT_STOP || event->type == OV_EVENT_TOUCH_DOWN)
		input_clear();
	switch (event->type) {
	case OV_EVENT_INPUT:
		return input_submit(event);
	case OV_EVENT_TOUCH_DOWN:
	case OV_EVENT_TOUCH_MOVE:
	case OV_EVENT_TOUCH_UP:
	case OV_EVENT_TOUCH_CANCEL:
		return dispatch_touch(gw, event);
	case OV_EVENT_NAVIGATE: {
		if (event->payload.navigate.url[0] == '\0' ||
		    memchr(event->payload.navigate.url, '\0',
			   sizeof(event->payload.navigate.url)) == NULL)
			return NSERROR_BAD_PARAMETER;
		struct nsurl *url = NULL;
		nserror error = nsurl_create(event->payload.navigate.url, &url);
		if (error != NSERROR_OK)
			return error;
		touch_cancel(gw);
		error = browser_window_navigate(gw->bw, url, NULL,
						BW_NAVIGATE_HISTORY,
						NULL, NULL, NULL);
		nsurl_unref(url);
		return error;
	}
	case OV_EVENT_BACK:
		touch_cancel(gw);
		return browser_window_history_back(gw->bw, false);
	case OV_EVENT_FORWARD:
		touch_cancel(gw);
		return browser_window_history_forward(gw->bw, false);
	case OV_EVENT_RELOAD:
		return reload_page(gw->bw);
	case OV_EVENT_STOP:
		browser_window_stop(gw->bw);
		return NSERROR_OK;
	case OV_EVENT_SCROLL:
		return ov_window_scroll_by(event->payload.scroll.delta_x,
					   event->payload.scroll.delta_y) ?
			NSERROR_OK : NSERROR_NOT_FOUND;
	case OV_EVENT_RESIZE:
		return ov_window_resize(event->payload.resize.width,
					event->payload.resize.height);
	default:
		return NSERROR_NOT_IMPLEMENTED;
	}
}

bool ov_window_render(struct ov_lvgl_view *view,
		      uint32_t *render_pixels, int render_stride,
		      int render_rows,
		      ov_plotter_surface_bind_fn bind_surface,
		      void *bind_ctx,
		      const struct plotter_table *plotters,
		      void *plotter_priv,
		      uint64_t serial)
{
	struct gui_window *gw = runtime.window;
	if (view == NULL || render_pixels == NULL || render_stride < runtime.width ||
	    render_rows <= 0 || bind_surface == NULL || plotters == NULL ||
	    plotter_priv == NULL || gw == NULL ||
	    (size_t)render_rows > SIZE_MAX / (size_t)render_stride / sizeof(uint32_t) ||
	    ov_lvgl_view_width(view) != runtime.width ||
	    ov_lvgl_view_height(view) != runtime.height)
		return false;
	int stride_pixels;
	uint16_t *pixels = ov_lvgl_view_begin_frame(view, &stride_pixels);
	if (pixels == NULL || stride_pixels != runtime.width) {
		if (pixels != NULL)
			ov_lvgl_view_abort_frame(view);
		return false;
	}
	ov_damage_reset(&runtime.damage);
	runtime.redraw_notified = false;
	struct redraw_context context = {
		.interactive = true,
		.background_images = true,
		.plot = plotters,
		.priv = plotter_priv,
	};
	for (int y = 0; y < runtime.height; y += render_rows) {
		int rows = runtime.height - y;
		if (rows > render_rows)
			rows = render_rows;
		for (int row = 0; row < rows; row++)
			for (int x = 0; x < runtime.width; x++)
				render_pixels[(size_t)row * render_stride + x] =
					0x00ffffffu;
		if (!bind_surface(bind_ctx, plotter_priv, render_pixels,
				  runtime.width, rows, render_stride)) {
			ov_lvgl_view_abort_frame(view);
			runtime.damage.full = true;
			return false;
		}
		struct rect clip = {0, 0, runtime.width, rows};
		int64_t origin_y = -(int64_t)gw->scroll_y - y;
		if (origin_y < INT_MIN)
			origin_y = INT_MIN;
		if (!browser_window_redraw(gw->bw, -gw->scroll_x, (int)origin_y,
					   &clip, &context)) {
			ov_lvgl_view_abort_frame(view);
			runtime.damage.full = true;
			return false;
		}
		for (int row = 0; row < rows; row++) {
			const uint32_t *src = render_pixels + (size_t)row * render_stride;
			uint16_t *dst = pixels + (size_t)(y + row) * stride_pixels;
			for (int x = 0; x < runtime.width; x++) {
				uint32_t rgb = src[x];
				uint16_t r = (uint16_t)((rgb >> 19) & 0x1fu);
				uint16_t g = (uint16_t)((rgb >> 10) & 0x3fu);
				uint16_t b = (uint16_t)((rgb >> 3) & 0x1fu);
				dst[x] = (uint16_t)((r << 11) | (g << 5) | b);
			}
		}
	}
	struct ov_damage damage = {.full = true};
	if (!ov_lvgl_view_present(view, &damage, serial)) {
		ov_lvgl_view_abort_frame(view);
		runtime.damage.full = true;
		return false;
	}
	return true;
}
