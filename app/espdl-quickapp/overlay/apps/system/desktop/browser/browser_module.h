/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef OV_BROWSER_MODULE_H
#define OV_BROWSER_MODULE_H
#include <stdbool.h>
#include <stdint.h>
#include <lvgl/lvgl.h>
#include <quickjs.h>

#define OV_BROWSER_MODULE_ABI 1u
/* This is a native code ABI, not a webpage capability. */
struct ov_browser_module {
	uint32_t abi, bytes;
	const char *host_abi;
	bool (*attach)(JSContext *, JSValue, lv_obj_t *, const char *);
	void (*detach)(void);
};
typedef const struct ov_browser_module *(*ov_browser_module_query_fn)(void);
const struct ov_browser_module *ov_browser_module_query(void);
#endif
