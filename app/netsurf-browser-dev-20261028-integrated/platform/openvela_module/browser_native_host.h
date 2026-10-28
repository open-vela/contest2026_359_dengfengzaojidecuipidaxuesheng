/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef OV_BROWSER_NATIVE_HOST_H
#define OV_BROWSER_NATIVE_HOST_H
#include "browser_module.h"
#include <stddef.h>

struct ov_browser_loader {
	void *(*open)(const char *path);
	ov_browser_module_query_fn (*query)(void *handle);
	void (*close)(void *handle);
};
struct ov_browser_host {
	void *handle;
	const struct ov_browser_module *api;
	bool attached;
	char error[160];
};
/* Load on a worker, then publish completion before accessing host on the UI.
 * No JS or LVGL calls are made by load. */
bool ov_browser_host_load(struct ov_browser_host *, const struct ov_browser_loader *,
	const char *host_abi, const char *package, const char *entry);
/* UI thread only. A successfully validated module remains loaded until reboot:
 * its core and HTTP threads outlive a single app page. Never unload their code.
 */
bool ov_browser_host_attach(struct ov_browser_host *, const struct ov_browser_loader *,
	const char *host_abi, JSContext *, JSValue, lv_obj_t *, const char *package,
	const char *entry);
void ov_browser_host_detach(struct ov_browser_host *);
bool ov_browser_native_install(JSContext *, JSValue, lv_obj_t *, const char *, const char *);
void ov_browser_native_stop(void);
#endif
