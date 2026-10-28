/* SPDX-License-Identifier: GPL-2.0-only */
#include "openvela_qpk_netsurf.h"

#include <string.h>

static const struct ov_qpk_netsurf_ops *active_ops;

static JSValue js_bool_result(JSContext *context, bool result)
{
	return JS_NewBool(context, result);
}

static JSValue js_navigate(JSContext *context, JSValueConst this_value,
			  int argc, JSValueConst *argv)
{
	(void)this_value;
	if (active_ops == NULL || active_ops->navigate == NULL || argc < 1 ||
	    !JS_IsString(argv[0]))
		return js_bool_result(context, false);
	const char *url = JS_ToCString(context, argv[0]);
	if (url == NULL)
		return JS_EXCEPTION;
	bool valid = strlen(url) < OV_QPK_TEXT_MAX;
	bool result = valid && active_ops->navigate(active_ops->ctx, url);
	JS_FreeCString(context, url);
	return js_bool_result(context, result);
}

#define NAV_FUNCTION(name, member) \
	static JSValue js_##name(JSContext *context, JSValueConst this_value, \
				 int argc, JSValueConst *argv) \
	{ \
		(void)this_value; (void)argc; (void)argv; \
		return js_bool_result(context, active_ops != NULL && \
			active_ops->member != NULL && \
			active_ops->member(active_ops->ctx)); \
	}

NAV_FUNCTION(back, back)
NAV_FUNCTION(forward, forward)
NAV_FUNCTION(reload, reload)
NAV_FUNCTION(stop, stop)

static JSValue js_set_viewport(JSContext *context, JSValueConst this_value,
			       int argc, JSValueConst *argv)
{
	(void)this_value;
	int32_t x, y, width, height;
	if (active_ops == NULL || active_ops->set_viewport == NULL || argc < 4 ||
	    JS_ToInt32(context, &x, argv[0]) < 0 ||
	    JS_ToInt32(context, &y, argv[1]) < 0 ||
	    JS_ToInt32(context, &width, argv[2]) < 0 ||
	    JS_ToInt32(context, &height, argv[3]) < 0 ||
	    x < 0 || y < 0 || width <= 0 || height <= 0)
		return js_bool_result(context, false);
	return js_bool_result(context, active_ops->set_viewport(active_ops->ctx,
								 x, y, width, height));
}

static JSValue js_pointer(JSContext *context, JSValueConst this_value,
			  int argc, JSValueConst *argv)
{
	(void)this_value;
	if (active_ops == NULL || active_ops->pointer == NULL || argc < 3)
		return js_bool_result(context, false);
	const char *name = JS_ToCString(context, argv[0]);
	if (name == NULL)
		return JS_EXCEPTION;
	enum ov_qpk_pointer_state state;
	bool valid = true;
	if (strcmp(name, "down") == 0) state = OV_QPK_POINTER_DOWN;
	else if (strcmp(name, "move") == 0) state = OV_QPK_POINTER_MOVE;
	else if (strcmp(name, "up") == 0) state = OV_QPK_POINTER_UP;
	else if (strcmp(name, "cancel") == 0) state = OV_QPK_POINTER_CANCEL;
	else valid = false;
	JS_FreeCString(context, name);
	int32_t x, y;
	if (!valid || JS_ToInt32(context, &x, argv[1]) < 0 ||
	    JS_ToInt32(context, &y, argv[2]) < 0 || x < 0 || y < 0)
		return js_bool_result(context, false);
	return js_bool_result(context,
		active_ops->pointer(active_ops->ctx, state, x, y));
}

static JSValue js_scroll_by(JSContext *context, JSValueConst this_value,
			    int argc, JSValueConst *argv)
{
	(void)this_value;
	int32_t dx, dy;
	if (active_ops == NULL || active_ops->scroll_by == NULL || argc < 2 ||
	    JS_ToInt32(context, &dx, argv[0]) < 0 ||
	    JS_ToInt32(context, &dy, argv[1]) < 0)
		return js_bool_result(context, false);
	return js_bool_result(context, active_ops->scroll_by(active_ops->ctx, dx, dy));
}

static JSValue js_set_images_visible(JSContext *context, JSValueConst this_value,
				     int argc, JSValueConst *argv)
{
	(void)this_value;
	if (!active_ops || !active_ops->set_images_visible || argc < 1 ||
	    !JS_IsBool(argv[0]))
		return js_bool_result(context, false);
	return js_bool_result(context, active_ops->set_images_visible(
		active_ops->ctx, JS_VALUE_GET_BOOL(argv[0]) != 0));
}

static JSValue js_set_zoom(JSContext *context, JSValueConst this_value,
			 int argc, JSValueConst *argv)
{
	(void)this_value;
	if(!active_ops || !active_ops->set_zoom || argc<1 || JS_VALUE_GET_TAG(argv[0])!=JS_TAG_INT)
		return js_bool_result(context,false);
	return js_bool_result(context,active_ops->set_zoom(active_ops->ctx,JS_VALUE_GET_INT(argv[0])));
}

static JSValue js_set_reading_enabled(JSContext *context, JSValueConst this_value,
				    int argc, JSValueConst *argv)
{
	(void)this_value;
	if(!active_ops || !active_ops->set_reading_enabled || argc<1 || !JS_IsBool(argv[0]))
		return js_bool_result(context,false);
	return js_bool_result(context,active_ops->set_reading_enabled(active_ops->ctx,JS_VALUE_GET_BOOL(argv[0])!=0));
}

static void set_status_string(JSContext *context, JSValue object,
			      const char *name, const char *value)
{
	JS_SetPropertyStr(context, object, name,
			  JS_NewString(context, value != NULL ? value : ""));
}

static JSValue js_status(JSContext *context, JSValueConst this_value,
			 int argc, JSValueConst *argv)
{
	(void)this_value; (void)argc; (void)argv;
	JSValue object = JS_NewObject(context);
	if (JS_IsException(object))
		return object;
	struct ov_qpk_netsurf_status status = {0};
	if (active_ops != NULL && active_ops->status != NULL)
		status.available = active_ops->status(active_ops->ctx, &status);
	status.url[sizeof(status.url) - 1] = '\0';
	status.title[sizeof(status.title) - 1] = '\0';
	status.message[sizeof(status.message) - 1] = '\0';
	JS_SetPropertyStr(context, object, "available", JS_NewBool(context, status.available));
	JS_SetPropertyStr(context, object, "loading", JS_NewBool(context, status.loading));
	JS_SetPropertyStr(context, object, "errorPage", JS_NewBool(context, status.error_page));
	JS_SetPropertyStr(context, object, "imagesVisible", JS_NewBool(context, status.images_visible));
	JS_SetPropertyStr(context, object, "readingActive", JS_NewBool(context, status.reading_active));
	JS_SetPropertyStr(context, object, "readingAvailable", JS_NewBool(context, status.reading_available));
	JS_SetPropertyStr(context, object, "zoomPercent", JS_NewInt32(context, status.zoom_percent));
	JS_SetPropertyStr(context, object, "canBack", JS_NewBool(context, status.can_back));
	JS_SetPropertyStr(context, object, "canForward", JS_NewBool(context, status.can_forward));
	JS_SetPropertyStr(context, object, "scrollX", JS_NewInt32(context, status.scroll_x));
	JS_SetPropertyStr(context, object, "scrollY", JS_NewInt32(context, status.scroll_y));
	set_status_string(context, object, "url", status.url);
	set_status_string(context, object, "title", status.title);
	set_status_string(context, object, "message", status.message);
	return object;
}

static JSValue js_take_input(JSContext *context, JSValueConst this_value,
			    int argc, JSValueConst *argv)
{
	(void)this_value; (void)argc; (void)argv;
	struct ov_input_request request = {0};
	if (!active_ops || !active_ops->take_input ||
	    !active_ops->take_input(active_ops->ctx, &request)) return JS_NULL;
	JSValue object = JS_NewObject(context);
	if (JS_IsException(object)) {
		if (active_ops->submit_input) active_ops->submit_input(active_ops->ctx, request.id, NULL);
		return object;
	}
	request.value[sizeof(request.value) - 1] = 0;
	JS_SetPropertyStr(context, object, "id", JS_NewInt32(context, request.id));
	JS_SetPropertyStr(context, object, "maxLength", JS_NewInt32(context, request.max_length));
	JS_SetPropertyStr(context, object, "password", JS_NewBool(context, request.password));
	set_status_string(context, object, "value", request.password ? "" : request.value);
	return object;
}

static JSValue js_submit_input(JSContext *context, JSValueConst this_value,
			      int argc, JSValueConst *argv)
{
	(void)this_value;
	int32_t id;
	if (!active_ops || !active_ops->submit_input || argc < 2 ||
	    JS_ToInt32(context, &id, argv[0]) < 0 || id <= 0)
		return js_bool_result(context, false);
	if (JS_IsNull(argv[1]))
		return js_bool_result(context, active_ops->submit_input(active_ops->ctx, id, NULL));
	if (!JS_IsString(argv[1])) return js_bool_result(context, false);
	size_t size = 0;
	const char *text = JS_ToCStringLen(context, &size, argv[1]);
	if (!text) return JS_EXCEPTION;
	bool result = size < OV_INPUT_TEXT_MAX && !memchr(text, 0, size) &&
		active_ops->submit_input(active_ops->ctx, id, text);
	JS_FreeCString(context, text);
	return js_bool_result(context, result);
}

bool ov_qpk_netsurf_bind(JSContext *context, JSValue system,
			 const struct ov_qpk_netsurf_ops *ops)
{
	if (context == NULL || ops == NULL || active_ops != NULL)
		return false;
	JSValue api = JS_NewObject(context);
	if (JS_IsException(api))
		return false;
	JS_SetPropertyStr(context, api, "navigate",
			  JS_NewCFunction(context, js_navigate, "navigate", 1));
	JS_SetPropertyStr(context, api, "back", JS_NewCFunction(context, js_back, "back", 0));
	JS_SetPropertyStr(context, api, "forward", JS_NewCFunction(context, js_forward, "forward", 0));
	JS_SetPropertyStr(context, api, "reload", JS_NewCFunction(context, js_reload, "reload", 0));
	JS_SetPropertyStr(context, api, "stop", JS_NewCFunction(context, js_stop, "stop", 0));
	JS_SetPropertyStr(context, api, "setViewport",
			  JS_NewCFunction(context, js_set_viewport, "setViewport", 4));
	JS_SetPropertyStr(context, api, "pointer",
			  JS_NewCFunction(context, js_pointer, "pointer", 3));
	JS_SetPropertyStr(context, api, "scrollBy",
			  JS_NewCFunction(context, js_scroll_by, "scrollBy", 2));
	JS_SetPropertyStr(context, api, "status",
			  JS_NewCFunction(context, js_status, "status", 0));
	JS_SetPropertyStr(context, api, "takeInput",
			  JS_NewCFunction(context, js_take_input, "takeInput", 0));
	JS_SetPropertyStr(context, api, "submitInput",
			  JS_NewCFunction(context, js_submit_input, "submitInput", 2));
	JS_SetPropertyStr(context, api, "setImagesVisible",
			  JS_NewCFunction(context, js_set_images_visible, "setImagesVisible", 1));
	JS_SetPropertyStr(context, api, "setZoom",
			  JS_NewCFunction(context, js_set_zoom, "setZoom", 1));
	JS_SetPropertyStr(context, api, "setReadingEnabled",
			  JS_NewCFunction(context, js_set_reading_enabled, "setReadingEnabled", 1));
	active_ops = ops;
	if (JS_SetPropertyStr(context, system, "netsurf", api) < 0) {
		active_ops = NULL;
		return false;
	}
	return true;
}

void ov_qpk_netsurf_unbind(const struct ov_qpk_netsurf_ops *ops)
{
	if (active_ops == ops)
		active_ops = NULL;
}
