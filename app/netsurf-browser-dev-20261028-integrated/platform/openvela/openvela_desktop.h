/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NETSURF_OPENVELA_DESKTOP_H
#define NETSURF_OPENVELA_DESKTOP_H
#include <lvgl/lvgl.h>
#include <quickjs.h>
/* LVGL thread only. Installs a capability only for com.netsurf.browser.
 * The persistent core owns all NetSurf calls; stop detaches the page before
 * its QPK root is deleted. A new launch can reuse the process-global core. */
bool ov_desktop_netsurf_install(JSContext *context, JSValue system,
			      lv_obj_t *root, const char *package);
void ov_desktop_netsurf_stop(void);
#endif
